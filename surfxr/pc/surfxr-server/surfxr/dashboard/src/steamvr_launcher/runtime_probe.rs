//! Reuse the existing local SURFXR version endpoint; no new service or driver registration.
use surfxr_common::SURFXR_VERSION;
use std::time::Duration;

pub(super) fn request_shutdown(port: u16, restart: bool) -> surfxr_common::anyhow::Result<()> {
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(2)))
        .build()
        .into();
    let action = if restart { "restart" } else { "shutdown" };
    agent
        .post(format!("http://127.0.0.1:{port}/api/steamvr/{action}"))
        .header("X-SURFXR", "true")
        .send_empty()?;
    Ok(())
}

pub(super) fn surfxr_driver_running(port: u16) -> bool {
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_millis(200)))
        .build()
        .into();
    agent
        .get(format!("http://127.0.0.1:{port}/api/version"))
        .header("X-SURFXR", "true")
        .call()
        .ok()
        .and_then(|r| r.into_body().read_to_string().ok())
        .is_some_and(|version| version.trim() == SURFXR_VERSION.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        io::{Read, Write},
        net::TcpListener,
        thread,
    };
    #[test]
    fn shutdown_is_delivered_before_return_and_http_failures_are_not_success() {
        for (restart, status) in [(true, 200), (false, 200), (true, 500)] {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let port = listener.local_addr().unwrap().port();
            let peer = thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                stream
                    .set_read_timeout(Some(Duration::from_secs(2)))
                    .unwrap();
                let mut bytes = [0; 4096];
                let count = stream.read(&mut bytes).unwrap();
                let request = std::str::from_utf8(&bytes[..count]).unwrap().to_lowercase();
                assert!(request.starts_with(if restart {
                    "post /api/steamvr/restart "
                } else {
                    "post /api/steamvr/shutdown "
                }));
                assert!(request.contains("x-surfxr: true"));
                write!(
                    stream,
                    "HTTP/1.1 {status} Test\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                )
                .unwrap();
            });
            assert_eq!(request_shutdown(port, restart).is_ok(), status == 200);
            peer.join().unwrap();
        }
    }
}
