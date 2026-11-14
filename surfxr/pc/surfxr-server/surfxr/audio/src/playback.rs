use surfxr_common::{
    ConnectionError,
    anyhow::{Result, bail},
    info,
    parking_lot::Mutex,
};
use surfxr_session::AudioBufferingConfig;
use surfxr_sockets::StreamReceiver;
use cpal::{Device, Sample};
use rodio::{OutputStreamBuilder, Source};
use std::{
    collections::VecDeque,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    time::{Duration, Instant},
};

// Audio callback. This is designed to be as less complex as possible. Still, when needed, this
// callback can render a fade-out autonomously.
#[inline]
pub fn get_next_frame_batch(
    sample_buffer: &mut VecDeque<f32>,
    channels_count: usize,
    batch_frames_count: usize,
) -> Vec<f32> {
    if sample_buffer.len() / channels_count >= batch_frames_count {
        let mut batch = sample_buffer
            .drain(0..batch_frames_count * channels_count)
            .collect::<Vec<_>>();

        if sample_buffer.len() / channels_count < batch_frames_count {
            // Render fade-out. It is completely contained in the current batch
            for f in 0..batch_frames_count {
                let volume = 1. - f as f32 / batch_frames_count as f32;
                for c in 0..channels_count {
                    batch[f * channels_count + c] *= volume;
                }
            }
        }
        // fade-ins and cross-fades are rendered in the receive loop directly inside sample_buffer.

        batch
    } else {
        vec![0.; batch_frames_count * channels_count]
    }
}

// The receive loop is resposible for ensuring smooth transitions in case of disruptions (buffer
// underflow, overflow, packet loss). In case the computation takes too much time, the audio
// callback will gracefully handle an interruption, and the callback timing and sound wave
// continuity will not be affected.
pub fn receive_samples_loop(
    is_running: impl Fn() -> bool,
    receiver: &mut StreamReceiver<()>,
    sample_buffer: Arc<Mutex<VecDeque<f32>>>,
    channels_count: usize,
    batch_frames_count: usize,
    average_buffer_frames_count: usize,
) -> Result<()> {
    receive_samples_from(is_running, || match receiver.recv(Duration::from_millis(500)) {
        Ok(data) => { let (_, packet)=data.get()?; Ok(Some((packet.to_vec(),data.had_packet_loss()))) },
        Err(ConnectionError::TryAgain(_)) => Ok(None),
        Err(ConnectionError::Other(e)) => Err(e),
    }, sample_buffer,channels_count,batch_frames_count,average_buffer_frames_count)
}
fn receive_samples_from(
    is_running: impl Fn() -> bool,
    mut receive: impl FnMut() -> Result<Option<(Vec<u8>, bool)>>,
    sample_buffer: Arc<Mutex<VecDeque<f32>>>,
    channels_count: usize,
    batch_frames_count: usize,
    average_buffer_frames_count: usize,
) -> Result<()> {
    let mut recovery_sample_buffer = vec![];
    while is_running() {
        let Some((packet, lost)) = receive()? else { continue; };

        let new_samples = packet
            .chunks_exact(2)
            .map(|c| i16::from_ne_bytes([c[0], c[1]]).to_sample::<f32>())
            .collect::<Vec<_>>();

        let mut sample_buffer_ref = sample_buffer.lock();

        if lost {
            info!("Audio packet loss!");

            if sample_buffer_ref.len() / channels_count < batch_frames_count {
                sample_buffer_ref.clear();
            } else {
                // clear remaining samples
                sample_buffer_ref.drain(batch_frames_count * channels_count..);
            }

            recovery_sample_buffer.clear();
        }

        if sample_buffer_ref.len() / channels_count < batch_frames_count {
            recovery_sample_buffer.extend(sample_buffer_ref.drain(..));
        }

        if sample_buffer_ref.is_empty() || lost {
            recovery_sample_buffer.extend(&new_samples);

            if recovery_sample_buffer.len() / channels_count
                > average_buffer_frames_count + batch_frames_count
            {
                // Fade-in
                for f in 0..batch_frames_count {
                    let volume = f as f32 / batch_frames_count as f32;
                    for c in 0..channels_count {
                        recovery_sample_buffer[f * channels_count + c] *= volume;
                    }
                }

                if lost
                    && sample_buffer_ref.len() / channels_count == batch_frames_count
                {
                    // Add a fade-out to make a cross-fade.
                    for f in 0..batch_frames_count {
                        let volume = 1. - f as f32 / batch_frames_count as f32;
                        for c in 0..channels_count {
                            recovery_sample_buffer[f * channels_count + c] +=
                                sample_buffer_ref[f * channels_count + c] * volume;
                        }
                    }

                    sample_buffer_ref.clear();
                }

                sample_buffer_ref.extend(recovery_sample_buffer.drain(..));
                info!("Audio recovered");
            }
        } else {
            sample_buffer_ref.extend(&new_samples);
        }

        // todo: use smarter policy with EventTiming
        let buffer_frames_size = sample_buffer_ref.len() / channels_count;
        if buffer_frames_size > 2 * average_buffer_frames_count + batch_frames_count {
            info!("Audio buffer overflow! size: {buffer_frames_size}");

            let drained_samples = sample_buffer_ref
                .drain(0..(buffer_frames_size - average_buffer_frames_count) * channels_count)
                .collect::<Vec<_>>();

            // Render a cross-fade.
            for f in 0..batch_frames_count {
                let volume = f as f32 / batch_frames_count as f32;
                for c in 0..channels_count {
                    let index = f * channels_count + c;
                    sample_buffer_ref[index] = sample_buffer_ref[index]
                        .mul_add(volume, drained_samples[index] * (1. - volume));
                }
            }
        }
    }

    Ok(())
}

struct StreamingSource {
    sample_buffer: Arc<Mutex<VecDeque<f32>>>,
    current_batch: Vec<f32>,
    current_batch_cursor: usize,
    channels_count: usize,
    sample_rate: u32,
    batch_frames_count: usize,
    progress: Arc<PlaybackProgress>,
}

// Observe renderer consumption, not PCM amplitude: a muted microphone is
// healthy, but a vanished WASAPI callback must allow the device to reopen.
struct PlaybackProgress {
    started: Instant,
    last_batch_ms: AtomicU64,
    failed: AtomicBool,
}

impl PlaybackProgress {
    fn new() -> Self {
        Self {
            started: Instant::now(),
            last_batch_ms: AtomicU64::new(0),
            failed: AtomicBool::new(false),
        }
    }
    fn batch(&self) {
        self.last_batch_ms
            .store(self.started.elapsed().as_millis() as u64, Ordering::Relaxed);
    }
    fn healthy(&self) -> bool {
        !self.failed.load(Ordering::Relaxed)
            && (self.started.elapsed().as_millis() as u64)
                .saturating_sub(self.last_batch_ms.load(Ordering::Relaxed))
                < 2000
    }
}

impl Source for StreamingSource {
    fn current_span_len(&self) -> Option<usize> {
        None
    }

    fn channels(&self) -> u16 {
        self.channels_count as _
    }

    fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    fn total_duration(&self) -> Option<std::time::Duration> {
        None
    }
}

impl Iterator for StreamingSource {
    type Item = f32;

    #[inline]
    fn next(&mut self) -> Option<f32> {
        if self.current_batch_cursor == 0 {
            self.progress.batch();
            self.current_batch = get_next_frame_batch(
                &mut self.sample_buffer.lock(),
                self.channels_count,
                self.batch_frames_count,
            );
        }

        let sample = self.current_batch[self.current_batch_cursor];

        self.current_batch_cursor =
            (self.current_batch_cursor + 1) % (self.batch_frames_count * self.channels_count);

        Some(sample)
    }
}

pub fn play_audio_loop(
    is_running: impl Fn() -> bool,
    device: &Device,
    channels_count: u16,
    sample_rate: u32,
    config: AudioBufferingConfig,
    receiver: &mut StreamReceiver<()>,
) -> Result<()> {
    play_audio_from(is_running,device,channels_count,sample_rate,config, || match receiver.recv(Duration::from_millis(500)) {
        Ok(data) => { let (_,packet)=data.get()?; Ok(Some((packet.to_vec(),data.had_packet_loss()))) },
        Err(ConnectionError::TryAgain(_)) => Ok(None),
        Err(ConnectionError::Other(e)) => Err(e),
    })
}
pub fn play_audio_from(
    is_running: impl Fn() -> bool,
    device: &Device,
    channels_count: u16,
    sample_rate: u32,
    config: AudioBufferingConfig,
    receive: impl FnMut() -> Result<Option<(Vec<u8>,bool)>>,
) -> Result<()> {
    // Size of a chunk of frames. It corresponds to the duration if a fade-in/out in frames.
    let batch_frames_count = sample_rate as usize * config.batch_ms as usize / 1000;

    // Average buffer size in frames
    let average_buffer_frames_count =
        sample_rate as usize * config.average_buffering_ms as usize / 1000;

    let sample_buffer = Arc::new(Mutex::new(VecDeque::new()));
    let progress = Arc::new(PlaybackProgress::new());
    let callback_progress = Arc::clone(&progress);
    let stream = OutputStreamBuilder::from_device(device.clone())?
        .with_error_callback(move |_| callback_progress.failed.store(true, Ordering::Relaxed))
        .open_stream()?;

    stream.mixer().add(StreamingSource {
        sample_buffer: Arc::clone(&sample_buffer),
        current_batch: vec![],
        current_batch_cursor: 0,
        channels_count: channels_count as _,
        sample_rate,
        batch_frames_count,
        progress: Arc::clone(&progress),
    });

    receive_samples_from(
        || is_running() && progress.healthy(),
        receive,
        sample_buffer,
        channels_count as _,
        batch_frames_count,
        average_buffer_frames_count,
    )?;

    if is_running() && !progress.healthy() {
        bail!("microphone_output_stream_failed_or_stalled");
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn output_error_and_missing_callbacks_end_playback() {
        let mut progress = PlaybackProgress::new();
        assert!(progress.healthy());
        progress.failed.store(true, Ordering::Relaxed);
        assert!(!progress.healthy());
        progress.failed.store(false, Ordering::Relaxed);
        progress.started -= Duration::from_secs(3);
        assert!(!progress.healthy());
        progress.batch();
        assert!(progress.healthy());
    }

    #[test]
    fn silent_source_keeps_output_alive_and_recovers_pcm() {
        let progress = Arc::new(PlaybackProgress::new());
        let samples = Arc::new(Mutex::new(VecDeque::new()));
        let mut source = StreamingSource {
            sample_buffer: Arc::clone(&samples),
            current_batch: vec![],
            current_batch_cursor: 0,
            channels_count: 1,
            sample_rate: 48000,
            batch_frames_count: 2,
            progress: Arc::clone(&progress),
        };
        assert_eq!(source.next(), Some(0.0));
        assert_eq!(source.next(), Some(0.0));
        assert!(progress.healthy());
        samples.lock().extend([0.25, -0.25, 0.5, -0.5]);
        assert_eq!(source.next(), Some(0.25));
        assert_eq!(source.next(), Some(-0.25));
    }
}
