use super::*;

pub(super) fn spawn(ctx: Arc<ConnectionContext>, client_hostname: String,
    mut statics_receiver: surfxr_sockets::StreamReceiver<ClientStatistics>) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        while is_streaming(&client_hostname) {
            let data = match statics_receiver.recv(STREAMING_RECV_TIMEOUT) {
                Ok(stats) => stats,
                Err(ConnectionError::TryAgain(_)) => continue,
                Err(ConnectionError::Other(_)) => return,
            };
            let Ok(client_stats) = data.get_header() else {
                return;
            };

            if let Some(stats) = &mut *ctx.statistics_manager.write() {
                let timestamp = client_stats.target_timestamp;
                let decoder_latency = client_stats.video_decode;
                let (network_latency, game_latency) = stats.report_statistics(client_stats);

                ctx.events_sender
                    .send(ServerCoreEvent::GameRenderLatencyFeedback(game_latency))
                    .ok();

                let session_manager_lock = SESSION_MANAGER.read();
                ctx.bitrate_manager.lock().report_frame_latencies(
                    &session_manager_lock.settings().video.bitrate.mode,
                    timestamp,
                    network_latency,
                    decoder_latency,
                );
            }
        }
    })
}
