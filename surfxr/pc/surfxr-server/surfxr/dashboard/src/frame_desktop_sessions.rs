//! Process-local session controls. Never persist tickets or change device trust.
use super::Ticket;
use std::{collections::HashMap, io::Write, net::TcpStream, sync::{LazyLock, Mutex}};

#[derive(Clone)]
pub struct Session {
    pub id: [u8; 32],
    pub hostname: String,
    pub ip: std::net::IpAddr,
    pub system: bool,
    pub mode: &'static str,
    pub disconnecting: bool,
}
struct Entry {
    status: Session,
    watch: Option<TcpStream>,
}
#[derive(Default)]
struct Sessions {
    closing: bool,
    entries: HashMap<[u8; 32], Entry>,
}
static SESSIONS: LazyLock<Mutex<Sessions>> = LazyLock::new(Default::default);

impl Entry {
    fn disconnect(&mut self) {
        self.status.disconnecting = true;
        // Nonblocking, one byte; a failed write still revokes the next lease.
        if let Some(watch) = &mut self.watch { watch.write_all(&[2]).ok(); }
    }
    fn alive(&self) -> bool {
        let Some(watch) = &self.watch else { return true; };
        match watch.peek(&mut [0]) {
            Ok(0) => false,
            Err(error) => error.kind() == std::io::ErrorKind::WouldBlock,
            _ => false, // The notification channel is server-to-host only.
        }
    }
}
pub fn snapshot() -> Vec<Session> {
    let mut entries: Vec<_> = SESSIONS.lock().unwrap().entries.values()
        .map(|entry| entry.status.clone()).collect();
    entries.sort_by(|a, b| a.hostname.cmp(&b.hostname));
    entries
}
pub fn disconnect(id: &[u8; 32]) {
    if let Some(entry) = SESSIONS.lock().unwrap().entries.get_mut(id) { entry.disconnect(); }
}
pub fn shutdown() {
    SESSIONS.lock().unwrap().shutdown();
}
impl Sessions {
    fn shutdown(&mut self) {
        self.closing = true;
        for entry in self.entries.values_mut() { entry.disconnect(); }
    }
}
pub(super) fn closing() -> bool { SESSIONS.lock().unwrap().closing }
pub(super) fn revoked(id: &[u8; 32]) -> bool {
    let sessions = SESSIONS.lock().unwrap();
    sessions.closing || sessions.entries.get(id).is_some_and(|e| e.status.disconnecting)
}
pub(super) fn update(id: [u8; 32], ticket: &Ticket, flags: u8) {
    let mut sessions = SESSIONS.lock().unwrap();
    if sessions.closing { return; }
    let status = Session { id, hostname: ticket.hostname.clone(), ip: ticket.ip,
        system: ticket.system, mode: if flags & 2 == 0 { "Connected (idle)" }
            else if flags & 16 != 0 { "VR streaming" } else { "Desktop streaming" },
        disconnecting: false };
    let entry = sessions.entries.entry(id).or_insert_with(|| Entry { status: status.clone(), watch: None });
    if !entry.status.disconnecting { entry.status = status; }
}
pub(super) fn watch(id: &[u8; 32], stream: &TcpStream) -> Option<()> {
    let mut sessions = SESSIONS.lock().unwrap();
    if sessions.closing { return None; }
    let entry = sessions.entries.get_mut(id)?;
    if entry.status.disconnecting || entry.watch.is_some() { return None; }
    let watch = stream.try_clone().ok()?;
    watch.set_nonblocking(true).ok()?;
    entry.watch = Some(watch);
    Some(())
}
pub(super) fn prune(tickets: &mut HashMap<[u8; 32], Ticket>) {
    let mut sessions = SESSIONS.lock().unwrap();
    tickets.retain(|id, ticket| ticket.expires > std::time::Instant::now()
        && sessions.entries.get(id).is_none_or(Entry::alive));
    sessions.entries.retain(|id, _| tickets.contains_key(id));
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{io::Read, net::{Ipv4Addr, TcpListener}, time::{Duration, Instant}};
    #[test]
    fn shutdown_notifies_all_sessions_before_any_worker_join() {
        let mut sessions = Sessions::default();
        let mut hosts = vec![];
        for id in [[1; 32], [2; 32]] {
            let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
            let host = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
            host.set_read_timeout(Some(Duration::from_millis(250))).unwrap();
            let (watch, _) = listener.accept().unwrap();
            watch.set_nonblocking(true).unwrap();
            sessions.entries.insert(id, Entry { status: Session { id, hostname: "test".into(),
                ip: Ipv4Addr::LOCALHOST.into(), system: true, mode: "Desktop streaming", disconnecting: false },
                watch: Some(watch) });
            hosts.push(host);
        }
        let start = Instant::now();
        sessions.shutdown();
        assert!(sessions.closing);
        for mut host in hosts {
            let mut notification = [0]; host.read_exact(&mut notification).unwrap();
            assert_eq!(notification, [2]);
        }
        assert!(sessions.entries.values().all(|e| e.status.disconnecting));
        assert!(start.elapsed() < Duration::from_millis(250));
    }
}
