# Companion unified Frame daemon (2026-10-12)

The current `FrameDaemonWorkerRunV7` gives the existing shard/FEC/pacing writer a
synchronous borrowed send callback. The paired native daemon sends directly to
the authenticated client; the same-process UDP video relay is removed. Shared
GPU mapping version 6 and legacy socket/V6 exports remain compatible. Retirement
blocks new callbacks, waits only an in-flight send, and clears Session captures;
the actual GPU/sender keeps its media lease until joined. Existing bounded desktop
admission remains in effect. Worker cancellation and control bridging use events.

Settings -> Extra -> Steam Frame Desktop -> **Encrypt unified streaming media**
defaults off and applies to the next unified connection. New peers explicitly
negotiate Plain UDP media (20-byte session cookie/sequence/generation prefix) or
the original AES-GCM framing (28-byte overhead). Plain has no media confidentiality
or cryptographic integrity; authentication and reliable control remain encrypted.
An old client/host keeps AES, with no packet-decode fallback. The selected overhead
is passed into the existing video pacer. The native Frame client reads video and
reassembles on one owner, transfers complete compressed packet ownership and waits
for HEVC device/release notifications. The historical V6 measurements below do
not validate this new path's power consumption or wearer experience.

The final V6 deployment passed ten paired real-headset switch groups. Completion
requires all three desktop decoders, twenty fresh frames on each panel, and
ready input: desktop 1.094–1.235 s, VR 0.932–0.990 s. Explicitly suspending the
real vrserver for six seconds retained the VR session; desktop completed while
the source was still suspended in 1.235 s. Runtime, viewer, SYSTEM daemon,
authenticated connection, UDP and single audio owners remained unchanged.
Closing SteamVR through its existing API restored desktop/input in 1.213 s;
process shutdown wait and decoder-retirement observation are measured separately.
The same client and daemon stayed running, including a subsequent 30-second
desktop observation. These are automated XR checks, not new wearer acceptance
or guarantees of frame rate under GPU saturation. Microphone meters completed
without invalid/clipped samples but received silence, so speech quality is not
revalidated. The updated production Parent passed the existing WARP offscreen
fixture; Dashboard's three CPU tests, paired Windows checks and 33 ARM64 offline
tests also passed. Detailed evidence categories are in the paired SURFXR_DAEMON.md.

The paired native project now builds `surfxr-daemon.exe` and
`surfxr-desktop-module.dll`. The LocalSystem console-session agent owns desktop
and VR encoding, external media sockets, loopback audio and virtual microphone
playback. SteamVR remains the source of VR pictures and tracking events. Its
shutdown closes the GPU session, not the daemon process.

`server_openvr/frame_worker` exports reusable worker, audio, capability-patching
and qWAVE entry points. The worker context can be opened and closed repeatedly
inside the daemon. `FrameWorkerParent` attaches mapping/wake/texture handles via
the local pipe and does not create a private encoding child for negotiated
daemon connections. The daemon owns the private video endpoint.
Private GPU IPC version is **6**; update the matching driver and daemon together.
The SYSTEM daemon creates its own private video sender and passes it within the
same PID to `FrameDaemonWorkerRunV6`. It does not duplicate a user vrserver's
Winsock descriptor into SYSTEM. The private receiver checks the daemon sender's
published source port; original shards, pacing, FEC and external ports remain.
Legacy clients retain their private child process/socket export path.
V6 transfers the unified media lease to the actual daemon encoder. Source
retirement can stop and join that encoder even while vrserver is stalled;
GPU/sender cleanup precedes lease release. Legacy connections keep their original
lease owner. The V5 export remains a rejecting stub to prevent unsafe mixed installs.

Dashboard keeps its Remote settings owner while the same vrserver PID/start time
remains alive despite HTTP delays. The desktop broker reads that owner's bounded
persisted session for current permissions; invalid/missing settings do not renew
the existing three-second lease. Partial source TCP packets and transient source
silence no longer imply outer authenticated-session loss. Confirmed source closure
still triggers desktop fallback.
The driver stops accepting captures after requested retirement. Parent::pump
checks genuine shared failure first, then recognizes the requested stop before
checking pipe liveness; normal daemon retirement no longer shuts down SteamVR.
The existing WARP process fixture checks cooperative retirement, rejection of
new captures, and failure reporting even when stop is already set. The paired
bounded source queue also coalesces duplicate keepalive and IDR requests.

The native client authenticates once and retains TCP 19950 / UDP 19951 / UDP 19952
across video-source changes. Initial capabilities are exchanged once; subsequent
source requests carry mode and generation. Audio/microphone owners and cipher
sequences remain live. Reliable control/tracking uses the existing server_core
through private loopback endpoints. The daemon supplies the authenticated
hostname, real peer IP and user-selected Windows audio endpoint IDs. Driver
audio capture/playback is disabled only for that explicit local capability.

The existing Frame media lease and native daemon encoder-idle barrier exclude
overlapping encoders. Original gaze, tracking timestamps, haptics, FEC, optional
capture/mirror paths, bounded queues and GPU keyed-mutex lifetimes remain.
Daemon overhead is included in pacing; physical sockets reuse the existing
qWAVE implementation. Original non-daemon clients, standalone desktop pairing,
AMF/VPL/software and diagnostic in-process paths are preserved. Unified mode
currently requires the paired desktop's NVIDIA/NVENC path and rejects fallback
encoding in vrserver.

Dashboard retains Devices/settings admission and a bounded session lease.
Tickets advertise unified support and the actual host port; the native daemon
reuses the same broker and does not persist another key or account. Settings
owner handoff during SteamVR exit/restart can retry within the existing lease;
explicit revocation still ends media immediately. Closing Dashboard ends the
managed session while the service agent remains available.

Trusted explicit Frame VR-start requests reuse the launcher's local SteamVR
route regardless of the Dashboard `direct_launch` preference. Steam's app-250820
launch can wait indefinitely at `SynchronizingControllerConfig "syncfailed"`;
that cloud-sync dialog must not gate headset streaming. Controller power or
connection is not a launch prerequisite. Driver registration, one-shot start
authorization, existing-runtime checks and child reaping still use the same
launcher. Discovery and untrusted requests cannot start SteamVR, and duplicate
requests retain the ten-second launch throttle. Manual Dashboard launch keeps
its existing preference.
Both existing Dashboard CPU tests passed. With `direct_launch=false`, manual
SURFXR enablement off and Dashboard automatic launch off, an explicit headset
request cold-started SteamVR and produced real VR frames despite the existing
Steam controller-config sync failure. No controller/cloud-sync setting changed.

Build the paired native target first:

```powershell
../../../Build-PC.ps1
$env:LIBCLANG_PATH = (Join-Path $PWD 'deps/libclang/clang/native')
$env:SURFXR_FRAME_DESKTOP_DIR = (Join-Path $PWD '../../../build/pc/native/Release')
cargo xtask build-streamer --release --keep-config
```

The existing xtask packages daemon/module/driver plus OpenVR, libvpl and FFmpeg
dependencies after copying the driver dependencies. Run the packaged daemon's
`--self-test` for actual DLL imports/exports and offline framing, without devices.
The existing explicit service update/start and firewall actions include both
packaged and installed daemon paths; building does not install, stop or restart
a user's service or SteamVR. The SCM name and install directory remain
`SurfXRDesktop`, while the installed executable is `surfxr-daemon.exe`.

Validation: companion Rust builds/tests, native Windows CPU IPC and desktop
checks, native 12-generation authenticated loopback test, synthetic three-output
NVENC and existing WARP/real-child texture lease checks. These are build/offline
and offscreen evidence. After the headset became available, the paired project
completed a full native ARM64 viewer/launcher build with 33 offline CTests and
zero compiler warnings, then installed both ends and enabled the SYSTEM daemon.
Actual owner/module inspection confirmed desktop, driver and NVENC in the same
SYSTEM agent, with no private encoding child.

Ten live mode-switch groups (six normal and four three-request bursts) retained
the viewer PID/start ticks, daemon PID/creation time, TCP peer port and UDP
owners. Audio had one playback connection and zero reconnects. Inactive decoders
stopped and retained mappings did not grow; stable VR samples were about 144 new
stereo frames/s. Completion observations were 1.38–1.43 s to VR and 6.24–6.44 s
to three-output desktop, including status publication and twenty fresh frames,
not exact first-frame latency. Desktop initialization/source teardown still takes
several seconds despite eliminating reauthentication.

Follow-up timing isolated roughly 2.1 s in media-lease retirement. Only an
admitted local daemon split UDP connection now uses a 20 ms receive poll after
the unchanged handshake. Legacy receive/keepalive deadlines and encoder/sender
joins, GPU lifetimes and lease-release order are preserved. All 25 socket CPU
tests passed, including an actual idle production receiver deadline.
Ten additional live switch groups retained connection, audio and process owners.
The paired status worker's bounded numeric first-XR-presentation record measured
1.136–1.359 s to desktop and 0.644–0.757 s to VR. The older completion observation,
including status sampling/twenty fresh frames, fell from about 6.4 s to about 4.4 s.
This is XR submission timing, not optical latency or simultaneous first pictures
on every desktop panel. The paired daemon also bounds/coalesces move controls and
backpressures transient input bursts while preserving key/button/release edges.

Final deployment repeated ten live groups with the same viewer, SYSTEM agent,
TCP connection and UDP owners; first XR submission was 0.887–1.394 s to desktop
and 0.611–0.774 s to VR. A separate five-minute desktop observation had 151 clean
samples, 17,904 new desktop frames and no process exit. A final SteamVR shutdown
again retained the viewer, daemon, connection and single audio owner and returned
to desktop. These are automated headset rendering checks; no new wearer
acceptance was requested. A 30-second virtual microphone meter completed without
invalid/clipped samples but received silence, so it does not prove speech quality.

Closing PC SteamVR through the existing SURFXR API returned the headset to desktop
with the same connection, viewer and SYSTEM agent. The temporary direct-launch
setting was restored. The wearer confirmed desktop/VR pictures and audio, and
desktop/audio after SteamVR exit. Existing numeric microphone checks received
speech during a 60-second switching window and 30 seconds after exit, without
invalid or clipped samples; application call listening was not tested.
No captured audio/video or packet payloads were saved. Detailed contracts and
validation boundaries are in the paired project's `surfxr/docs/SURFXR_DAEMON.md`.
