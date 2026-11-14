//! Dashboard owns desktop admission. Tickets never persist and are consumed once.
use surfxr_common::RelaxedAtomic;
use surfxr_session::Settings;
use rand::RngExt;
use std::{
    collections::HashMap,
    io::{Read, Write},
    net::{IpAddr, Ipv4Addr, TcpListener, TcpStream},
    sync::{Arc, Mutex, OnceLock},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

pub const PORT: u16 = surfxr_common::ports::DASHBOARD;
// Retain consumed unified tickets through a bounded PC scheduler stall.
// The daemon's 15s silence budget ends before this 20s retention window.
const UNIFIED_LEASE_TTL: Duration = Duration::from_secs(20);
#[path = "frame_desktop_sessions.rs"]
pub mod sessions;
#[derive(Clone, Default)]
pub struct Status {
    pub message: String,
    pub hostname: String,
    pub system: bool,
    pub active: bool,
}
static STATUS: OnceLock<Mutex<Status>> = OnceLock::new();
pub fn status() -> Status {
    let mut status = STATUS.get_or_init(Default::default).lock().unwrap().clone();
    if !status.hostname.is_empty() {
        if let Some(session) = sessions::snapshot().into_iter().find(|s| s.hostname == status.hostname) {
            if session.disconnecting { status.message = "Disconnecting".into(); status.active = false; }
        } else {
            status.message = "Desktop ready".into(); status.hostname.clear(); status.active = false;
        }
    }
    status
}
fn publish(value: Status) {
    *STATUS.get_or_init(Default::default).lock().unwrap() = value;
}
struct Ticket {
    key: [u8; 32],
    ip: IpAddr,
    hostname: String,
    expires: Instant,
    consumed: bool,
    input: bool,
    audio: bool,
    system: bool,
    unified: bool,
    audio_config: Vec<u8>,
    port: u16,
}
fn response(key: &[u8; 32], id: &[u8; 32], settings: &Settings, unified: bool,port:u16) -> Vec<u8> {
    let config = &settings.extra.frame_desktop;
    let mut reply = vec![0];
    reply.extend(key);
    reply.extend(id);
    reply.extend(port.to_le_bytes());
    reply.extend([
        u8::from(config.allow_input) | u8::from(config.allow_audio) * 2 | u8::from(unified) * 4,
        config.framerate.clamp(30, 144) as u8,
        config.bitrate_mbps.clamp(1, 200) as u8,
    ]);
    reply
}
pub fn spawn(
    running: Arc<RelaxedAtomic>,
    mut authorize: impl FnMut(&str, IpAddr, bool) -> Option<(bool, Settings)> + Send + 'static,
    mut save_monitors: impl FnMut(u32) -> bool + Send + 'static,
    mut settings: impl FnMut(&[u8]) -> Option<Vec<u8>> + Send + 'static,
) -> JoinHandle<()> {
    thread::spawn(move || {
        let listener = match TcpListener::bind((Ipv4Addr::UNSPECIFIED, PORT)) {
            Ok(value) => value,
            Err(_) => {
                publish(Status {
                    message: "Desktop broker port is unavailable".into(),
                    ..Default::default()
                });
                return;
            }
        };
        listener.set_nonblocking(true).ok();
        let mut tickets: HashMap<[u8; 32], Ticket> = HashMap::new();
        let mut host_seen = Instant::now() - Duration::from_secs(30);
        let mut host_system = false;
        let mut host_unified = false;
        let mut host_media_modes = false;
        let mut host_port=surfxr_common::ports::DESKTOP_CONTROL;
        let mut host = None;
        while running.value() {
            sessions::prune(&mut tickets);
            if let Some(child) = host.as_mut() {
                let child: &mut std::process::Child = child;
                if child.try_wait().ok().flatten().is_some() {
                    host = None;
                }
            }
            if let Ok((mut stream, peer)) = listener.accept() {
                stream
                    .set_read_timeout(Some(Duration::from_millis(200)))
                    .ok();
                stream
                    .set_write_timeout(Some(Duration::from_millis(200)))
                    .ok();
                let reply = handle(
                    &mut stream,
                    peer.ip(),
                    &mut tickets,
                    &mut host_seen,
                    &mut host_system,
                    &mut host_unified,
                    &mut host_media_modes,
                    &mut host_port,
                    &mut authorize,
                    &mut save_monitors,
                    &mut settings,
                );
                if let Some(reply) = reply {
                    stream.write_all(&reply).ok();
                }
            }
            if Instant::now().duration_since(host_seen) > Duration::from_secs(3) {
                let service = crate::desktop_service::status();
                if !service.running && host.is_none() {
                    host = crate::desktop_service::start_user_host();
                }
                publish(Status { message: if service.running { "Waiting for managed SYSTEM desktop host" } else if host.is_some() { "Starting desktop host" } else { "Desktop host missing: place surfxr-desktop.exe and its DLLs beside Dashboard" }.into(), ..Default::default() });
            }
            thread::sleep(Duration::from_millis(10));
        }
        sessions::shutdown();
        // Notify before joining children; silence is reserved for crash/stall recovery.
        if let Some(mut child) = host {
            let deadline = Instant::now() + Duration::from_secs(4);
            while Instant::now() < deadline && child.try_wait().ok().flatten().is_none() {
                thread::sleep(Duration::from_millis(20));
            }
            if child.try_wait().ok().flatten().is_none() {
                child.kill().ok();
            }
            child.wait().ok();
        }
    })
}
fn handle(
    stream: &mut TcpStream,
    peer: IpAddr,
    tickets: &mut HashMap<[u8; 32], Ticket>,
    host_seen: &mut Instant,
    host_system: &mut bool,
    host_unified: &mut bool,
    host_media_modes: &mut bool,
    host_port: &mut u16,
    authorize: &mut impl FnMut(&str, IpAddr, bool) -> Option<(bool, Settings)>,
    save_monitors: &mut impl FnMut(u32) -> bool,
    settings_request: &mut impl FnMut(&[u8]) -> Option<Vec<u8>>,
) -> Option<Vec<u8>> {
    let mut header = [0; 9];
    stream.read_exact(&mut header).ok()?;
    if &header[..8] != b"ALVRDB01" {
        return None;
    }
    match header[8] {
        8 => Some(vec![if host_seen.elapsed()<Duration::from_secs(3)&&*host_system&&*host_unified {5}else{0}]),
        9 => Some(vec![u8::from(host_seen.elapsed()<Duration::from_secs(3)&&*host_system&&*host_unified&&*host_media_modes)]),
        0 => {
            if sessions::closing() { return Some(vec![2]); }
            let mut length = [0];
            stream.read_exact(&mut length).ok()?;
            if length[0] == 0 || length[0] > 63 {
                return None;
            }
            let mut bytes = vec![0; length[0] as usize];
            stream.read_exact(&mut bytes).ok()?;
            let name = std::str::from_utf8(&bytes).ok()?;
            if !name
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
            {
                return None;
            }
            let (trusted, settings) = authorize(name, peer, true)?;
            if !trusted {
                return Some(vec![1]);
            }
            if !settings.extra.frame_desktop.enabled {
                return Some(vec![2]);
            }
            if host_seen.elapsed() > Duration::from_secs(3)
                || (settings.extra.frame_desktop.require_system_service && !*host_system)
            {
                return Some(vec![3]);
            }
            if tickets.len() >= 64 {
                return Some(vec![3]);
            }
            let key = rand::rng().random();
            let id = rand::rng().random();
            let reply = response(&key, &id, &settings, *host_unified,*host_port);
            tickets.insert(
                id,
                Ticket {
                    key,
                    ip: peer,
                    hostname: name.into(),
                    expires: Instant::now() + Duration::from_secs(15),
                    consumed: false,
                    input: settings.extra.frame_desktop.allow_input,
                    audio: settings.extra.frame_desktop.allow_audio,
                    system: false,
                    unified: *host_unified,
                    audio_config: serde_json::to_vec(&settings.audio).ok()?,
                    port:*host_port,
                },
            );
            Some(reply)
        }
        1 | 2 | 4 | 6 | 10 | 11 if peer.is_loopback() => {
            let mut id = [0; 32];
            stream.read_exact(&mut id).ok()?;
            if sessions::revoked(&id) { return Some(vec![2]); }
            let mut address = [0; 4];
            stream.read_exact(&mut address).ok()?;
            let ip = IpAddr::V4(Ipv4Addr::from(address));
            let Some(ticket) = tickets.get_mut(&id) else { return Some(vec![2]); };
            if ticket.ip != ip || ticket.expires <= Instant::now() {
                return Some(vec![2]);
            }
            let mut system = [0];
            stream.read_exact(&mut system).ok()?;
            let (trusted, settings) = match authorize(&ticket.hostname, ip, false){
                Some(value)=>value,None if header[8]==2&&ticket.unified=>return Some(vec![3]),None=>return None,
            };
            let cfg = &settings.extra.frame_desktop;
            if !trusted
                || !cfg.enabled
                || (cfg.require_system_service && system[0] & 1 != 1)
                || ticket.input != cfg.allow_input
                || ticket.audio != cfg.allow_audio
                || (ticket.unified && ticket.audio_config != serde_json::to_vec(&settings.audio).ok()?)
            {
                return Some(vec![2]);
            }
            if header[8] == 1 {
                if ticket.consumed {
                    return Some(vec![2]);
                }
                ticket.consumed = true;
                ticket.system = system[0] & 1 == 1;
                crate::pairing::connected(&ticket.hostname);
            } else if !ticket.consumed {
                return Some(vec![2]);
            }
            if header[8] == 11 {
                return Some(vec![if sessions::watch(&id, stream).is_some() { 0 } else { 2 }]);
            }
            if header[8] == 10 {
                let mut bytes=[0u8;25];
                stream.read_exact(&mut bytes).ok()?;
                let payload=settings_request(&bytes)?;
                let mut reply=vec![0];reply.extend((payload.len() as u32).to_le_bytes());reply.extend(payload);
                return Some(reply);
            }
            if header[8] == 6 && ticket.unified {
                let mut metadata=serde_json::to_value(&settings.audio).ok()?;
                metadata["frame_daemon_hostname"]=serde_json::json!(ticket.hostname);
                metadata["frame_daemon_media_aes"]=serde_json::json!(cfg.media_encryption);
                #[cfg(windows)] {
                    if let surfxr_common::settings_schema::Switch::Enabled(game)=&settings.audio.game_audio {
                        if let Ok(id)=surfxr_audio::new_output(game.device.as_ref()).and_then(|device|surfxr_audio::get_windows_device_id(&device)) {
                            metadata["frame_daemon_playback_id"]=serde_json::json!(id);
                        }
                    }
                    if let surfxr_common::settings_schema::Switch::Enabled(microphone)=&settings.audio.microphone {
                        if let Ok((_,source))=surfxr_audio::new_virtual_microphone_pair(microphone.devices.clone()) {
                            if let Ok(id)=surfxr_audio::get_windows_device_id(&source){metadata["frame_daemon_recording_id"]=serde_json::json!(id);}
                        }
                    }
                }
                let bytes=serde_json::to_vec(&metadata).ok()?;
                let mut reply = vec![0];
                reply.extend((bytes.len() as u32).to_le_bytes());
                reply.extend(bytes);
                return Some(reply);
            }
            if header[8] == 4 {
                let mut count = [0];
                stream.read_exact(&mut count).ok()?;
                return Some(if count[0] == 0 {
                    vec![0, cfg.monitor_count.clamp(1, 4) as u8]
                } else if count[0] <= 4 && save_monitors(count[0].into()) {
                    vec![0, count[0]]
                } else {
                    vec![2]
                });
            }
            ticket.expires = Instant::now() + if ticket.unified {
                UNIFIED_LEASE_TTL
            } else {
                Duration::from_secs(if header[8] == 1 { 15 } else { 3 })
            };
            let active = header[8] == 2 && system[0] & 2 != 0;
            sessions::update(id, ticket, if header[8] == 2 { system[0] } else { 0 });
            publish(Status {
                message: if active {
                    if system[0] & 16 != 0 { "VR streaming" } else { "Desktop streaming" }
                } else {
                    "Connected (idle)"
                }
                .into(),
                hostname: ticket.hostname.clone(),
                system: ticket.system,
                active,
            });
            if header[8] == 1 {
                Some(response(&ticket.key, &id, &settings, ticket.unified,ticket.port))
            } else {
                Some(vec![0])
            }
        }
        3 if peer.is_loopback() => {
            let mut system = [0];
            stream.read_exact(&mut system).ok()?;
            if system[0] & 1 == 0 && crate::desktop_service::status().running {
                return Some(vec![3]);
            }
            *host_system = system[0] & 1 == 1;
            *host_unified = system[0] & 4 != 0;
            *host_media_modes = system[0] & 8 != 0;
            *host_port=if *host_unified{let mut port=[0;2];stream.read_exact(&mut port).ok()?;let port=u16::from_le_bytes(port);if !(1024..=65533).contains(&port){return None;}port}else{surfxr_common::ports::DESKTOP_CONTROL};
            *host_seen = Instant::now();
            if !tickets.values().any(|t| t.consumed) {
                publish(Status {
                    message: "Desktop ready".into(),
                    system: *host_system,
                    ..Default::default()
                });
            }
            Some(vec![0])
        }
        _ => None,
    }
}

#[cfg(test)]
#[path = "frame_desktop_tests.rs"]
mod tests;
