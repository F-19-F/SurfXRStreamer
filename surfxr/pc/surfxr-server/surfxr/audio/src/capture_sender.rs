use surfxr_common::{
    anyhow::Result,
    parking_lot::{Condvar, Mutex},
};
use std::{
    collections::VecDeque,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    thread::{self, JoinHandle},
    time::Duration,
};

struct Pending {
    packets: VecDeque<Vec<u8>>,
    bytes: usize,
}

struct Shared {
    pending: Mutex<Pending>,
    changed: Condvar,
    stopping: AtomicBool,
    limit: usize,
    dropped_bytes: AtomicU64,
    send_failures: AtomicU64,
}

#[derive(Clone)]
pub(crate) struct CaptureProducer(Arc<Shared>);

impl CaptureProducer {
    // WASAPI's buffer must be released before any network wait. Overflow
    // preserves the newest PCM; the queue cannot grow with a stalled link.
    pub(crate) fn submit(&self, mut bytes: Vec<u8>) {
        if self.0.stopping.load(Ordering::Acquire) {
            return;
        }
        let Some(mut pending) = self.0.pending.try_lock() else {
            self.0
                .dropped_bytes
                .fetch_add(bytes.len() as u64, Ordering::Relaxed);
            return;
        };
        if self.0.stopping.load(Ordering::Acquire) {
            return;
        }
        if bytes.len() > self.0.limit {
            let skip = bytes.len() - self.0.limit;
            self.0
                .dropped_bytes
                .fetch_add(skip as u64, Ordering::Relaxed);
            bytes.drain(..skip);
        }
        while pending.bytes + bytes.len() > self.0.limit {
            if let Some(old) = pending.packets.pop_front() {
                pending.bytes -= old.len();
                self.0
                    .dropped_bytes
                    .fetch_add(old.len() as u64, Ordering::Relaxed);
            }
        }
        pending.bytes += bytes.len();
        pending.packets.push_back(bytes);
        drop(pending);
        self.0.changed.notify_one();
    }
}

pub(crate) struct CaptureSender {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
}

impl CaptureSender {
    pub(crate) fn new(
        limit: usize,
        running: Arc<dyn Fn() -> bool + Send + Sync>,
        scheduling: Option<bool>,
        mut send: impl FnMut(&[u8]) -> Result<()> + Send + 'static,
    ) -> Self {
        assert!(limit > 0);
        let shared = Arc::new(Shared {
            pending: Mutex::new(Pending {
                packets: VecDeque::new(),
                bytes: 0,
            }),
            changed: Condvar::new(),
            stopping: AtomicBool::new(false),
            limit,
            dropped_bytes: AtomicU64::new(0),
            send_failures: AtomicU64::new(0),
        });
        let queue = Arc::clone(&shared);
        let worker = thread::spawn(move || {
            let _scheduling = scheduling.map(surfxr_sockets::FrameUdpThread::new);
            while !queue.stopping.load(Ordering::Acquire) && running() {
                let packet = {
                    let mut pending = queue.pending.lock();
                    if pending.packets.is_empty() {
                        queue
                            .changed
                            .wait_for(&mut pending, Duration::from_millis(20));
                    }
                    if queue.stopping.load(Ordering::Acquire) {
                        break;
                    }
                    pending
                        .packets
                        .pop_front()
                        .inspect(|bytes| pending.bytes -= bytes.len())
                };
                if let Some(bytes) = packet
                    && send(&bytes).is_err()
                {
                    queue.send_failures.fetch_add(1, Ordering::Relaxed);
                    queue.stopping.store(true, Ordering::Release);
                }
            }
        });
        Self {
            shared,
            worker: Some(worker),
        }
    }

    pub(crate) fn producer(&self) -> CaptureProducer {
        CaptureProducer(Arc::clone(&self.shared))
    }

    pub(crate) fn stop(&mut self) -> (u64, u64) {
        self.shared.stopping.store(true, Ordering::Release);
        self.shared.changed.notify_all();
        if let Some(worker) = self.worker.take() {
            worker.join().ok();
        }
        let mut pending = self.shared.pending.lock();
        pending.packets.clear();
        pending.bytes = 0;
        (
            self.shared.dropped_bytes.load(Ordering::Relaxed),
            self.shared.send_failures.load(Ordering::Relaxed),
        )
    }
}

impl Drop for CaptureSender {
    fn drop(&mut self) {
        self.stop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;

    #[test]
    fn capture_never_waits_for_send_and_overflow_keeps_recent_pcm() {
        let (entered_tx, entered_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        let (sent_tx, sent_rx) = mpsc::channel();
        let mut first = true;
        let mut sender = CaptureSender::new(8, Arc::new(|| true), None, move |bytes| {
            if first {
                first = false;
                entered_tx.send(()).unwrap();
                release_rx.recv().unwrap();
            }
            sent_tx.send(bytes.to_vec()).unwrap();
            Ok(())
        });
        let producer = sender.producer();
        producer.submit(vec![1; 4]);
        entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        producer.submit(vec![2; 4]);
        producer.submit(vec![3; 4]);
        producer.submit(vec![4; 4]);
        assert_eq!(producer.0.pending.lock().bytes, 8);
        release_tx.send(()).unwrap();
        for value in [1, 3, 4] {
            assert_eq!(
                sent_rx.recv_timeout(Duration::from_secs(1)).unwrap(),
                vec![value; 4]
            );
        }
        assert_eq!(sender.stop(), (4, 0));
        assert_eq!(sender.stop(), (4, 0));
        producer.submit(vec![5; 4]);
        assert_eq!(producer.0.pending.lock().bytes, 0);
    }

    #[test]
    fn send_failure_stops_the_queue_and_teardown_joins() {
        let (failed_tx, failed_rx) = mpsc::channel();
        let mut sender = CaptureSender::new(8, Arc::new(|| true), None, move |_| {
            failed_tx.send(()).unwrap();
            surfxr_common::anyhow::bail!("fixture send failure")
        });
        sender.producer().submit(vec![0; 4]);
        failed_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        assert_eq!(sender.stop(), (0, 1));
    }
}
