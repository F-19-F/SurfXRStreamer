//! Restart intent survives a late or temporarily lagging Dashboard subscription.
use surfxr_events::{Event, EventType};
use std::sync::atomic::{AtomicBool, Ordering};
use tokio::sync::broadcast;

pub(crate) static RESTART: RestartState = RestartState::new();

pub(crate) struct RestartState(AtomicBool);

impl RestartState {
    const fn new() -> Self {
        Self(AtomicBool::new(false))
    }

    pub(crate) fn request(&self) {
        // A requested driver restart is terminal for this runtime. Only the new
        // driver process clears it; a disconnected subscriber cannot consume it.
        self.0.store(true, Ordering::Release);
    }

    pub(crate) fn subscribe<'a>(&'a self, sender: &broadcast::Sender<Event>) -> Subscription<'a> {
        Subscription {
            state: self,
            receiver: sender.subscribe(),
            delivered: false,
        }
    }
}

pub(crate) struct Subscription<'a> {
    state: &'a RestartState,
    receiver: broadcast::Receiver<Event>,
    delivered: bool,
}

impl Subscription<'_> {
    pub(crate) async fn next(&mut self) -> Option<Event> {
        loop {
            // Subscribe before checking intent: requests racing registration are
            // either observed here or arrive through the existing event channel.
            if !self.delivered && self.state.0.load(Ordering::Acquire) {
                self.delivered = true;
                return Some(Event {
                    timestamp: chrono::Local::now().format("%H:%M:%S.%3f").to_string(),
                    event_type: EventType::ServerRequestsSelfRestart,
                });
            }
            match self.receiver.recv().await {
                Ok(event) => {
                    if matches!(event.event_type, EventType::ServerRequestsSelfRestart) {
                        if self.delivered {
                            continue;
                        }
                        self.delivered = true;
                    }
                    return Some(event);
                }
                Err(broadcast::error::RecvError::Lagged(_)) => continue,
                Err(broadcast::error::RecvError::Closed) => return None,
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn event(restart: bool) -> Event {
        Event {
            timestamp: String::new(),
            event_type: if restart {
                EventType::ServerRequestsSelfRestart
            } else {
                EventType::DriversList(vec![])
            },
        }
    }
    fn is_restart(event: Option<Event>) -> bool {
        matches!(
            event.unwrap().event_type,
            EventType::ServerRequestsSelfRestart
        )
    }

    #[tokio::test]
    async fn late_dashboard_and_reconnect_receive_pending_restart() {
        let state = RestartState::new();
        let (sender, _) = broadcast::channel(2);
        state.request();
        assert!(sender.send(event(true)).is_err()); // Original startup race: no subscribers.
        assert!(is_restart(state.subscribe(&sender).next().await));
        assert!(is_restart(state.subscribe(&sender).next().await));
        assert!(!RestartState::new().0.load(Ordering::Acquire));
    }

    #[tokio::test]
    async fn request_racing_subscription_is_delivered_once() {
        let state = RestartState::new();
        let (sender, _) = broadcast::channel(4);
        let mut subscription = state.subscribe(&sender);
        state.request();
        sender.send(event(true)).unwrap();
        sender.send(event(false)).unwrap();
        assert!(is_restart(subscription.next().await));
        assert!(!is_restart(subscription.next().await));
    }

    #[tokio::test]
    async fn lagging_dashboard_recovers_restart_and_keeps_normal_events() {
        let state = RestartState::new();
        let (sender, _) = broadcast::channel(2);
        let mut subscription = state.subscribe(&sender);
        state.request();
        sender.send(event(true)).unwrap();
        for _ in 0..4 {
            sender.send(event(false)).unwrap();
        }
        assert!(is_restart(subscription.next().await));
        assert!(!is_restart(subscription.next().await));
    }

    #[tokio::test]
    async fn connected_dashboard_receives_request_while_waiting() {
        let state = RestartState::new();
        let (sender, _) = broadcast::channel(2);
        let mut subscription = state.subscribe(&sender);
        let (received, _) = tokio::join!(subscription.next(), async {
            tokio::task::yield_now().await;
            state.request();
            sender.send(event(true)).unwrap();
        });
        assert!(is_restart(received));
    }
}
