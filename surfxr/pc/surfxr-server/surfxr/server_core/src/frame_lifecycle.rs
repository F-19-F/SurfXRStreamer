//! Explicit Frame shutdown is distinct from a driver restart or transport loss.
use surfxr_common::parking_lot::Mutex;
use surfxr_packets::ServerControlPacket;
use surfxr_sockets::ControlSocketSender;
use std::{collections::HashMap, sync::Arc, time::Duration};

type Sender = Arc<Mutex<ControlSocketSender<ServerControlPacket>>>;

#[derive(Default)]
pub(crate) struct FrameSessions {
    senders: Mutex<HashMap<String, Sender>>,
    restarting: std::sync::atomic::AtomicBool,
}

pub(crate) fn negotiate(capabilities: &str, extensions: &str) -> (String, bool) {
    let requested = serde_json::from_str::<serde_json::Value>(capabilities)
        .ok()
        .is_some_and(|c| c["frame_session_lifecycle"].as_u64() == Some(1));
    if !requested {
        return (extensions.to_owned(), false);
    }
    let mut ext = serde_json::from_str::<serde_json::Value>(extensions)
        .unwrap_or_else(|_| serde_json::json!({}));
    if requested {
        ext["frame_session_lifecycle"] = 1.into();
    }
    (ext.to_string(), requested)
}

impl FrameSessions {
    pub(crate) fn register(&self, hostname: String, sender: Sender) {
        self.senders.lock().insert(hostname, sender);
    }
    pub(crate) fn remove(&self, hostname: &str) {
        self.senders.lock().remove(hostname);
    }
    pub(crate) fn restarting(&self) {
        self.restarting
            .store(true, std::sync::atomic::Ordering::Release);
    }
    pub(crate) fn shutdown(&self) {
        let restart = self.restarting.load(std::sync::atomic::Ordering::Acquire);
        let senders: Vec<_> = self.senders.lock().values().cloned().collect();
        for sender in senders {
            if let Some(mut sender) = sender.try_lock_for(Duration::from_millis(250)) {
                let packet = if restart {
                    ServerControlPacket::Restarting
                } else {
                    ServerControlPacket::Reserved("frame_session_stopped".into())
                };
                sender.send(&packet).ok();
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use surfxr_sockets::ProtoControlSocket;
    use std::net::{TcpListener, TcpStream};

    #[test]
    fn shutdown_and_restart_use_existing_reserved_wire_and_only_opt_in() {
        let unchanged = "{ \"existing\": 1 }";
        assert_eq!(negotiate("{}", unchanged).0, unchanged);
        assert!(!negotiate("{}", "{}").1);
        assert!(!negotiate(r#"{"frame_session_lifecycle":2}"#, "{}").1);
        assert!(negotiate(r#"{"frame_session_lifecycle":1}"#, "{}").1);
        for restart in [false, true] {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let client = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
            let server = listener.accept().unwrap().0;
            let (sender, _) = ProtoControlSocket::from_accepted(server, Duration::from_millis(250))
                .ok()
                .unwrap()
                .split::<ServerControlPacket, ()>(Duration::from_millis(250))
                .unwrap();
            let sessions = FrameSessions::default();
            sessions.register("frame".into(), Arc::new(Mutex::new(sender)));
            if restart {
                sessions.restarting();
            }
            sessions.shutdown();
            let mut client = ProtoControlSocket::from_accepted(client, Duration::from_secs(1))
                .ok()
                .unwrap();
            match client
                .recv::<ServerControlPacket>(Duration::from_secs(1))
                .ok()
                .unwrap()
            {
                ServerControlPacket::Restarting => assert!(restart),
                ServerControlPacket::Reserved(value) => {
                    assert!(!restart);
                    assert_eq!(value, "frame_session_stopped");
                }
                _ => panic!("wrong lifecycle notification"),
            }
            sessions.remove("frame");
        }
    }
}
