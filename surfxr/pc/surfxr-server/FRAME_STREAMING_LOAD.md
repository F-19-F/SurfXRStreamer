# Frame PC streaming under startup load

Negotiated Windows 8-bit Frame gaze on NVIDIA defaults to the separate
`surfxr_frame_worker.exe` encoder/video socket owner. `CEncoder` in vrserver
captures and packs the atlas; `platform/win32/frame-worker` owns private IPC,
GPU leases and process lifetime. `src/frame_worker` bridges source metadata,
controls and the original production `VideoSendWorker`. The worker never
initializes OpenVR. The existing xtask installs it beside the driver DLL.

Three D3D11 NT shared keyed-mutex textures pass original pictures, resolved
source views/reference space, exact gaze crops and presentation/tracking
timestamps. Capture reserves without waiting, submits the GPU copy and
publishes key 1. The child leases a GPU-complete image through NVENC Transmit.
Capture cannot overwrite Ready/Reading slots; full slots reject new raw
images. Only older unencoded images may be discarded. Encoded video remains
FIFO in the original bounded send worker (runtime bound 1–16 packets).

An unnamed file mapping and event carry same-build POD state. Only those
handles and three texture handles are inherited. The child joins a private
KILL_ON_JOB_CLOSE Job atomically at CreateProcess, with no orphan interval.
Shutdown stops/join the observer, signals the child, finishes encoding and
the send worker, then releases GPU resources. A three-second exit deadline
terminates only that private Job. Startup failure restores the old encoder
and qWAVE registration; running failure requests SteamVR reconnect rather
than silently changing an encoded reference chain.

WSADuplicateSocket transfers the negotiated video UDP socket; read ownership
stays in vrserver. Its qWAVE flow is released before duplication (Windows
does not support duplicating QoS-enabled sockets) and registered in the
child. Audio/tracking sockets remain separate. The existing fragmenter,
pacing budget, FEC and wire/header shape are reused. Encoding dimensions,
codec, NVENC preset, quantization and bitrate parameters are unchanged.
Encoding/send threads keep their existing MMCSS registration. The worker
inherits the parent's actual CPU/GPU process and relative GPU context levels;
new processes otherwise silently default to Normal. It does not choose higher
levels or change affinity. Application/system GPU load can still delay it.
AMF/VPL/software and other clients
keep their implementations. `SURFXR_FRAME_PROCESS=0` selects the prior
in-process path; `SURFXR_FRAME_ASYNC_ENCODER=0` remains its device diagnostic.

Control snapshots carry dynamic bitrate/FPS and mapped per-hand haptics
with original QPC expiry/pending age. The child never waits for a parent
session/statistics/tracking lock. Decoder configuration has a separate
bounded reliable slot. Four observation slots, each at most 8 MiB, return
statistics/logs and optional mirror/recording payloads without blocking
video. Full observation slots lose telemetry; optional captured payloads
resume at a requested IDR after a gap. Recording/mirroring is therefore
bounded best effort under a blocked parent, never a reason to stop sending.

When no new picture arrives for 20 ms/two frame periods, the child re-encodes
its last leased image at the configured FPS. It preserves that image's old
timestamp/views/crops, maintaining the stream without pretending that the
game rendered a fresh pose. A private high-resolution waitable timer keeps
the polling cadence independent of vrserver's timer resolution. Source-image
statistics exclude these repeats.
The headset may keep presenting its cached image; long stalls exceed its
finite tracking/metadata window. This is stream continuity with a frozen
picture, not new game frames or reserved GPU capacity. A parent crash kills
the worker; process suspension is a different case and does not kill it.

Five-second numeric encoder/queue/UDP windows retain their contracts.
`Frame process timing` additionally reports worker PID, source images,
encodes, repeated images, source idle age, observation drops, failure and
the inherited CPU/GPU class and GPU application success.
The paired repository's existing timing collector retains only numeric
whitelisted fields. Capture duration measures CPU submission; encode time
includes output retrieval/callback and is not pure hardware latency.

Validation reuses `cargo test -p surfxr_server_core -p surfxr_sockets --lib`,
the existing WARP `gaze_atlas_test` executable and its private process helper
(compile command in `FRAME_GAZE_STREAMING.md`). It exercises production NT
mailbox/Parent methods on actual child processes, exact pixels/timestamps/
crops, immutable held leases, parent inactivity, full-slot rejection,
newest selection/cancellation, atomic Job membership and bounded cooperative/
forced teardown. Rust fixtures use reserved ephemeral loopback endpoints,
exercise the production imported UDP writer/FEC/payload/FIFO indices, and
verify haptics expiry and no stale pulse replay. No live Streamer endpoints
are contacted by these fixtures. Build/package with the existing
`cargo xtask build-streamer --release --keep-config`.

Reference contracts: [WSADuplicateSocket](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaduplicatesocketw),
[DXGI NT shared handles](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiresource1-createsharedhandle).
Build/offline checks, hardware NVENC, actual headset rendering and wearer
acceptance are separate evidence categories. The paired SURFXR.md records live
validation and exact deployed hashes; no private media is stored here.

Microphone output recovery (2026-10-10) belongs to `audio/src/playback.rs` and
`server_core/src/microphone.rs`. The renderer reports output callback errors
and two seconds without sample consumption to its owner; actual zero-valued
PCM remains healthy. The microphone owner re-enumerates only the configured
virtual cable pair and reopens it after a cancellable one-second delay,
refreshing the OpenVR recording endpoint property. It keeps the original
bounded receiver subscription and retires old playback buffers on reopen.
Network errors propagate instead of being discarded. No session settings,
default Windows audio devices, microphone PCM format or video policy change.
Offline checks reuse `cargo test -p surfxr_audio --lib`; real virtual-cable
delivery and game voice acceptance require the paired client and live test.

Connection teardown recovery (2026-10-10): the original `Hmd::StopStreaming`
only lowered proximity and retained its encoder. A Frame process therefore
kept the previous connection's UDP 9944 socket, making the next handshake
fail with Windows error 10048. For a process encoder, disconnect now detaches
it under the compositor Present mutex, joins the observer/child with the
existing bounded shutdown, then releases its GPU leases. The next connection
creates a fresh encoder and restores the saved eye view parameters. Other
encoder backends retain their lifecycle. UDP bind/import/clone handles also
explicitly clear inheritance; only WSADuplicateSocket transfers the video
socket to its intended child. Protocol bytes and QoS/pacing remain unchanged.

Windows Release build/package, 21 socket tests and the existing WARP/process
fixture passed (147456 channel checks, maximum error 1). The socket fixture
uses reserved loopback endpoints to check teardown/rebind and real child
socket import. Authorized live validation kept vrserver PID 3864 and Frame
viewer PID 69738: disconnect/reconnect completed in about 2.5 seconds, old
worker PID 17264 exited and new worker PID 24116 started. Steam Streaming
Microphone then delivered 719520 capture frames in 15 seconds with 575474
nonzero samples and no invalid samples. Session settings matched the backup.
These are transport/rendering and numeric signal checks, not listening or
game voice acceptance; the paired client documentation records the hashes.

144 Hz support (2026-10-11): the Dashboard framerate preset includes 144 Hz
and the advanced preferred-FPS slider extends to 144. Defaults and the
closest-client-rate negotiation stay unchanged; Frame advertises its runtime
rates, including 144 Hz. Display cadence must also be verified on the client.

The existing five-second encoder window additionally reports encode_total_us,
encode_over_budget and encode_budget_us. Divide total by encoded for the mean
worker Transmit call; compare overruns with the negotiated frame period. This
includes input GPU waits, NVENC completion/readback and packet handoff, not
pure hardware occupancy. Repeated old images remain separately counted in
the process window and must not be interpreted as fresh game frames. The
paired numeric collector accepts these optional fields from newer workers,
while retaining compatibility with the preceding window format.
The same window counts source_nonadvancing_timestamps against the source
high-water timestamp across windows. It distinguishes new capture serials
from eligible new presentation identities without retaining timestamps or
poses, and without changing the source metadata or encoding policy.

Steam microphone gain correction (2026-10-10): a live capture endpoint at
scalar 1.0 had +30 dB master/channel gain (range -96 to +30 dB). This applied
about 31.6 times amplification after the client's bounded PCM16/local gain.
A five-second capture had peak 16.5525 and 110364 of 477120 samples above
full scale. Nonzero PCM and successful reconnect were therefore insufficient
evidence of usable microphone volume in the preceding validation.

`audio/windows::cap_steam_microphone_boost_windows` now lowers positive
capture gain to 0 dB before each configured microphone open/recovery. It
matches only the selected Steam Streaming Microphone source, keeps negative
gain and mute, and leaves other endpoints/configured cable types unchanged.
Failure enters the existing output retry instead of opening with unsafe gain.
Frame retains its local 0–1 volume control; no limiter flattens the PCM and
no automatic gain/silence gate was added. Windows exposes 0 dB here as about
53.7%, so its percentage must not be interpreted as linear PCM amplitude.

Existing four audio tests, Windows Release build/package and the paired
numeric meter self-tests passed. Installed driver SHA-256 is
`39b3070a2174ee88e2001e54076fcd98f0170715eee7a1413b4f8919aa0a17f6`.
After authorized PC restart the same Frame viewer reconnected; its binary
was unchanged. Session settings matched the immediate deployment backup.
The capture endpoint was 0 dB, unmuted, both channels 0 dB; a 15-second live
capture delivered 718560 frames, 646542 nonzero samples, peak 0.686147,
RMS 0.0450551 and zero full-scale/over-full-scale/invalid samples. The user
then confirmed normal actual volume. No voice recording or game call was
captured, and no new Frame build/offscreen GPU checks were required.
