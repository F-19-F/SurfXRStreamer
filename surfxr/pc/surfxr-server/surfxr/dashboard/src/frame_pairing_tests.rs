//! Exercise the production owner API on a reserved loopback endpoint, never a live driver.
use super::*;
use std::{
    io::{BufRead, BufReader, Write},
    net::TcpListener,
    time::Instant,
};

#[test]
fn remote_owner_registration_trusts_one_device_without_enabling_permanent_trust() {
    let _serial = crate::pairing::TEST_SERIAL.lock();
    struct Disarm;
    impl Drop for Disarm {
        fn drop(&mut self) {
            crate::pairing::set_enabled(false);
        }
    }
    let _disarm = Disarm;
    let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
    let port = listener.local_addr().unwrap().port();
    listener.set_nonblocking(true).unwrap();
    let owner = std::thread::spawn(move || {
        let mut manager = surfxr_server_io::ServerSessionManager::new(None);
        manager
            .session_mut()
            .session_settings
            .connection
            .client_discovery
            .content
            .auto_trust_clients = false;
        let deadline = Instant::now() + Duration::from_secs(5);
        // First registration: get/add/get/trust/get. Second: get/add/get, no trust.
        let mut actions = vec![];
        for _ in 0..8 {
            let mut stream = loop {
                match listener.accept() {
                    Ok((stream, _)) => break stream,
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                        assert!(Instant::now() < deadline, "Owner API request timed out");
                        std::thread::sleep(Duration::from_millis(2));
                    }
                    Err(error) => panic!("{error}"),
                }
            };
            stream.set_nonblocking(false).unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(2)))
                .unwrap();
            stream
                .set_write_timeout(Some(Duration::from_secs(2)))
                .unwrap();
            let mut reader = BufReader::new(&mut stream);
            let mut request = String::new();
            reader.read_line(&mut request).unwrap();
            let mut length = 0;
            let mut owner_header = false;
            loop {
                let mut header = String::new();
                assert!(reader.read_line(&mut header).unwrap() > 0);
                if header == "\r\n" {
                    break;
                }
                let lower = header.to_ascii_lowercase();
                if let Some(value) = lower.strip_prefix("content-length:") {
                    length = value.trim().parse().unwrap();
                }
                if lower.trim() == "x-surfxr: true" {
                    owner_header = true;
                }
            }
            assert!(owner_header);
            if request.starts_with("POST /api/session/client-connections ") {
                let mut bytes = vec![0; length];
                reader.read_exact(&mut bytes).unwrap();
                let (name, action): (String, ClientConnectionsAction) =
                    serde_json::from_slice(&bytes).unwrap();
                actions.push(serde_json::to_value(&action).unwrap());
                manager.update_client_connections(name, action);
            } else {
                assert!(request.starts_with("GET /api/session "));
            }
            drop(reader);
            let body = serde_json::to_vec(manager.session()).unwrap();
            write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).unwrap();
            stream.write_all(&body).unwrap();
        }
        (manager.session().clone(), actions)
    });
    crate::pairing::set_enabled(true);
    assert!(
        register_remote(port, "frame-first", "127.0.0.2".parse().unwrap())
            .unwrap()
            .0
    );
    assert!(crate::pairing::enabled());
    crate::pairing::connected("frame-first");
    assert!(!crate::pairing::enabled());
    assert!(
        !register_remote(port, "frame-second", "127.0.0.3".parse().unwrap())
            .unwrap()
            .0
    );
    let (session, actions) = owner.join().unwrap();
    assert!(session.client_connections["frame-first"].trusted);
    assert!(!session.client_connections["frame-second"].trusted);
    assert!(
        !session
            .session_settings
            .connection
            .client_discovery
            .content
            .auto_trust_clients
    );
    assert_eq!(
        actions
            .iter()
            .filter(|value| **value == serde_json::json!("Trust"))
            .count(),
        1
    );
}
