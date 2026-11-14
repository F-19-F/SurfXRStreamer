# Development and feature extensions

English | [简体中文](DEVELOPMENT.md)

Follow the [runtime adaptation policy and audit](RUNTIME_ADAPTATION.md): prefer public interfaces; never patch compatibility with product versions, private offsets or captured instruction patterns. Necessary dynamic parsing must be explicitly disclosed in maintenance reports, with evidence and rejection/compatibility limits.

Read [architecture](ARCHITECTURE.md), [tool owners](TOOLS.md) and [SurfXR contracts](PROTOCOL.md) before editing. Unless stated otherwise, source paths below are relative to `surfxr/`. Commands and evidence requirements are in [TESTING.en.md](TESTING.en.md).

## Owners and interfaces

| Module | Responsibility | Adjacent interfaces |
|---|---|---|
| `pc/surfxr-server/surfxr/dashboard` | PC UI, trust, accepted settings, tray | Session, service/device orchestration |
| `pc/surfxr-server/surfxr/session` | PC schema, defaults, migration | Dashboard, server_core, Frame settings |
| `pc/surfxr-server/surfxr/server_core` | SurfXR sessions, networking, stream orchestration | Packets, driver, Dashboard |
| `pc/surfxr-server/surfxr/server_openvr` | SteamVR driver, textures, encoding, worker | OpenVR, shared textures, GPU IPC |
| `pc/daemon` | SYSTEM service, admission, backend switching, worker leases | Dashboard, desktop module, VR worker |
| `pc/desktop` | Capture, input arbitration, audio, encoding, authenticated media | Windows, daemon, desktop protocol |
| `shared/surfxr` | Wire codecs, authentication, framing, FEC, extensions | Both endpoints |
| `shared/protocol` | Internal geometry, semantic input and client commands | Native modules; no second network backend |
| `shared/platform` | Genuinely shared platform helpers | Status files, interface binding |
| `frame/surfxr` | Negotiation, sessions, control and media reception | Callbacks, wire, tracking |
| `frame/session` | Public callback/options interface | Network and application assembly |
| `frame/app` | Viewer assembly, receiver worker, status/commands | Launcher, session, media, XR |
| `frame/tracking` | Stage, pose identity/prediction, metadata, batteries, haptics | Runtime, SurfXR, renderer |
| `frame/media` | Bounded asynchronous decoders, audio/microphone, frame resources | Reception, leases, XR sampling |
| `frame/xr` | OpenXR/Vulkan, presentation, timewarp, reconstruction, fences | Runtime, media, tracking, UI |
| `frame/ui` | Local UI, pointer/keyboard, HUD | XR, preferences, input |
| `frame/preferences` | Local formats, migration, remote-setting cache | Dashboard, launcher, viewer |
| `frame/dashboard` | Headset settings/status UI | Launcher and shared preference interfaces |
| `frame/input` | System input bridge and runtime driver | Runtime and explicit input capture |
| `frame/launcher/native` | Installation, services, connection and lifecycle | Embedded assets, viewer IPC, configuration |
| `scripts/launch` | Windows orchestration and remote launcher RPC | Root entries, reports, installers |
| `scripts/build`, `cmake` | Manifests, build, structure and packaging | Inputs and evidence |

Keep private headers with their subsystem. [public_includes.cmake](../cmake/public_includes.cmake) declares stable `surfxr/*.hpp` roots; do not bypass owners with a flat public include directory.

## Follow the data

VR flows from SteamVR through PC capture/encoding and the SurfXR session to Frame reception, decoding, metadata/eye association and Vulkan/OpenXR presentation. Tracking flows in reverse. Timestamp identities are not automatically measurements on a shared clock.

Desktop flows from Windows capture/input/audio through the daemon-managed desktop module and authenticated stream to the Frame session and UI. Persistent connection and media-generation owners coordinate desktop/VR switching.

PC-authoritative settings use requests, receipts and acceptance serials. Local Frame preferences use local persistence and IPC. Locate the authoritative state before adding a UI cache or default.

## Frame-local settings and UI

For HUD, layout, image processing or input controls that do not change the wire contract:

1. Specify defaults, bounds, valid modes, invalid-state behavior and persistence.
2. Use the appropriate `frame/preferences` owner; preserve old field order, strict validation and reserved slots, with explicit migration for new fields.
3. Follow commands/status through `frame/app/include/surfxr/viewer_commands.hpp`, `viewer_status.hpp` and the native launcher. Queued is not accepted.
4. Keep `frame/dashboard` and `frame/ui` entry points aligned on IDs, bounds and acceptance serials.
5. Put behavior in its owner: UI decides interaction, XR renders it, HUD reuses existing telemetry.
6. Extend existing preference, status, launcher and UI tests for migration, bounds, invalid inputs, restart persistence and rejected/stale responses.
7. Update architecture and user instructions; reserve visual comfort for device/wearer acceptance.

Examples include `frame/preferences/input_preferences.cpp`, `tests/frame/preferences/input_preferences_test.cpp`, `frame/ui/mini_hud.cpp` and `tests/frame/ui/mini_hud_test.cpp`.

## PC-authoritative settings

Define schema, defaults and migration in `pc/surfxr-server/surfxr/session`, then follow Dashboard, actual PC consumers, Frame requests/snapshots and local UI. State whether a value applies immediately or needs a restart. A cached value cannot invent acknowledgement or override a newer accepted serial.

If the existing message cannot carry a new value, design explicit negotiation at both ends. Keep live input/encoding state off settings network calls, using the existing worker/snapshot owners. Test old/missing fields, conflicts, reordered receipts, rejection, reconnect and restart. Update private module ABI when its interface changes.

References: `frame/preferences/include/surfxr/remote_settings.hpp`, `tests/frame/surfxr/settings_sync_checks.cpp` and `pc/surfxr-server/surfxr/session/src/tests.rs`. Exact pointer-priority bytes are in [PROTOCOL.md](PROTOCOL.md), not inferred from UI types.

## SurfXR protocol extensions

SurfXR is the only backend, defaulting to `21.0.0-dev14`, with the existing explicit v20 path preserved. [FRAME_POSE_TIME.md](FRAME_POSE_TIME.md) illustrates coordinated sampling, clock mapping, prediction ownership and source-image poses.

Specify negotiation and missing-capability behavior; exact widths, order, endianness, units, ranges and size limits; requests/receipts, generations, timeouts, replay and ordering; authentication and directional keys; actual endpoint consumers; and disconnect/cancel/switch/exit cleanup.

Preserve original byte fixtures, then extend wire and isolated loopback tests under `tests/frame/surfxr`. Reserve test endpoints; do not use a live streaming service. Exercise actual Rust/native relays where relevant rather than relying only on matching newly written parsers. Commit endpoint code, tests and documentation together and document the compatible upgrade path.

## Decoding, rendering, color and prediction

Define whether the change concerns pixel meaning, matrix/range, transfer, spatial sampling, prediction or presentation cadence.

- Keep stereo, remote timestamp, tracking-ledger entry and Stage epoch associated; reset stale associations.
- Retain immutable decoder allocations until all GPU sampling completes. Upload completion alone does not end the lease.
- Preserve bounded queues and recovery behavior, and retire rendering before decoder/device teardown.
- Keep current 8-bit SDR interpretation, SPS/decoder fallback and CPU/GPU color paths consistent with architecture.
- Regenerate shaders through existing `scripts/shaders/compile_*_shaders.py` tools and check them. The PC CSO owner is documented in [TOOLS.md](TOOLS.md).
- Preserve timing definitions. Inferred timing and XR submission are not photon-latency measurements.

Choose mathematical/metadata/lifecycle tests, affected native builds, explicit offscreen GPU fixtures and device/wearer checks as appropriate. Identify GPU, driver, software versus hardware Vulkan and fixture inputs. Never inject fixtures into a live stream.

## Desktop interaction

`frame/ui` owns pointer stabilization and click/drag recognition, `frame/xr` draws it, and `pc/desktop` owns PC/VR arbitration and input. Check press/release/all-up, focus loss, the other hand, source/display changes, stale tracking, disconnect and exit. One-frame motion must not accidentally become a drag; rejected input must not replay later. Release previously pressed buttons after ownership changes.

Use existing `tests/pc/test_desktop_input.py`, `test_desktop_recovery.py` and `tests/frame/ui` fixtures. Distinguish model compliance from actual desktop behavior and comfort.

## Launch, installation and packaging

Use `scripts/` entries, `scripts/launch`, `frame_remote.py` and native launcher owners. Public wrappers forward explicitly bound arguments with synchronized signatures; PC builds remain owned by `scripts/Build-PC.ps1` and `cargo xtask`.

Verify build-only side effects; `%h/.local/share/surfxr-client/launcher/surfxr-client` as installed ExecStart; separate source/staging/release/installed paths; embedded assets, architecture, dependencies and hashes; spaces and escaping; missing/corrupt files, rollback and data preservation; and complete matching PC binaries. Startup registration, services, firewall and drivers retain their owners.

Prefer `launcher-native`, existing release Python tests and `scripts/launch/test_frame_release.ps1`. Real Setup fixtures change registration/shortcuts and are separate installation checks. Actual deployment must be within scope and verified against [RELEASE.md](RELEASE.md).

## Dependencies and upstream changes

Explain why existing code is insufficient, platforms, source/version, download/cache owner and license location before adding dependencies. Preserve imported notices with the owning tree. See [public preparation](OPEN_SOURCE.en.md).

For SurfXR/OpenVR upgrades, record versions, preserved local extensions, generated resources and endpoint compatibility evidence. `UPSTREAM_LINE_BASELINE.json` is an exact reviewed exception list, not permission for arbitrary large files. Inspect baseline changes and run structure checks before committing.
