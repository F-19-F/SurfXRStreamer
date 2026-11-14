use super::*;

pub fn create_recording_file(connection_context: &ConnectionContext, settings: &Settings) {
    let codec = settings.video.preferred_codec;
    let ext = match codec {
        CodecType::H264 => "h264",
        CodecType::Hevc => "h265",
        CodecType::AV1 => "av1",
    };

    let path = FILESYSTEM_LAYOUT.get().unwrap().log_dir.join(format!(
        "recording.{}.{ext}",
        chrono::Local::now().format("%F.%H-%M-%S")
    ));

    match File::create(path) {
        Ok(mut file) => {
            if let Some(config) = &*connection_context.decoder_config.lock() {
                file.write_all(&config.config_buffer).ok();
            }

            *connection_context.video_recording_file.lock() = Some(file);

            connection_context
                .events_sender
                .send(ServerCoreEvent::RequestIDR)
                .ok();
        }
        Err(e) => {
            error!("Failed to record video on disk: {e}");
        }
    }
}
