# SurfXR Dashboard

The Steam Frame fork uses the **SurfXR** product name. The executable remains
`SurfXR Dashboard.exe`; driver names, app ID, session keys and negotiated protocol
identifiers retain their existing values. Upstream SURFXR links and update messages
are explicitly identified as upstream resources.

## Settings

| Page | Contents |
| --- | --- |
| Settings > Quick setup | Picture/performance, sound and tracking presets |
| Settings > SurfXR features > Devices & input | Headset model, controller model, Frame gaze/capture guidance |
| Settings > SurfXR features > Desktop streaming | Access, keyboard/mouse, audio, service requirement, FPS, per-monitor bitrate, monitor count, desktop tracking and unified media encryption |
| Settings > SurfXR features > Startup & connection | Manual SteamVR driver activation, automatic Dashboard/runtime lifecycle and direct launch |
| Settings > Advanced | Existing Video, Audio, Headset, Connection and Extra controls, with search within the selected category |
| Desktop | Host/service status and explicit service actions, plus a link to desktop settings |

Quick setup resolution now uses only the nine SurfXR per-eye tiers from Frame's
`preferences/remote_settings.cpp`: Potato 1344x1440, Low 1728x1824, Medium 2112x2304,
High 2496x2688, Ultra 2688x2880, Godlike 3072x3264, Monster 3648x4032,
Rampage 4224x4608 and Unstoppable 4992x5376. Godlike is the factory/reset default.
Each choice sets explicit width and height for both the SteamVR target and stream;
gaze atlas dimensions remain negotiated separately. Raw controls stay in Advanced.

Windows microphone defaults to enabled with the named `SteamStreaming` device
route. Both playback and recording endpoints match Steam Streaming Microphone,
as with the previously deployed Custom configuration. The microphone preset is
only in Advanced > Audio, with Disabled, Steam Streaming Microphone, the existing
third-party routes and raw Custom endpoint settings. Reset returns to Steam.
Linux retains its existing PipeWire defaults. Opening settings never overrides a
saved route. First-run guidance no longer requires Virtual Audio Cable for Steam.

## Windows portable packaging

Use the existing xtask after building the current release. `--no-rebuild` reuses
that validated output; omit it for the normal distribution build. The private
packaging step allowlists binaries/licenses, adds app-local Microsoft x64 CRT to
the Dashboard and worker directories, validates the Frame package hashes, and
includes its existing installer/tools. It neither installs nor starts services.

```powershell
$env:SURFXR_MSVC_REDIST_DIR = '<Visual Studio>/VC/Redist/MSVC/<version>/x64/Microsoft.VC143.CRT'
$env:SURFXR_FRAME_CLIENT_ZIP = '<validated SteamFrameClient release.zip>'
cargo xtask package-streamer --portable --no-rebuild
```

Output: `build/SurfXR-Windows-portable.zip`, with SHA-256 sidecar and per-file
manifest. The package excludes session/trust files, logs and debug symbols. Steam,
SteamVR and GPU drivers remain installed prerequisites. `Install-SteamFrame.cmd`
forwards to the included Frame release installer; it is not another installer.
Packaging generates the existing Rust dependency license report with cargo-about.

Validation for the microphone/resolution follow-up: 21 Dashboard, six session,
four audio, 27 sockets and 19 server_core tests passed (77 total). The real egui
test verifies default selection, moving the microphone out of Quick setup,
Disable/Reset, and selecting the last SurfXR resolution from a custom configuration,
including explicit height on both FrameSize fields. Release streamer built and
ten native preview pages rendered; Quick setup and Advanced Audio were inspected.
The portable ZIP passed CRC and all 117 file hashes, includes CRT for both executable
directories, and its daemon offline self-test passed. Microsoft CRT signature was
verified. This is not a clean-machine install test or live microphone/headset test.

The two emulation controls, desktop settings and SteamVR launcher controls are
extracted from the original schema and bind to their **original JSON paths**.
The regrouping preserves existing defaults and uses the original setting writer.
The additional gaze preferences and their effect on negotiation are described below.
Other controller and eye-tracking controls remain under Advanced > Headset.
New sessions and the identity Reset buttons now use Steam Frame and Steam Frame
Controllers. Existing user-selected identities are not overwritten on opening.
The Dashboard schema projection removes upstream-client-only controls: passthrough,
client buffering/history, Android decoder/MediaCodec settings, client foveation,
post-processing/upscaling, headset performance level, wired client selection/start,
client socket buffers and client log reporting. TCP/UDP selection is also removed;
the existing session default remains UDP. Full persisted settings and experimental
transport implementations remain intact. Frame display/decoder settings belong on
the headset. Search cannot expose the removed controls.
The follow-up passed 22 Dashboard and six session tests, including real identity
dropdown/Reset input and search absence without session writes. Release streamer,
ten native preview pages and daemon offline self-test passed; identity and
Connection previews were inspected. No service update or live headset test was run.
Controller emulation remains editable while forwarding is off; the page then
explains where to enable forwarding. Model changes retain the existing SteamVR
restart behavior. Merely opening a page does not save or reset anything.

Advanced search includes labels, help and nested schema fields. Search exposes
matching collapsible sections temporarily without writing their collapsed state.
Disabled switches retain their normal enabled/disabled behavior. Clearing search
restores the saved view. Shared presets keep their original modifiers; selecting
a preset after custom values correctly accounts for the extra custom-value row.

## SurfXR video presets

Quick setup offers **SurfXR default** for Encoder preset and Foveation preset.
These are explicit actions, also used by each row's Reset button. New sessions
now initialize their encoder settings to SurfXR default (including single-pass
encoding and ForceThree). Existing saved settings remain unchanged until a
preset is applied; opening the Dashboard does not overwrite them.

- **Encoder:** CBR, generic Speed, NVENC P1 / LowLatency / Disabled multipass /
  ForceThree / Spatial AQ. It clears the NVENC rate-control override and disables
  weighted prediction so those overrides cannot defeat CBR or split encoding.
  This follows the Frame latency tuning documented in the paired client repo.
  Splitting depends on GPU and codec support; H.264 remains unsplit. Codec,
  bitrate, frame rate, resolution and other advanced tuning remain independent.
- **Foveation:** disables upstream `video.foveated_encoding`, retaining its
  saved center/edge parameters. This row is editable **only when SurfXR gaze
  streaming is off**. To use the atlas, enable it in SurfXR features > Gaze streaming
  and on Frame. Selecting this preset alone does not enable gaze streaming.

Upstream Light / Medium / High adjust the center size and edge compression of
the upstream encoded image. SurfXR instead packs nonlinear full-view bases and
moving, full-resolution gaze crops into one atlas. Active upstream foveation
and the SurfXR atlas are mutually exclusive. These are streaming optimizations,
not a request for the game to shade fewer pixels. The existing upstream
eye-tracked foveation option is separate from SurfXR atlas negotiation.

Original Speed / Balanced / Quality still change only their two quality fields;
original Light / Medium / High retain their five assignments. Preset detection
chooses the match with the most fields, using schema order for ties, so the
complete SurfXR configuration is not randomly displayed as Speed. Custom values
remain custom when no preset matches.

## Gaze streaming and focus size

**SurfXR features > Gaze streaming** owns `video.frame_gaze`: an enabled switch and
independent focus width/height percentages. Quick setup shows the switch state
and a direct configuration link. The region controls disable when gaze is off;
the original Foveation preset disables while it is on, retaining saved values.

Defaults preserve the existing atlas: enabled, 25% width and 25% height per eye.
Both dimensions support 10–33%, rounded up to 32 pixels. Percentages describe
linear dimensions, not area: 25% × 25% is approximately 6.25% of each eye's area
before alignment. The nonlinear base stays at one third per axis. The current
atlas reserves a base-width slot for each focus crop, which bounds focus width;
the controls use the same range on both axes. For 3072×3264 eyes, 20% × 30%
negotiates 640×992 focus regions, while 25% × 25% remains 768×832.

The server uses the enabled flag and focus dimensions during negotiation and
includes all three values in its stream restart hash. Changes rebuild geometry
through the existing reconnect/reinitialization path, not inside an active frame.
The renderer, encoder and Frame decoder/reconstructor consume the negotiated
dimensions. For a Frame gaze-capable client, enabled SurfXR gaze takes priority over
saved upstream foveated encoding. The headset must also enable foveated streaming;
Windows, UDP, H.264/HEVC, SDR 8-bit and gamma 1 remain required. If those conditions
are not met, the Frame session uses full-frame video. There is no new legacy-client
compatibility path. No running session is restarted by the development preview.

## Appearance and messages

The Dashboard uses a Windows-style dark palette, blue selection, compact controls,
text navigation and restrained cards. Its styling is local to the Dashboard,
without changing the separate upstream launcher. Windows loads installed Segoe UI,
Microsoft YaHei and Segoe UI Symbol fonts, with default egui fallbacks retained.
CJK fallbacks apply to proportional text and monospace logs; no Windows font is
redistributed. Native UI scaling remains enabled.

The obsolete SteamVR-required device warning is replaced with a dismissible
status hint that distinguishes desktop service status from VR. Compatibility
guidance opens in a closable Notes window. Help and restart/live badges retain
the original schema descriptions and flags.

Notifications use a neutral surface and colored severity text. They expire after
eight seconds and can be dismissed immediately. Up to 64 exact message/severity
pairs are suppressed for the current Dashboard session; new messages still appear
and the original Logs event flow remains intact. Tips can be dismissed independently
and keep the existing logging preference. Expanded details have a bounded height.

## Build and offline validation

```powershell
cargo build -p surfxr_dashboard --release
cargo test -p surfxr_dashboard
cargo test -p surfxr_session
target/release/surfxr_dashboard.exe --ui-preview
target/release/surfxr_dashboard.exe --ui-preview --capture-dir build/surfxr-ui-preview
target/release/surfxr_dashboard.exe --ui-preview --compact --capture-dir build/surfxr-ui-compact
```

`--ui-preview` is an early branch before process cleanup, disk session loading,
network listeners, runtime or service ownership. It displays production settings
controls and applies their requests through an in-memory ServerSessionManager.
It cannot install/start services or change the user's saved session. Optional
capture uses eframe's renderer to write eleven synthetic PPM views (ten settings pages and Devices) and exit, with
a 45-second limit. These generated images stay in ignored build directories.

The egui input tests exercise the actual emulation dropdowns, desktop toggle and
reset, custom preset dropdown, nested search, both font families and browsing all
ten pages at compact and standard widths without writes. This is build/offline UI
evidence, not streaming, headset rendering or wearer acceptance. The developer
preview navigation is intentionally labeled and is not the service control UI.

2026-10-14 validation: Windows Release build, 16 Dashboard tests and five session
tests passed. Both 1120×760 and 820×560 previews produced nine views through the
native renderer. CJK glyph coverage was checked in both font families. Modified
Rust files remain below 500 lines (maximum 387). Desktop automation could not
activate the window, so OS-level interactive acceptance remains unverified.

Gaze-controls follow-up validation (2026-10-14): 20 Dashboard, six session,
27 sockets and 19 server_core tests passed. This includes actual UI gating/reset,
aligned focus sizes, invalid bounds, crop association and the restart hash.
The paired Frame `surfxr-wire` CPU test accepts a 640×992 negotiated focus region.
Release Dashboard, OpenVR driver and encoding worker built successfully using
the existing Cargo packages; `LIBCLANG_PATH=deps/libclang/clang/native` supplies
the workspace's bindgen dependency. MSVC emitted its library-creation messages.
Native previews rendered ten views each at 1120×760 and 820×560; the gaze and
quick-setup pages were visually checked. No live headset rendering or wearer
acceptance was performed.

The current Dashboard was running, so the complete review package is staged in
the historical gaze-settings build, based on the existing streamer package
with the new Dashboard, both driver DLL copies, worker and PDB replaced and
hash-checked. Its configuration remains unchanged. The running package and
services were not replaced or restarted. New/split modules stay below 500 lines;
the paired architecture document records the two pre-existing oversized upstream
files whose schema/connection interfaces required small wiring changes.

The default-selection follow-up aligns factory encoder values with the actual
SurfXR preset, instead of just changing the preset row's Reset target. The existing
preset test verifies that a fresh production session immediately selects SurfXR
without applying any modifiers. The deployed user's saved session is explicitly
set to this preset as part of the requested deployment. Session JSON conversion
and its existing tests were moved into separate modules without changing their
cases or serialization behavior, keeping those touched files below 500 lines.


## One-time pairing and language selection (2026-10-14)

Devices > New Wireless Devices > One-time pairing opens a process-local admission
window, initially off on each Dashboard launch. The first untrusted candidate is
reserved under one mutex; another candidate cannot use the same window. Existing
trusted devices do not consume it. The candidate is trusted through the existing
session owner, and the window closes after VR Connected/Streaming or authenticated
desktop ticket consumption. Turning it off cancels further automatic admission;
trust already granted remains in the normal device list and can be removed there.
The independent permanent Auto trust clients option must be off to use this switch.

`pairing.rs` owns only this transient state. Local discovery/start requests and the
desktop broker use the existing SessionManager; while the driver owns the session,
Dashboard session events and desktop registration use its existing request API.
No new protocol fields, second session writer, or persistent auto-trust setting
are introduced. Desktop ticket admission does not establish that video rendered.

The sidebar and first-run wizard offer English / 简体中文. The selection saves in
`dashboard-ui.json` beside session.json, independently of streaming settings.
`i18n.rs` and compiled JSON catalogs translate UI labels, common help and choices
at display time; schema paths, preset IDs, user device names, technical identifiers,
raw diagnostic logs and upstream release notes keep their original values.
Untranslated upstream help falls back to English. Chinese settings search also
accepts English terms. No network translation or client compatibility changes.

Offline preview accepts `--language=zh-CN`; its selector changes memory only.
30 Dashboard tests cover one-candidate pairing, automatic closure, unchanged saved
settings, translated controls with English serialized keys, bilingual search and
language preference round trips. A reserved loopback HTTP fixture exercises the
production driver-owner registration path, including the second-device rejection
and unchanged permanent trust setting. Previously observed offline untrusted
entries do not reserve a new window; explicit connection retries remain eligible.
Release and native previews are build/offline
checks; live pairing, headset rendering and wearer acceptance remain unverified.


Pairing placement / localization follow-up: the one-time switch now belongs to the
Devices page beside pending devices and is absent from Quick setup. Its lifetime
and trust policy are unchanged. All current schema help/notice text and built-in
preset guidance have Chinese catalog entries, including encoder, gaze, desktop,
tracking and startup tooltips. Notification tips and known service/firewall
summaries translate at display time, preserving raw diagnostic details and logs.
The existing preview now includes Devices as page 10 (eleven captures total).
31 Dashboard tests include the actual moved control and a production-schema
help/notice coverage check to catch missing translations in future changes.

### First-connect restart delivery

Fresh portable settings can require a SteamVR restart after Frame negotiates
its resolution and refresh rate. The driver retains this pending request for its
process lifetime and replays it when Dashboard subscribes or reconnects to the
existing event WebSocket. A subscriber receives the request once, including after
broadcast lag; ordinary events retain their existing behavior. Dashboard ignores
duplicate requests while its existing restart worker is active instead of blocking
the UI and launching a second restart. No streaming protocol or saved settings
format changes. Four server tests cover early/late subscribers, registration races,
reconnect and broadcast overflow; the 31 Dashboard tests also pass.

### Matched portable and Setup releases

`cargo xtask package-streamer --portable --setup --no-rebuild` creates both
`build/SurfXR-Windows-portable.zip` and `build/SurfXR-Windows-setup.exe` from
one allowlisted staging directory. Set `SURFXR_FRAME_CLIENT_ZIP` to a freshly
validated native Frame release, `SURFXR_MSVC_REDIST_DIR` to the x64 CRT directory,
`SURFXR_RELEASE_ID` to the shared release label, and `SURFXR_MAKENSIS` to NSIS 3's
Unicode `makensis.exe`. The compiler is a build dependency, not an installed app
dependency. The PC manifest records the release label, both source commits,
the Frame archive hash and every payload hash. `--setup` also emits the portable
archive; it does not build a second payload list.

Setup supports English/Simplified Chinese, installs for the current user under
`%LOCALAPPDATA%\Programs\SurfXR`, and adds Start-menu Dashboard, Frame installer
and uninstall shortcuts. Close the target Dashboard/SteamVR before upgrading.
Update the headset using **Install Steam Frame** (or `Install-SteamFrame.cmd` in
the portable folder). Desktop service installation/update/removal remains in the
existing Dashboard Desktop page with its administrator flow; Setup and uninstall
do not manage the shared service or deploy the headset. Uninstall removes only
manifest-listed payloads and its own registration/shortcuts, retaining settings,
logs and user files. The Setup build is unsigned.

`powershell -File surfxr/xtask/src/test_setup_windows.ps1 -Installer PATH` exercises
the real installer in a fresh directory containing spaces, checks every manifest
hash after install and upgrade, and verifies uninstall preserves configuration.
It requires no existing Setup registration or Start-menu folder, and never opens
Dashboard or streaming services.

Release `2026.10.14-r2` uses Frame source `666e026` (including the `bf0ad40`
desktop input changes) and the PC startup fix `a1f9e6c5`. Frame packaging verified
all 623 build inputs against the zero-warning ARM64 build with 33/33 offline tests.
The real Setup install/upgrade/uninstall check passed for all 117 payload hashes,
including paths with spaces and configuration preservation. NSIS 3.11 compiled
without warnings; its portable compiler archive SHA-256 is
`c7d27f780ddb6cffb4730138cd1591e841f4b7edb155856901cdf5f214394fa1`.
