# SurfXR link contracts

The client backend is SurfXR only. SurfXR product releases start at 1.0.0, based on ALVR. Product versions do not select wire layouts. The sole product protocol uses the `21.0.0-dev14` wire revision. Historical v20 codec vectors remain offline regression fixtures; they are not a selectable client mode. Both endpoints of SurfXR extensions are included in this repository. The PC Dashboard owns trusted clients and accepted settings. Configure the selected PC address or use v21 discovery on the selected Frame interface.

## PC

`pc/surfxr-server/surfxr/dashboard` provides the SurfXR UI. `server_core`, `server_openvr` and the frame worker provide SteamVR rendering, tracking and encoding. `pc/daemon` owns the persistent service and desktop/VR backend selection. `pc/desktop` provides desktop duplication, audio, native input and authenticated desktop media. Build all of them with the `scripts/Build-PC.ps1` entry.

The private PC-local `ALVRDB01` broker accepts opcode 11 only from loopback: the existing consumed 32-byte ticket ID, matching four-byte client IPv4 and identity flags authenticate one retained notification socket. Reply 0 accepts the watch; reply 2 rejects/revokes it. Dashboard sends byte 2 on this socket for explicit session disconnect or confirmed full exit, before SteamVR shutdown and worker joins. Native owners poll without waiting and close the client transport before normal resource retirement. Watch EOF alone on the host preserves the existing heartbeat fallback; an older Dashboard closing the unknown opcode is supported. The unified daemon's 15-second silence grace and Dashboard's 20-second ticket retention remain unchanged. Immediate full-exit notification requires the updated PC service as well as Dashboard; old services retain heartbeat behavior.

Heartbeat opcode 2 retains its layout: flag bit 1 means an active source, and additive bit 4 identifies VR rather than desktop. No active source means connected/idle, including after End or source completion. Existing hosts lacking bit 4 retain the previous desktop label. Devices projects these process-local sessions without changing the SteamVR-owned `connection_state` or persistent trust. Watch closure retires the corresponding consumed ticket/status; disconnect targets its exact ticket so a later reconnection is unaffected. No headset wire bytes, settings format or desktop module ABI change.

## Frame

The native connection service starts `surfxr-viewer` with SurfXR parameters. v21 can keep one authenticated connection while switching desktop/VR media sources. The session parses negotiated configuration before delivering media callbacks; unsupported codecs/configurations fail explicitly. Frame defaults to hardware decoding and bounds pending media, tracking and upload queues.

The tracking ledger associates echoed SurfXR Duration values with original local queries and Stage epochs. Negotiated `frame_pose_time` sessions sample head and both grips at upload time, retain velocities and map sample age into the PC clock so SteamVR owns prediction. Existing v21 video views carry the actual submitted image pose. Native Frame VR also requires `frame_clock_calibration`: software exchanges measure clock offset, frequency difference and uncertainty, and every head/controller packet carries its calibration evidence. The calibrated timeline cannot switch to legacy prediction or fixed-delay timing. See [the timestamped pose contract](FRAME_POSE_TIME.md) for layout, evidence and continuous holdover. Haptic packets and video haptic levels feed the same bounded two-hand mailbox with explicit expiry and focus handling.

Current clients request calibration capability revision 2: first valid reply starts the timeline, quality/reply age are advisory and reply loss uses continuous holdover. PC acknowledges this policy explicitly; the existing 40-byte clock reply and 80-byte tracking suffix remain unchanged. Revision-1 acknowledgements cannot satisfy a new client's holdover requirement. Calibration-quality degradation does not disconnect the stream or drive display-delay changes; actual connection and resource failures remain separately owned.

## SurfXR extensions

SurfXR provides native Frame controller state, battery status, microphone recovery, boundary/IPD synchronization, accepted video settings, desktop input, stream lifecycle and SteamVR startup status. The gaze atlas carries base-eye imagery plus high-resolution focus crops. CPU geometry and generated shader math must remain consistent with PC packing and the negotiated metadata. Ordinary SurfXR sessions use their negotiated capabilities; unsupported extensions are not guessed.

The settings request keeps its 25-byte layout. Action bit 7 explicitly requests the extended snapshot; low action 5 edits simultaneous desktop pointer priority (0 VR, 1 PC). Extended snapshots append one f64 policy value to the existing 40-byte snapshot. Requests without the bit still receive exactly 40 bytes. Both endpoints and the authenticated Dashboard relay implement this extension together. The private desktop module ABI retains the live policy callback introduced in v2; the current ABI remains v2.

Quality tiers, FPS and bitrate are PC-owned accepted values. Client preference caches cannot invent acknowledgement. Local timewarp, image processing, overlay visibility and input capture remain local controls. An out-of-date settings response must not overwrite a newer accepted serial.

## Operation and evidence

Use `scripts/Start-Frame.ps1 -Plan` to inspect orchestration. `-BuildOnly` compiles and runs offline validation. Installation is a separate action. Configure/trust Frame in the PC Dashboard, then choose Desktop or SteamVR from the Frame dashboard. For release packaging see `RELEASE.md`.

Build, protocol-loopback, CPU model checks, offscreen GPU rendering and actual wearer experience are distinct evidence categories. Consult `VALIDATION.md` for validation requirements for the selected commit.

## Pose composition and acceptance

The current [timestamped pose contract](FRAME_POSE_TIME.md) defines clock
calibration, prediction ownership and immutable source-image poses. PC and Frame
must be released together. Offscreen composition checks do not establish headset
rendering or wearer acceptance; see [TESTING.md](TESTING.md).

## Desktop capture control v1

The multi-monitor PC configuration advertises `frame_desktop.capture_control: 1`. Only a negotiated client sends control type 15. Modern integer encoding carries `(type, version=1, layout_generation, request_serial, mask)`, each declared u32; the four mask bits retain the existing source IDs. Generation and serial must be nonzero, mask is 0..15, and trailing bytes/unknown versions are rejected. Type 16 echoes the exact tuple after the video owner adopts the mask and discards queued previous-policy output. All-off is valid; it does not close the authenticated session or audio. The Frame client bounds requests to one in flight, coalesces newer intent, and ignores unmatched/stale acknowledgements. A new layout invalidates the pending request and reissues current intent with the new generation. Legacy mono bytes remain unchanged.

Capture-off skips desktop duplication polling, cursor composition and encoder work for that source, preserving its capture/encoder objects and source ID. Resume requests an IDR from the retained image owner. Decoder demand/epoch handling remains independent of capture intent: headset visibility/focus/VR transitions do not manufacture capture acknowledgements. Capture buttons are disabled without the advertised capability; they never silently act as local-only hide controls. Telemetry exposes supported/requested/accepted masks through the existing viewer status writer.

## Configurable keyboard shortcuts

All generic shortcut-bar entries use existing version-1 physical keyboard events, with no input-method capability or semantic action extension. The client locally records and persists scan-code chords, then sends ordered downs and reverse ups surrounded by neutral releases through the existing authenticated queue. Candidate capability bodies [1]/[2], image trailer/token encoding and candidate-click body version 2 are unchanged. Private desktop module ABI is v2; there is no input-method callback or broker action.

## Desktop keyboard candidate overlay v1

The client advertises `frame_desktop_keyboard: 1` alongside `frame_keyboard` in its authenticated desktop capabilities. Only a client advertising this extension receives a generation-zero Keyboard capability body `[1, 1]` or `[2, 1]`: the first byte retains image-only/clickable meaning and the second byte acknowledges desktop overlay v1. Clients without the extension still receive exactly `[1]` or `[2]`. New clients accept these legacy replies and leave desktop overlays disabled.

In desktop mode, negotiated peers reuse generation-scoped Keyboard visibility `[0, visible]`. The server sends Keyboard body `[1, encoded_image...]` at the existing 10 Hz capture cadence, with one latest pending image and no FIFO of captures. `encoded_image` is the unchanged bounded candidate image/token codec (at most 65536 bytes); `[1]` clears the candidate. Images with stale generations are ignored, current images require negotiated desktop mode, and malformed images fail closed. The client clears its candidate on every mode generation. This uses the authenticated reliable control connection, so all-off desktop capture still allows candidate overlays.

Candidate clicks keep the original type-11 version-2 body and token. Desktop clicks use that envelope directly, without the monitor-layout wrapper; ordinary mouse/key/release events keep their existing layout-aware route. The candidate owner intercepts only version-2 candidate bodies in desktop mode, validates token, freshness and target, and cannot consume ordinary desktop releases. VR candidate suffixes and private desktop module ABI v2 are unchanged. No configuration format or IPC acceptance serial changes.
