//! Reuse the Dashboard's session owner and SteamVR launcher for explicit Frame requests.
use surfxr_common::{RelaxedAtomic,anyhow::Result,error};
#[path="frame_start_owner.rs"] mod owner;
use owner::{Owner,STARTING,READY};
pub(crate) use owner::RuntimeEvidence;
use surfxr_sockets::{CONTROL_PORT, frame_discovery_reply, frame_start_hostname, frame_start_reply,frame_start_status_hostname,frame_start_status_reply};
use std::{
    net::{IpAddr, Ipv4Addr, UdpSocket},
    sync::Arc,
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

pub enum StartDecision {
    Ready,
    Launch,
    Untrusted,
}

pub fn spawn(
    running: Arc<RelaxedAtomic>,
    authorize: impl FnMut(&str, IpAddr) -> StartDecision + Send + 'static,
    trusted_status:impl FnMut(&str,IpAddr)->bool+Send+'static,
    probe:impl Fn()->RuntimeEvidence+Clone+Send+'static,
) -> JoinHandle<()> {
    thread::spawn(move || {
        serve(running,None,authorize,trusted_status,||crate::steamvr_launcher::LAUNCHER.lock().launch_frame_steamvr(),probe)
    })
}

fn serve(
    running: Arc<RelaxedAtomic>,
    mut socket: Option<UdpSocket>,
    mut authorize: impl FnMut(&str, IpAddr) -> StartDecision,
    mut trusted_status:impl FnMut(&str,IpAddr)->bool,
    launch: impl Fn()->Result<()>+Clone+Send+'static,
    probe: impl Fn()->RuntimeEvidence+Clone+Send+'static,
) {
    let mut next_bind = Instant::now();
    let mut query = [0u8; 128];
    let mut owner=Owner::new();let mut job:Option<(u64,JoinHandle<Result<()>>)>=None;
    let mut next_probe=Instant::now();let mut probe_job:Option<(u64,JoinHandle<RuntimeEvidence>)>=None;
    let mut refresh_explicit:Option<[u8;8]>=None;let mut last_launch_nonce:Option<[u8;8]>=None;
    while running.value() {
        owner.tick(Instant::now());
        if job.as_ref().is_some_and(|(_,job)|job.is_finished()) {
            let(attempt,completed)=job.take().unwrap();
            match completed.join() {
                Ok(Ok(()))=>{},Ok(Err(error))=>{error!("Frame SteamVR startup failed: {error}");owner.launch_failed_for(attempt);},
                Err(_)=>owner.launch_failed_for(attempt),
            }
        }
        if probe_job.as_ref().is_some_and(|(_,job)|job.is_finished()) {
            let(attempt,completed)=probe_job.take().unwrap();
            if let Ok(evidence)=completed.join(){
                owner.observe_for(attempt,Instant::now(),evidence,job.is_some());
                // A stale cached Ready is refreshed before any new launch. Only
                // an explicit accepted start request can authorize this branch.
                if refresh_explicit.is_some()&&attempt==owner.snapshot.attempt {
                    let nonce=refresh_explicit.take().unwrap();
                    if !evidence.driver_ready&&!evidence.pending&&job.is_none()&&
                        last_launch_nonce!=Some(nonce)&&owner.begin(Instant::now()){
                        last_launch_nonce=Some(nonce);let launch=launch.clone();job=Some((owner.snapshot.attempt,thread::spawn(launch)));
                    }
                }
            }
        }
        if owner.snapshot.phase==STARTING&&probe_job.is_none()&&Instant::now()>=next_probe {
            let probe=probe.clone();probe_job=Some((owner.snapshot.attempt,thread::spawn(probe)));next_probe=Instant::now()+Duration::from_millis(250);
        }
        if socket.is_none() && Instant::now() >= next_bind {
            socket = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, CONTROL_PORT))
                .ok()
                .and_then(|s| {
                    s.set_nonblocking(true).ok()?;
                    Some(s)
                });
            next_bind = Instant::now() + Duration::from_secs(1);
        }
        if let Some(socket) = &socket {
            for _ in 0..16 {
                let Ok((size, peer)) = socket.recv_from(&mut query) else {
                    break;
                };
                let bytes = &query[..size];
                if let Some(reply) = frame_discovery_reply(bytes, CONTROL_PORT) {
                    socket.send_to(&reply, peer).ok();
                    continue;
                }
                if let Some(hostname)=frame_start_status_hostname(bytes) {
                    let trusted=trusted_status(hostname,peer.ip());
                    if trusted&&probe_job.is_none()&&Instant::now()>=next_probe {
                        let probe=probe.clone();probe_job=Some((owner.snapshot.attempt,thread::spawn(probe)));next_probe=Instant::now()+Duration::from_millis(250);
                    }
                    let status=owner.snapshot;
                    let reply=frame_start_status_reply(bytes,if trusted{status.phase}else{3},if trusted{status.reason}else{1},if trusted{status.attempt}else{0}).unwrap();
                    socket.send_to(&reply,peer).ok();continue;
                }
                if let Some(hostname) = frame_start_hostname(bytes) {
                    let nonce:[u8;8]=bytes[8..16].try_into().unwrap();
                    let decision=authorize(hostname,peer.ip());
                    let accepted=!matches!(decision,StartDecision::Untrusted);
                    if accepted {
                        if owner.snapshot.phase==READY {
                            if matches!(decision,StartDecision::Launch)||trusted_status(hostname,peer.ip()){refresh_explicit=Some(nonce);}
                            owner.refresh_ready(Instant::now());
                            if probe_job.is_none(){let probe=probe.clone();probe_job=Some((owner.snapshot.attempt,thread::spawn(probe)));}
                        }else if matches!(decision,StartDecision::Launch)&&job.is_none()&&
                            last_launch_nonce!=Some(nonce)&&owner.begin(Instant::now()) {
                            last_launch_nonce=Some(nonce);let launch=launch.clone();
                            job=Some((owner.snapshot.attempt,thread::spawn(launch)));
                        }else if matches!(decision,StartDecision::Ready)&&job.is_none()&&last_launch_nonce!=Some(nonce)&&owner.begin(Instant::now()){
                            last_launch_nonce=Some(nonce);
                        }
                    }
                    // A successful legacy ACK follows state publication, so a
                    // new status consumer cannot observe an idle launch owner.
                    socket.send_to(&frame_start_reply(bytes,accepted).unwrap(),peer).ok();
                }
            }
        }
        thread::sleep(Duration::from_millis(20));
    }
    // The Dashboard datasource owns these jobs just as it owned the former
    // synchronous launcher. Shutdown never detaches configuration/launch work.
    if let Some((_,probe))=probe_job{probe.join().ok();}
    if let Some((_,launch))=job{launch.join().ok();}
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

    #[test]
    fn status_queries_are_read_only_and_reply_while_launch_and_probe_are_blocked(){
        let socket=UdpSocket::bind((Ipv4Addr::LOCALHOST,0)).unwrap();socket.set_nonblocking(true).unwrap();
        let client=UdpSocket::bind((Ipv4Addr::LOCALHOST,0)).unwrap();client.connect(socket.local_addr().unwrap()).unwrap();
        client.set_read_timeout(Some(Duration::from_secs(1))).unwrap();
        let running=Arc::new(RelaxedAtomic::new(true));let released=Arc::new(AtomicBool::new(false));
        let starts=Arc::new(AtomicUsize::new(0));let trusted=Arc::new(AtomicBool::new(true));
        let ready=Arc::new(AtomicBool::new(false));let launches=Arc::new(AtomicUsize::new(0));
        let worker=thread::spawn({
            let running=running.clone();let released= released.clone();let starts=starts.clone();let trusted=trusted.clone();
            let probe_release=released.clone();
            let ready=ready.clone();let launches=launches.clone();
            move||serve(running,Some(socket),move|_,_|{starts.fetch_add(1,Ordering::Relaxed);StartDecision::Launch},
                move|_,_|trusted.load(Ordering::Relaxed),
                move||{launches.fetch_add(1,Ordering::Relaxed);while !released.load(Ordering::Relaxed){thread::sleep(Duration::from_millis(2));}Ok(())},
                move||{while !probe_release.load(Ordering::Relaxed){thread::sleep(Duration::from_millis(2));}
                    RuntimeEvidence{driver_ready:ready.load(Ordering::Relaxed),process:Some((1,2)),pending:false}})
        });
        struct Worker(Arc<RelaxedAtomic>,Arc<AtomicBool>,Option<JoinHandle<()>>);
        impl Drop for Worker{fn drop(&mut self){self.1.store(true,Ordering::Relaxed);self.0.set(false);self.2.take().unwrap().join().unwrap();}}
        let _worker=Worker(running,released.clone(),Some(worker));
        let mut query=b"ALVRFS01abcdefgh\x0aframe-test".to_vec();let mut reply=[0;128];
        client.send(&query).unwrap();let size=client.recv(&mut reply).unwrap();assert_eq!(&reply[..size],&frame_start_reply(&query,true).unwrap());
        query[..8].copy_from_slice(b"ALVRFQ01");
        for _ in 0..2 {
            client.send(&query).unwrap();let size=client.recv(&mut reply).unwrap();
            assert_eq!(&reply[..size],&frame_start_status_reply(&query,1,0,1).unwrap());
        }
        assert_eq!(starts.load(Ordering::Relaxed),1,"Status must not register trust or restart SteamVR");
        trusted.store(false,Ordering::Relaxed);client.send(&query).unwrap();let size=client.recv(&mut reply).unwrap();
        assert_eq!(&reply[..size],&frame_start_status_reply(&query,3,1,0).unwrap());
        assert_eq!(starts.load(Ordering::Relaxed),1);
        trusted.store(true,Ordering::Relaxed);ready.store(true,Ordering::Relaxed);released.store(true,Ordering::Relaxed);
        let await_ready=||{
            let mut observed=[0;128];let deadline=Instant::now()+Duration::from_secs(1);
            loop{client.send(&query).unwrap();let size=client.recv(&mut observed).unwrap();
                if observed[17]==2{assert_eq!(&observed[..size],&frame_start_status_reply(&query,2,0,1).unwrap());break;}
                assert!(Instant::now()<deadline);}
        };
        await_ready();
        // A subsequent explicit start refreshes cached readiness before ACK,
        // while keeping the attempt and avoiding a second launcher invocation.
        released.store(false,Ordering::Relaxed);
        let mut request=query.clone();request[..8].copy_from_slice(b"ALVRFS01");
        client.send(&request).unwrap();client.recv(&mut reply).unwrap();
        client.send(&query).unwrap();let size=client.recv(&mut reply).unwrap();
        assert_eq!(&reply[..size],&frame_start_status_reply(&query,1,0,1).unwrap());
        released.store(true,Ordering::Relaxed);await_ready();assert_eq!(launches.load(Ordering::Relaxed),1);
    }

    #[test]
    fn discovery_does_not_start_and_only_trusted_explicit_requests_launch_once() {
        // Reserve a private endpoint. Never invoke the real SteamVR launcher.
        let socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        socket.set_nonblocking(true).unwrap();
        let peer = socket.local_addr().unwrap();
        let client = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        client.connect(peer).unwrap();
        client
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let running = Arc::new(RelaxedAtomic::new(true));
        let trusted = Arc::new(AtomicBool::new(false));
        let launches = Arc::new(AtomicUsize::new(0));
        let thread = thread::spawn({
            let running = Arc::clone(&running);
            let trusted = Arc::clone(&trusted);
            let launches = Arc::clone(&launches);
            move || {
                serve(
                    running,
                    Some(socket),
                    move |name, _| {
                        assert_eq!(name, "frame-test");
                        if trusted.load(Ordering::Relaxed) {
                            StartDecision::Launch
                        } else {
                            StartDecision::Untrusted
                        }
                    },
                    |_,_|true,
                    move || {
                        launches.fetch_add(1, Ordering::Relaxed);Err(surfxr_common::anyhow::anyhow!("fixture launch failure"))
                    },
                    RuntimeEvidence::default,
                )
            }
        });
        struct Worker(Arc<RelaxedAtomic>, Option<JoinHandle<()>>);
        impl Drop for Worker {
            fn drop(&mut self) {
                self.0.set(false);
                self.1.take().unwrap().join().unwrap();
            }
        }
        let _worker = Worker(running, Some(thread));
        let mut query = b"ALVRFD01abcdefgh".to_vec();
        let mut reply = [0; 128];
        client.send(&query).unwrap();
        let size = client.recv(&mut reply).unwrap();
        assert_eq!(
            &reply[..size],
            frame_discovery_reply(&query, CONTROL_PORT).unwrap()
        );
        assert_eq!(launches.load(Ordering::Relaxed), 0);
        query[..8].copy_from_slice(b"ALVRFS01");
        query.push(10);
        query.extend_from_slice(b"frame-test");
        client.send(&query).unwrap();
        let size = client.recv(&mut reply).unwrap();
        assert_eq!(&reply[..size], frame_start_reply(&query, false).unwrap());
        assert_eq!(launches.load(Ordering::Relaxed), 0);
        trusted.store(true, Ordering::Relaxed);
        for _ in 0..2 {
            client.send(&query).unwrap();
            let size = client.recv(&mut reply).unwrap();
            assert_eq!(&reply[..size], frame_start_reply(&query, true).unwrap());
        }
        // The second reply is after the first launch, exercising retransmission suppression.
        let deadline=Instant::now()+Duration::from_secs(1);
        while launches.load(Ordering::Relaxed)==0&&Instant::now()<deadline{thread::sleep(Duration::from_millis(2));}
        assert_eq!(launches.load(Ordering::Relaxed), 1);
        let mut status=query.clone();status[..8].copy_from_slice(b"ALVRFQ01");
        loop{client.send(&status).unwrap();client.recv(&mut reply).unwrap();if reply[17]==3{break;}assert!(Instant::now()<deadline);}
        assert_eq!(&reply[17..19],&[3,2]);assert_eq!(u64::from_le_bytes(reply[19..27].try_into().unwrap()),1);
        // Retransmission uses the same request nonce; a new user retry uses a
        // fresh nonce and starts a new attempt immediately after explicit failure.
        client.send(&query).unwrap();client.recv(&mut reply).unwrap();assert_eq!(launches.load(Ordering::Relaxed),1);
        query[8..16].copy_from_slice(b"ijklmnop");client.send(&query).unwrap();client.recv(&mut reply).unwrap();
        let deadline=Instant::now()+Duration::from_secs(1);
        loop{client.send(&status).unwrap();client.recv(&mut reply).unwrap();
            if reply[17]==3&&u64::from_le_bytes(reply[19..27].try_into().unwrap())==2{break;}assert!(Instant::now()<deadline);}
        assert_eq!(launches.load(Ordering::Relaxed),2);
    }
}
