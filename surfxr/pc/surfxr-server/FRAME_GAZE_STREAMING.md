# Steam Frame gaze atlas extension

## Frame controller status (2026-10-18)

Steam Frame emulation now assigns all status icon properties to the native
SteamVR `frame_controller` left/right assets, including the actual `seaching.gif`
filename and info/error alternatives. A valid incoming Battery sample enables
DeviceProvidesBatteryStatus; nonfinite or out-of-range values are rejected.
Clients that do not supply batteries retain the existing capability default.

The companion Frame client now omits a physically disconnected hand's motion
instead of transmitting its cached pose forever. The existing TrackingManager
and OpenVR event path consequently publish deviceIsConnected=false. Frame
controllers clear their input components and skeleton cache on this transition,
even when UDP tracking arrives before the reliable button release. New poses
reconnect the same device; no serial, input profile, role or protocol changes.
This is a standard runtime disconnect, not removal of a registered device slot.

Validation: Windows Release driver build, 28 server_core offline tests (including
production motion history on disconnect/reconnect), and all 12 referenced native
icon resource files passed. MSVC linker informational output is surfaced by Rust
as linker_messages warnings. Real power-button transitions, SteamVR status UI and
mixed Frame/Index game selection still require hardware/wearer acceptance.

## Manual SteamVR start policy (2026-10-11)

Settings → Extra → SteamVR Launcher exposes **Enable SURFXR on manual SteamVR
start**, backed by `enable_surfxr_on_manual_steamvr_start`, default false. Missing
fields in existing sessions also default false. When disabled, direct Steam or
shortcut launches decline SURFXR before creating its runtime, HTTP/media listeners
or virtual headset. When enabled, direct launches register the virtual headset
even without a Dashboard process. Changes apply to the next SteamVR start.

Dashboard launches/restarts and trusted explicit Frame connection requests share
the existing launcher. Before launch it writes a one-use, 120-second ticket beside
session.json. Driver Init consumes it atomically; Steam's factory probes do not.
Expired, future, malformed and oversized tickets fail closed. Failed process
spawns cancel the ticket, and manual-on starts consume any pending ticket too.
The launcher replaces a running SteamVR that declined SURFXR, while an active SURFXR
runtime or a launch with a valid pending ticket suppresses duplicate starts.
This does not toggle the persistent preference or unregister other VR drivers.

The Dashboard's restart/shutdown actions use the existing `/api/steamvr/restart`
and `/api/steamvr/shutdown` routes, ensuring Frame receives Restarting rather than
normal-stop notification during a Dashboard restart. Client configuration-driven
restarts continue through the same launcher and receive fresh tickets.

Validation: official Windows `cargo xtask build-streamer --release --keep-config`,
48 server_io/session/sockets/server_core unit checks and the Dashboard discovery
isolation check pass. Ticket checks include legacy settings, one-use/cancel,
120-second expiry, invalid input and four concurrent consumers. Live results and
the separate headset rendering/wearer boundaries are recorded in the companion
Frame repository's `docs/SURFXR.md` and startup-policy performance JSON.
The frozen deployment passed manual-off with Dashboard closed/open, manual-on
with Dashboard closed, Dashboard launch/restart, and Frame cold launch/reopen.
Dashboard restart replaced vrserver in 19.950 seconds with the same Frame viewer;
the following live XR sample submitted 144.006 FPS and 143.864 new stereo FPS.
The preference was restored to false and other PC settings were unchanged.
This round includes actual headset rendering; no new wearer acceptance was taken.

## Client discovery, SteamVR start and shutdown (2026-10-11)

The companion Dashboard advertises itself on UDP 19943 even before PC SteamVR
starts. Discovery uses `SURFXRFD01` + an 8-byte nonce; replies contain `SURFXRPC01`,
the nonce, `21-dev14`, the little-endian control port and a bounded ASCII name.
Only explicit client connection sends `SURFXRFS01`, nonce and a bounded hostname.
The Dashboard shares its existing SessionSource, adds unknown Frames to Devices
and requires the existing trust decision before using the existing SteamVR
launcher. `SURFXRPS01` + nonce + one acceptance byte confirms the request.
Existing Add firewall rules now includes the Dashboard executable; building
does not apply firewall rules. Discovery alone never launches the runtime.

`server_core/frame_connect` accepts Frame-initiated TCP control and reuses the
existing trusted-client pipeline. The client advertises `frame_client_connect`
v1 and `frame_client_hostname`; v20 and upstream clients keep their original
connection path. Optional `frame_session_lifecycle` v1 negotiates normal exit
using existing `ServerControlPacket::Reserved("frame_session_stopped")` before
teardown. Restart remains `Restarting`; unnegotiated wire bytes are preserved.
The Frame retires old decoder/eye sources at each attempt boundary and returns
to its existing Dashboard after normal shutdown or bounded reconnect failure.
Automatic reconnect never repeats the SteamVR start request after connecting.
Control EOF is immediate; accepted sockets and sends have bounded deadlines.

Validation covers real reserved loopback sockets for shutdown/restart, inbound
trust/state reservation and discovery/start wire bounds. Windows release and
Frame offline builds are separate from headset rendering and wearer acceptance;
this change has not been deployed or tested in a live VR session.
Final PC checks: 24 sockets tests, 15 server_core tests and the Dashboard's
reserved UDP endpoint test passed. `cargo xtask build-streamer --release
--keep-config` built the driver, worker and Dashboard; the MSVC linker emitted
its existing library-creation messages. No live service or firewall action ran.

Branch `codex/frame-gaze-streaming` starts at `c690841d`, paired with
the matching native Frame client in this repository.
This implements nonlinear base images and full-resolution gaze crops
in one video atlas. The Windows OpenVR renderer uses one existing encoder;
Frame decodes one stream and reconstructs four views of the immutable atlas.

The client advertises `{"frame_gaze_streaming":1}` only when its OpenXR runtime
supports eye gaze and the existing foveated-streaming preference is enabled.
Negotiation requires Windows, UDP, H.264/HEVC SDR 8-bit and gamma 1. Upstream
foveated encoding cannot run simultaneously with the SurfXR atlas.
The acknowledgment contains version 1 and base/focus dimensions, rounded up
to multiples of 32. The base uses one third of each full eye dimension; focus
defaults to one quarter. Dashboard's SurfXR features > Gaze streaming controls the
PC enable flag and focus width/height independently (10–33% of each eye dimension).
The server prioritizes enabled SurfXR gaze over saved upstream foveation for a
Frame gaze-capable client. Changes use the existing stream restart hash and
reinitialization path. No legacy-client compatibility branch was added.
For 3072x3264 eyes: base 1024x1088, focus 768x832, atlas 2048x1920.

The atlas has left/right nonlinear bases in the top row; the bottom row has
left/right 1:1 focus crops, each in a base-width slot with black padding.
The coordinate map is the shared piecewise base map. Crops use UV = gaze/2 + .5,
are clamped to the full eye, and have even integer origins for NV12 alignment.

When negotiated, native tracking headers are followed by bincode 2 standard
`Option<FrameGazeSample>`: sampled_ns, origin_epoch, and per-eye valid/xy.
Validity requires finite coordinates in [-.5,.5], the same origin, and age
below 100 ms in both the Frame clock and PC receipt clock. Repeated samples
retain their original receipt time. Missing/future/stale gaze uses centered
crops when no fresh per-eye value remains; invalid eyes retain their last valid
value for at most 100 ms. Focus loss and origin changes clear the per-eye cache.

`server_core/frame_gaze` retains at most 1024 tracking records and 1024 actual
compositions. `GazeAtlas::Render(timestamp)` selects and records crops before
the GPU packs that frame. Repeated compositions of one timestamp reuse their
recorded crops. Video sending looks up that exact composition, then appends
four little-endian u32 crop origins after optional source haptics and before
NAL bytes. The native header, NAL bytes and encoder reference chain remain
unchanged. These 16 bytes have no Option tag or length prefix.

Validation uses `cargo test -p surfxr_sockets --lib`,
`cargo test -p surfxr_server_core --lib frame_gaze`, and
`cargo build -p surfxr_server_openvr --release`. To check the actual GPU packer,
run in a Windows x64 VS developer prompt from this repository:

```bat
cl /std:c++17 /EHsc /O2 /MD /wd4005 /DNOMINMAX /D_WINSOCKAPI_ /Isurfxr\server_openvr\cpp /Iopenvr\headers surfxr\server_openvr\cpp\tools\gaze_atlas_test.cpp surfxr\server_openvr\cpp\tools\frame_worker_test.cpp surfxr\server_openvr\cpp\tools\pose_history_test.cpp surfxr\server_openvr\cpp\surfxr_server\PoseHistory.cpp surfxr\server_openvr\cpp\platform\win32\frame-worker\FrameWorkerParent.cpp surfxr\server_openvr\cpp\platform\win32\frame-worker\FrameWorkerMailbox.cpp surfxr\server_openvr\cpp\platform\win32\EncoderFrameMailbox.cpp surfxr\server_openvr\cpp\platform\win32\GazeAtlas.cpp surfxr\server_openvr\cpp\platform\win32\d3d-render-utils\RenderPipeline.cpp surfxr\server_openvr\cpp\platform\win32\d3d-render-utils\RenderUtils.cpp /Fotarget\ /Fetarget\gaze_atlas_test.exe /link d3d11.lib dxgi.lib
target\gaze_atlas_test.exe surfxr\server_openvr\cpp\platform\win32\QuadVertexShader.cso
```

The fixture uses D3D11 WARP and production renderer methods without SteamVR,
services or network. It checks nonlinear texel centers, 1:1 focus, movement,
eye seams, padding and sRGB. Headset rendering, gaze calibration, image
quality, encoder latency and wearer acceptance require a separate live test.
No driver or session settings are installed or changed by these checks.

The fixture also checks the production independent-device encoder mailbox,
cross-process NT texture leases and bounded private-Job teardown.
Ownership, startup-load isolation and numeric telemetry contracts are in
[FRAME_STREAMING_LOAD.md](FRAME_STREAMING_LOAD.md).

Frame UDP video can additionally negotiate `frame_video_fec` version 1 with
`data_shards:16`. Only an explicit capability plus the UDP split acknowledgment
enables it. Each data shard uses MTU minus 18 payload bytes, leaving four bytes
for the parity packet's original complete payload length. Prefixes retain the
14-byte stream/index/count/part shape. Data parts are `[0,count)`; parity parts
are `count + group`, one per sixteen consecutive data parts. The parity payload
is little-endian u32 total length followed by a zero-padded XOR of that group.
Parity follows each group and shares the video pacer's byte budget. Normal
complete packets do not wait for parity; one absent data part per group can be
restored exactly, including a short last part. Larger bursts retain IDR recovery.
Legacy video and all priority stream bytes remain unchanged. A failed send
advances the packet index even if only a prefix was transmitted, preventing
different encoded pictures from reusing a partially delivered packet identity.

For constant bitrate, the negotiated Frame UDP writer uses twice the encoder
rate as its send budget (400 Mbps for 200 Mbps video). It waits every four
data shards to reduce bursts; FEC still covers sixteen data shards and parity
bytes share that same budget. The normal writer and external process worker
use the same policy. Encoder bitrate, payload bytes, FIFO ordering and adaptive
bitrate behavior are unchanged.

`frame_video_timestamps` v1 is explicitly negotiated for Frame gaze. It appends
one raw little-endian u64 after source crops, before NAL bytes. The original
video timestamp remains the exact tracking/crop association. The process worker
anchors its first real capture to that tracking timestamp and advances image
identity by actual monotonic PC capture intervals. Re-encoding a held lease
retains its capture identity; real source stalls remain gaps. No pose timestamp
is clamped, extrapolated or substituted. The in-process fallback appends the
original tracking timestamp, retaining its prior identity behavior. Unnegotiated
clients retain their exact original packet bytes.

Pose history is registered before publishing the pose to SteamVR. Matching uses
the complete submitted 3x4 transform, including translation, within the existing
bounded window. The WARP fixture also links `cpp/tools/pose_history_test.cpp` and
`cpp/surfxr_server/PoseHistory.cpp` to verify production matching and transforms.
Windows PostPresent retains its existing vsync grid, sleeps until 150 us before
the deadline, then waits the bounded remainder. Five-second `Frame pacing timing`
windows report wake lateness; they do not classify game/GPU stalls as timer delay.
`Frame present timing` separately records DirectMode call gaps, complete Present,
SubmitLayer, mutex/keyed-mutex waits, copy and statistics-call maxima, plus empty
layers and failure counts. These phases overlap and their maxima cannot be summed.
The measurements retain existing lock, texture ownership and pacing behavior.

`cargo test -p surfxr_sockets --lib` exercises the real UDP writer on reserved
ephemeral loopback ports. Set `SURFXR_FRAME_FEC_FIXTURE` to an ignored output path
to export synthetic datagrams, then replay them with the native wire test's
`--fec-fixture PATH`; this checks cross-language byte equality and actual
receiver recovery without contacting a live Streamer.
# Dashboard-managed desktop and in-process Frame mode switching (2026-10-11)

The companion Frame v21 viewer now keeps its PID/OpenXR session while closing one
media backend and opening the other. Dashboard owns desktop admission on TCP 19946
using the existing Devices trust list. Single-use, source-IP-bound tickets expire
after 15 seconds and are never written as pairing files; active desktop permissions
are renewed through the local broker. Disabling desktop, untrusting the device or
changing input/audio permission closes that session. Encoding settings apply on
the next desktop connection.

Settings → Extra → Steam Frame Desktop controls desktop access, input, sound,
SYSTEM requirement, framerate and bitrate. Desktop shows host/session and SCM
service status and offers explicit elevated install/update/start/stop/uninstall.
Dashboard starts its owned user host when the SYSTEM service is not running. Old
key-file services must be explicitly stopped and updated to `--dashboard-managed`;
startup/build never silently replaces a running service.
The existing explicit Add firewall rules action includes the bundled desktop host
and the installed SYSTEM host path; Remove firewall rules removes both rules.

The Windows desktop host remains in `../desktop`.
Build it with the existing `../tools/build/build_surfxr_windows.ps1` entry. The
existing `cargo xtask build-streamer` bundles the completed exe/DLL/license output
beside Dashboard, either from the repository's `build/pc/native/Release`
or `SURFXR_FRAME_DESKTOP_DIR`. An explicit missing output fails packaging; no new
additional build entry was introduced.

`server_io/frame_media_lock` shares the Windows media semaphore with the desktop
host. The VR pipeline acquires before any audio/video owner starts, without holding
the session lock while waiting. It and the OpenVR encoder event keep references
until transport joins and DeinitializeStreaming complete. Desktop holds through
capture, encoder, sender/audio and policy joins. No second encoder starts on timeout.

Validation: Windows native desktop Release and 21 CTest checks; ARM64 viewer Release
with zero warnings and 33 CTest checks, including same-PID launcher requests and
managed encrypted desktop admission on reserved loopback endpoints. Dashboard
admission/revocation plus existing discovery/start tests, four session/config
migration checks and the existing incoming Frame trust test pass. Dashboard and
OpenVR driver Release binaries build (only MSVC linker informational warnings).
No installed service/runtime was replaced or restarted. These are build/offline
checks; actual headset mode rendering and wearer acceptance remain unverified.

Live deployment follow-up (2026-10-11): installed the managed SYSTEM desktop host,
Dashboard/driver/worker and the companion Frame viewer, preserving trust/settings.
The first VR→desktop attempt exposed that GET `/api/session` only emitted a Session
event. Commit `a13153c5` retains the event and returns the same driver-owned session
as JSON for synchronous desktop admission. A reserved-endpoint HTTP test passes;
Desktop UI now refreshes every 500 ms. Updated PC Release binaries then completed
three desktop→VR→desktop cycles with Frame viewer PID 117428 and /proc start ticks
3027111 unchanged. Old-mode media sockets/encoder contexts disappear, old-mode
frame counters stop, and desktop's 10-second observation has no VR encode/send
windows. Desktop status visibly reports streaming/SYSTEM; SteamVR remains idle in
desktop mode without a VR worker. Headset OpenXR rendering was observed; wearer,
image/input/audio quality and long-run/game acceptance are still unverified.
Sanitized hashes/counters are recorded in the companion project's
`surfxr/performance/2026-10-11-surfxr-mode-switch-live-validation.json`.

# Frame boundary stability and center (2026-10-18)

Negotiated Frame VR
and desktop tracking now use STAGE directly. PlayspaceSync cannot recenter from
a last UDP head pose belonging to the previous Stage. Legacy peers retain their
configured recenter mode; stored settings and tracking wire bytes are unchanged.
The existing Chaperone owner computes the full wall polygon's area centroid,
places standing/seated there at floor height and expresses walls relative to it.
Raw driver poses remain in STAGE, preserving head-to-wall distances. The center
does not depend on vertex density or the user's position at connection time.
The existing five-second boundary retry checks actual live origin, dimensions
and walls before deduplicating, allowing runtime resets to be repaired.
Failed commits revert the working copy and remain retryable.

Validation: server_core 31/31 tests, Windows OpenVR Release build (only MSVC
linker informational warnings), and the production FrameBoundary helper through
`g++ -std=c++17 -I openvr/headers surfxr/server_openvr/cpp/tools/frame_boundary_test.cpp -o /tmp/frame-boundary-test`
then `/tmp/frame-boundary-test`. The fake runtime checks asymmetric/tessellated
geometry, both windings, preserved physical coordinates, live drift and failed
commit recovery. Companion Frame ARM64 Release and 33/33 CTest also pass.
No runtime deployment, actual headset rendering or wearer acceptance this round.

# Frame controller offline presentation (2026-10-18)

Steam Frame emulation declares its actual wireless identity alongside the native
icon properties. The original claim that wireless identity fixes inconsistent
offline presentation was disproved by the longer live observation below.

SteamVR 2.17.10 reports both controllers disconnected after hot-disable, but its
activity state can later become Standby and override the offline status shown
by vrmonitor. After the user woke both controllers, a live enable/disable cycle
produced connected=true/true then false/false. With an experimental 250 ms
bounded disconnect publication, both stayed disconnected throughout 332 seconds,
but both activity levels changed to Standby at the final sample and the panel
showed standby icons again. Wireless identity alone failed the same long wait.

The bounded publication and neutral-quaternion experiment was withdrawn. Normal
pose publication and input release are preserved. No global power setting,
fabricated input, runtime binary patch, or private runtime ABI workaround is
installed. The remaining panel behavior is unresolved; build success and actual
connection flags do not establish panel or mixed-controller game acceptance.
Sanitized evidence is in the companion repository's controller status document.
