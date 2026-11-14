//! Transfer a negotiated Frame UDP writer to an independent process.
//! The production shard writer, FEC, pacing and QoS implementation is reused.
use super::{StreamSender, udp};
use surfxr_common::{anyhow::Result, parking_lot::Mutex};
use std::{marker::PhantomData, net::UdpSocket, sync::Arc};

impl<H> StreamSender<H> {
    /// Called by the socket owner between packets, never by the encoder thread.
    pub fn update_video_bitrate(&self, bitrate_bps: u64, overhead: u32) {
        self.inner.lock().update_video_bitrate(bitrate_bps, overhead);
    }

    /// Uses the production UDP shard/FEC/pacing writer without an intermediate
    /// socket or receive thread. `send` may only borrow bytes during this call.
    pub fn from_frame_callback(
        send:impl Fn(&[u8])->Result<()>+Send+'static,packet_size:usize,fec:bool,pacing_bytes_per_second:u64,
    )->Result<Self> {
        let mut writer=udp::direct_writer(send,packet_size)?;
        writer.set_frame_video_fec(fec);writer.set_video_pacing(pacing_bytes_per_second);
        let payload_offset=writer.payload_offset();
        Ok(Self {inner:Arc::new(Mutex::new(writer)),stream_id:surfxr_packets::VIDEO,
            payload_offset,next_packet_index:0,used_buffers:vec![],_phantom:PhantomData})
    }
    pub fn external_owner_endpoint(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
            stream_id: self.stream_id,
            payload_offset: self.payload_offset,
            next_packet_index: self.next_packet_index,
            used_buffers: vec![],
            _phantom: PhantomData,
        }
    }
    #[cfg(windows)]
    pub fn frame_udp_socket_for_process(&self) -> Option<usize> {
        self.inner.lock().frame_udp_socket_for_process()
    }
    pub fn restore_frame_udp_qos(&self) {
        self.inner.lock().set_frame_udp_qos(false);
    }

    pub fn from_external_frame_udp(
        socket: UdpSocket,
        packet_size: usize,
        fec: bool,
        pacing_bytes_per_second: u64,
    ) -> Result<Self> {
        let (mut writer, _) = udp::split_multiplexed(socket, packet_size)?;
        writer.set_frame_video_fec(fec);
        writer.set_video_pacing(pacing_bytes_per_second);
        writer.set_frame_udp_qos(false);
        let payload_offset = writer.payload_offset();
        Ok(Self {
            inner: Arc::new(Mutex::new(writer)),
            stream_id: surfxr_packets::VIDEO,
            payload_offset,
            next_packet_index: 0,
            used_buffers: vec![],
            _phantom: PhantomData,
        })
    }
}
