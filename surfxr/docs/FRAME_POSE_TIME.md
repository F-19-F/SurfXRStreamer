# Timestamped Frame head and controller transport

This defines sample-time and prediction ownership, using SurfXR transport and public OpenXR. It does not copy Steam Link's private packets, runtime APIs or compensation constants. Both endpoints must negotiate it. Floating-window wearer acceptance remains separate from offline validation.

## Owners and negotiation

| Owner | Responsibility |
|---|---|
| `frame/xr/current_tracking.hpp` | Fresh current-time head and grip locates; handle and Stage lifetime |
| `frame/session`, `frame/app` | Sampler availability, callbacks and session mode |
| `frame/surfxr/clock_fit.hpp`, `pose_clock.hpp`, `upload.cpp` | Measured clock model, continuous mapping, serialization and ledger registration |
| `shared/surfxr/include/surfxr/surfxr_pose.hpp` | Fixed little-endian records |
| `server_core/src/frame_pose.rs`, `tracking/receive.rs` | Clock replies, validity/order and exact identity lookup |
| `server_openvr/src/events.rs`, `tracking.rs` | Prediction ownership, velocity axes and publication |
| `PoseHistory`, direct-mode capture, `render_pose.rs` | Immutable actual source-image pose |
| `server_core/src/frame_clock.rs` | Calibration negotiation and per-packet evidence validation |

Native VR requests `"frame_pose_time":1` and `"frame_clock_calibration":2`. Calibration capability revision 2 explicitly acknowledges first-reply startup, continuous holdover and advisory quality evidence. The four-timestamp reply and 80-byte pose suffix are unchanged. New PC still acknowledges revision 1 for older clients; a new client requires revision 2 so an old PC cannot silently discard its holdover poses. A missing capability is a configuration mismatch, never a switch to arrival-time or legacy prediction. Desktop-only tracking and historical v20 fixtures remain separate. No saved setting or reserved-slot control is added.

The XR thread synchronizes actions and publishes handle, optics, focus and input snapshots. The upload thread locates head and both active grips at one converted `CLOCK_MONOTONIC` instant each tick, four times the negotiated display rate (480 requested queries/device/s at 120 Hz). These are fresh runtime queries, not a sensor-frequency claim. Cached input snapshots expire after 100 ms. A mutex/barrier prevents queries across session teardown; session/instance/reference-space events clear availability. Queries across a pending Stage boundary are rejected.

Position, quaternion and valid linear/angular velocities come from `xrLocateSpace` with `XrSpaceVelocity`. Invalid velocities become zero; unavailable controller poses are omitted instead of reusing the legacy controller cache. Existing focus, forwarding and UI-capture policy applies. OpenXR reports velocity in base-space axes; PC converts angular velocity into final device axes, including controller offsets, and retains the linear lever-arm contribution. See [OpenXR's pose/velocity contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrLocateSpace.html).

## Clock records

All fields below are fixed little-endian within the unchanged SurfXR bincode envelope. Request: client `ReservedBuffer` variant 11. Reply: server `ReservedBuffer` variant 6.

| Request offset | Type | Meaning |
|---|---|---|
| 0 | u32 | `0x31435645` (`EVC1`) |
| 4 | u32 | Version 1 |
| 8 | u64 | Client send t1, steady-clock ns |

The reply is exactly 40 bytes: the same magic/version followed by four u64 fields: server session generation, echoed t1, server receive t2 and server send t3. Frame captures receive t4. PC uses one process monotonic clock; epochs are not presumed equal.

## Measured software calibration

The client and PC clocks have independent origins and rates. The client requests exchanges every 25 ms until a first reply, then every 250 ms, with at most eight pending requests. A full pending window also uses the slower cadence rather than continually evicting delayed requests. It accepts only a matching nonce/generation and ordered, numerically bounded timestamps. There is no RTT quality cutoff: a queued reply contributes a wider interval. Server processing time is separated from network time; their units are related by the fitted clock rate rather than assuming identical rates.

`clock_fit.hpp` fits `S(C) = server_anchor + (C-client_anchor)*(1+skew) + offset` to at most 64 exchanges over 16 seconds. Each clock is first reduced against its own anchor to avoid subtracting large unrelated epochs in floating point. Nonnegative path delays give two measured constraints per exchange: `S(t1) <= t2` and `S(t4) >= t3`. The intersection is a feasible offset/rate polygon. Its rate midpoint and conditional offset midpoint supply the estimate; all polygon vertices supply the error interval for the timestamp being mapped. Equal upstream/downstream latency is **not** presumed. A 1 microsecond software-clock precision floor is included; rates outside +/-5000 ppm are rejected as incompatible with the nanosecond-clock contract. These are model/precision bounds, not a claim of perfect physical synchronization. The four-timestamp offset, delay and frequency concepts are described in [RFC 5905](https://www.rfc-editor.org/rfc/rfc5905.html); this implementation uses interval constraints instead of treating its midpoint formula as exact.

The first valid exchange establishes the clock epoch and allows poses immediately. There is no sample-count, observation-span, quality-threshold or forecast wait before VR. Frequency estimation continues in the background. Later exchanges preserve the existing base rate while it remains inside the measured rate interval and preserve the mapped phase while it remains inside the measured time interval. The client does not follow a moving midpoint caused by asymmetric queue delay. Only measurements excluding the existing estimate can discipline it. Any phase correction is applied continuously over at most two seconds, capped at 1000 ppm; the correction ends even when subsequent replies disappear. There is no display-delay actuator, buffer enlargement or time-base substitution. Reported uncertainty includes the difference between the continuous mapping and feasible clock models; it is never clipped to pretend that synchronization is more accurate than measured.

Invalid, replayed, foreign-session and inconsistent exchanges are counted and rejected individually. Missing or delayed replies continue the same clock, with growing uncertainty visible in telemetry. The former 15-second acquisition timeout, three-second calibration expiry and 5 ms uncertainty exception are removed. Calibration quality cannot disconnect a healthy stream or switch its mode. A new connection gets a new owner and needs one measured reply for its unrelated epoch. Actual socket closure, authentication revocation and media failure remain separate lifecycle conditions; no software can maintain delivery over a completely unavailable link.

Existing `tracking_upload` telemetry retains the latest actual four timestamps (`clock_t1` through `clock_t4`) and reports `clock_ready`, sample/accepted/rejected counts, revision, signed offset, mapping rate and measured rate interval, minimum RTT, reply age and mapping uncertainty. Rate fields are server nanoseconds per client nanosecond multiplied by 1e9; subtract 1e9 and divide by 1000 for ppm. They distinguish an estimate from its uncertainty; a small residual in another feedback loop is not clock accuracy.

## Tracking suffix

The normal `TrackingData` layout is unchanged. Calibrated packets append 80 bytes **after** the optional gaze payload:

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | `0x31505645` (`EVP1`) |
| 4 | u32 | Version 2 with required clock calibration |
| 8 | u64 | Server session generation |
| 16 | u64 | Nonzero Stage epoch |
| 24 | u64 | Nonzero fresh query sequence |
| 32 | u64 | Original sample time mapped into PC monotonic ns |
| 40 | u64 | Original client sample time, steady-clock ns |
| 48 | u64 | Nonzero calibration revision (one or more observations) |
| 56 | u64 | Mapping uncertainty in ns, including continuous-clock correction |
| 64 | u64 | Applied clock rate in parts per billion (1e9 means equal rates) |
| 72 | u64 | Client timestamp of the last accepted calibration reply |

PC requires the v2 suffix in calibrated sessions; it rejects a 48-byte v1 suffix, zero/decreasing revisions, zero uncertainty, invalid clock-rate units and measurement timestamps later than the packet's local send timestamp. Uncertainty magnitude and reply age are advisory, not pose-admission gates. The v1 layout remains readable only for separately negotiated older clients, never as a transition from a calibrated session.

SurfXR `poll_timestamp` remains the client send time and ledger/video identity, never a PC-clock value. PC strips the suffix before gaze parsing. It requires one valid head and at most one of each hand, finite transforms/velocities and near-unit quaternions. Wrong generations, old epochs, duplicate/regressing sequences or send times and locally stale samples older than 200 ms are dropped individually. Mapped pose age uses the same 200 ms plausibility range in either direction; the former independent 5 ms future-quality cutoff is removed. This prevents physically implausible prediction without treating modest clock-estimate error as stream failure. The bounded 1,024-entry identity/time history clears on epoch change. Disconnect marks the negotiated owner stopped and clears its samples; queued events cannot fall back to legacy prediction.

## Prediction and returned image

Frame requests current remote poses in this mode. PC bypasses SurfXR extrapolation, velocity cutoffs and velocity multiplication. Both head and controller OpenVR poses describe the original sample, with `poseTimeOffset = mapped_sample_time - current_PC_time`; SteamVR owns extrapolation. Accelerations remain zero. Calibrated sessions have one prediction path and do not fall back to legacy prediction.

`PoseHistory::GetFramePose` takes an identity from the latest real sample but retains actual layer-zero `mHmdPose` as the composition target. It removes the universe transform before storing that source pose in driver coordinates. `RecordFramePose` accepts an identity once, before capture: duplicate submissions cannot rewrite an accepted image's metadata. Capture timestamps, GPU IPC, leases, fences and eye association retain their existing ownership.

The existing v21 video header already carries global per-eye poses/FOV. The frozen source head pose and existing optics generate these views; the existing recenter inverse converts them before transmission. Frame uses them for that decoded image, while its ledger supplies original query and Stage epoch. Worker repeats retain immutable source metadata. No new video layout or timing collector is added. Steam Link's private compositor prediction-time parameter is not reproduced: public OpenXR presentation and local timewarp retain their owners. Rotation-only composition cannot reconstruct depth-dependent translation/parallax.

## Display composition

New video images retain their immutable source poses for OpenXR composition. Enabled local rotational correction reconstructs retained images from original decoder pixels, querying both eyes at the unchanged local `predictedDisplayTime` after staging/prior GPU waits. The resulting views describe the corrected orientation. Source positions/FOV, Stage checks, leases and GPU fences keep their owners. This is rotational correction without depth; near-field translation/parallax remains a separate issue. See [the OpenXR late-query contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrLocateViews.html).

Clock calibration retains ClientStatistics submit-time semantics. `vr_timewarp_frames` counts retained-image corrections; new-image composition stays with the runtime.

## Link selection

Native `--deploy start` forwards the requested link through the existing launcher
and connection controller as `start.profile`. It applies that profile before
discovery/launch and rejects a different profile during an active session.
The isolated deployment IPC fixture checks a saved LAN profile, an explicit
direct-link request and a fake viewer. Compare actual interface, fresh-picture
rate, drops and repeated images when evaluating timing or rendering changes.

## Checks and wearer acceptance

Clock quality never gates a healthy desktop/VR transition. Start on the first measured reply, retain continuous holdover and keep shortcut/System handling independent. See [GATE_REVIEW.md](GATE_REVIEW.md).

`surfxr-wire` checks both suffix layouts and capability-revision acknowledgement, independent clock origins, +180/-220 ppm rates, first-reply startup, asymmetric queues including RTT over 100 ms, unchanged mapping/rate under queue-only variation, 60-second reply gaps, continuous recovery, honest uncertainty, replay and inconsistent-step rejection. Ground-truth clocks are independent of the fit. `surfxr-loopback` runs the actual uploader over isolated socket pairs, withholding calibration replies for more than three seconds while fresh poses continue. Rust tests accept first-reply/wide-error/old-calibration evidence while retaining identity, order, units and physical pose-validity checks. `current-tracking` and source-image pose tests retain their separate ownership coverage.

Use `scripts/Build-PC.ps1`, selected Cargo tests, and native Linux plus target-SDK ARM64 `build_viewer.py --local`. The existing `--render-pose-probe` checks both legacy matched-pose and timed submitted-pose composition against their metadata, retaining mismatched metadata as a negative control. These are build, CPU/isolated-loopback and offscreen GPU checks, not headset rendering.

`late-timewarp` injects runtime stereo queries into the production display-correction helper and independently checks world-ray invariance, both eyes, direction reversals, revised estimates for retained images, unchanged source positions/FOV/image handles and atomic failure fallback. `surfxr-foveation-gpu-test --timewarp-check` checks the production shader and gaze reconstruction, including repeated source reuse and missing-ray borders. Identify software Vulkan separately from physical GPU execution.

Acceptance requires matching PC/Frame builds with calibration capability revision 2. Record request-to-first-presented-frame time against the previous version, fallback counts, offset/rate/error/reply-age evidence, fresh/repeated poses, submission/reuse counters and deployed hashes. Do not infer physical synchronization accuracy from an estimate alone. At fixed quality/FPS/timewarp settings, inspect a world-fixed SteamVR window during lateral motion, reversals/stops and yaw/pitch, with both hands. Include repeated desktop/VR transitions, immediate System presses and reconnect. Build/CPU/offscreen results do not establish wearer acceptance.
