//! Five-second numeric windows; no packet contents or endpoint identities.
use super::frame_qos::FrameQos;
use std::time::Instant;

pub(super) struct UdpTiming {
    started: Instant,
    last: Option<Instant>,
    packets: u64,
    bytes: u64,
    packet_max_us: u64,
    idle_max_us: u64,
    idle_over_20ms: u64,
}

impl UdpTiming {
    pub fn new() -> Self {
        Self {
            started: Instant::now(),
            last: None,
            packets: 0,
            bytes: 0,
            packet_max_us: 0,
            idle_max_us: 0,
            idle_over_20ms: 0,
        }
    }
    pub fn packet(&mut self, stream: u16, start: Instant, bytes: usize, qos: &FrameQos) {
        let now = Instant::now();
        let idle = self.last.map_or(0, |last| {
            start.saturating_duration_since(last).as_micros() as u64
        });
        self.last = Some(now);
        self.packets += 1;
        self.bytes += bytes as u64;
        self.idle_max_us = self.idle_max_us.max(idle);
        self.idle_over_20ms += u64::from(idle > 20_000);
        self.packet_max_us = self
            .packet_max_us
            .max(now.duration_since(start).as_micros() as u64);
        if now.duration_since(self.started).as_secs() >= 5 {
            let data = serde_json::json!({"stream":stream, "seconds":now.duration_since(self.started).as_secs_f64(),
                "packets":self.packets,"bytes":self.bytes,"packet_max_us":self.packet_max_us,
                "idle_max_us":self.idle_max_us,"idle_over_20ms":self.idle_over_20ms,
                "qos_applied":u8::from(qos.applied),"qos_error":qos.error,
                "qos_dscp":qos.dscp,"qos_l2":qos.l2});
            surfxr_common::info!("Frame UDP timing: {data}");
            self.started = now;
            self.packets = 0;
            self.bytes = 0;
            self.packet_max_us = 0;
            self.idle_max_us = 0;
            self.idle_over_20ms = 0;
        }
    }
}
