# Testing and acceptance

English | [简体中文](TESTING.md)

Evidence must identify the change and environment it actually validates. Historical [VALIDATION.md](VALIDATION.md) results, test counts and hardware do not automatically apply to the current commit. Commands below run from the repository root unless stated otherwise.

## Evidence levels

For intermittent-bug repairs, record normal-path latency/responsiveness before and after as well as failure reproduction. A materially slower normal interaction fails acceptance even if the rare failure disappears. Include realistic queue variation, reply loss, scheduling gaps and rapid consecutive actions when relevant; do not test only ideal timing. Gate decisions belong in [GATE_REVIEW](GATE_REVIEW.md).

| Level | Establishes | Does not establish |
|---|---|---|
| Source/document checks | Structure, links, flags, static consistency | Compilation or execution |
| Build | Outputs for a particular platform/toolchain | Correct behavior or target runtime compatibility |
| CPU/offline/isolated loopback | Models, parsing, state machines, formats and simulated lifecycle | Hardware decoding, GPU output or comfort |
| Offscreen GPU/hardware encoder | Computation, encoding or pixel properties on that GPU/driver | A live OpenXR session or headset display |
| Headset operation | Tested decoding, submission, transitions and recovery | Subjective comfort, other hardware or long-term stability |
| Wearer acceptance | A person's assessment of a version and scenario | Untested modes, durations or future commits |

Select the levels relevant to the change. Documentation edits need document checks; rendering changes cannot stop at compilation.

## Select tests by responsibility

| Change | Automated starting point | Additional evidence |
|---|---|---|
| Docs/prompts/templates | Structure, diff, relative links, commands and paths | No unrelated runtime builds |
| PC settings/Dashboard | `surfxr_session`/`surfxr_dashboard` Rust tests and full PC build | Relevant UI; native tray test needs an interactive desktop |
| Local preferences/UI | `settings-store`, `input-preferences`, `local-image-quality`, `vr-ui-model` | Status/launcher paths and device interaction |
| HUD/tracking/haptics/battery | Relevant `mini-hud`, `performance-overlay`, `tracking-model`, `motion-sync`, `haptics-queue`, `device-battery` | Runtime samples and wearer assessment |
| SurfXR/shared/extensions | `surfxr-wire`, `surfxr-loopback`, PC consumers and both builds | Negotiation compatibility and actual connection when required |
| PC desktop/daemon | Windows CTest and existing input/recovery Python fixtures | Actual capture/input/service behavior within scope |
| Decoding/color | `h264-sequence`, `hevc-sequence`, `video-color-metadata`, `video-decoder`, `async-video-decoder` | Production encoder fixtures and target decoder |
| XR/shaders/resources | Shader checks, `foveation-math`, `image-enhancement-math`, `rgba-upload`, `presentation-pacing`, `late-timewarp` | Explicit GPU, real XR and wearer checks |
| Installation/launch/releases | `launcher-native`, release Python/PowerShell fixtures, correct architecture | Package hashes and authorized install/upgrade checks |

The `launcher-native` deployment/IPC fixture checks a saved LAN profile followed by an explicit direct-link request, actual child host/interface/profile, same-profile continuation and rejection of an active cross-profile switch. Device comparisons must record actual route and fresh-picture rate, not merely requested flags or XR submission rate.

CTest registration varies by platform; inspect `surfxr/cmake/` and run `ctest -N`. A filter matching zero tests is not a passing result.

The `surfxr-loopback` startup fixture reserves its own loopback endpoint and checks that preparing SteamVR does not cancel desktop ingestion, late readiness cannot commit a cancelled request, failure preserves desktop, and a prepared launch is consumed once. `mini-hud` also covers startup-notice placement, sticky hiding after video/dismissal, and close-trigger consumption through release. These checks do not establish a gapless handoff between media generations; test both switch directions, cancellation, recovery and HUD comfort on the headset.

## Minimum checks for each commit

```powershell
python scripts/build/check_structure.py
git diff --check
git status --short
```

Structure checks enforce the 500-line rule and byte-pinned imported exceptions, not Markdown links. For documentation, also inspect relative links/anchors, paths, shell type and actual script signatures or `--help` output. Keep language editions aligned.

## Windows PC build and native tests

Prepare the environment using [GETTING_STARTED.en.md](GETTING_STARTED.en.md):

```powershell
.\scripts\Build-PC.ps1 -PrepareDependencies
```

Use `./scripts/Build-PC.ps1` once dependencies exist. The PC builder creates product targets but does not automatically build or run every test:

```powershell
cmake --build build/pc/native --config Release --parallel 2
ctest --test-dir build/pc/native -C Release -N
ctest --test-dir build/pc/native -C Release --output-on-failure
```

Reconfigure a moved build through the existing owner rather than reusing a relocated CMake cache. After the full payload is built, an optional offline ABI check is:

```powershell
.\build\pc\surfxr_streamer_windows\surfxr-daemon.exe --self-test
```

It checks matching module/driver exports, not service installation or live streaming.

For native controller input, build `cmake --build build/pc/native --config Release --target surfxr-openvr-boolean-test surfxr-openvr-frame-input-test`, then run `ctest --test-dir build/pc/native -C Release --output-on-failure -R '^openvr-(boolean|frame)-input$'`. The boolean test first reproduces a shared pose hook truncating a 64-bit input handle, then checks press/release for both hands' 40 boolean components (including D-pad and Steam system keys), proximity, time offsets and unknown-ABI fallback. The frame test covers all 48 native mappings and skeleton caching. Neither connects to SteamVR. Hardware verification separately checks the loaded fixed driver hash, verified forwarding-target messages, disappearance of boolean-update errors, and wearer testing of D-pad and the Steam system key after Take Over. The right Menu key is a different input.

## PC Rust tests

Use the pinned workspace toolchain and root `build/cargo` target directory:

```powershell
Push-Location surfxr/pc/surfxr-server
try {
    cargo test -p surfxr_session --locked
    if ($LASTEXITCODE) { throw 'surfxr_session tests failed' }
    cargo test -p surfxr_dashboard --locked
    if ($LASTEXITCODE) { throw 'surfxr_dashboard tests failed' }
} finally {
    Pop-Location
}
```

Select relevant crates instead of unconditionally testing all platform components. Record ignored/skipped tests. The native tray test is ignored by default and requires an interactive Windows desktop and native renderer. Run it separately inside the Cargo workspace:

```powershell
cargo test -p surfxr_dashboard --locked native_startup_stays_hidden_until_explicit_activation -- --ignored --nocapture --test-threads=1
```

It creates isolated UI without starting streaming owners or changing startup registrations; it is not a no-UI CPU check.

Run `python -B scripts/tests/pc/test_dashboard_startup.py` for offline singleton failure coverage. It uses the existing `source_fixture.py` to extract production startup/drop methods against a fake Win32 API, checking retained ownership when window/icon creation fails, rejected startup when mutex creation fails, duplicate activation policy and reentry after full exit. It creates no real windows, tray icons, registry or service objects. Generated Rust and results remain under root `build/reports/dashboard-startup-test/`.

## Python and PowerShell fixtures

Keep temporary output under root `build/tmp` and avoid Python bytecode in source:

```powershell
$taskTemp = Join-Path (Get-Location).Path 'build/tmp'
$null = New-Item -ItemType Directory -Path $taskTemp -Force
$taskOldTemp = $env:TEMP
$taskOldTmp = $env:TMP
try {
    $env:TEMP = $taskTemp
    $env:TMP = $taskTemp
    python -B -m unittest discover -s scripts/build -p 'test_release_*.py' -v
    if ($LASTEXITCODE) { throw 'Release fixture tests failed' }
    # Select these existing fixtures when relevant; input/recovery need Windows C++ tools.
    python -B scripts/tests/pc/test_desktop_input.py
    if ($LASTEXITCODE) { throw 'Desktop input fixture failed' }
    python -B scripts/tests/pc/test_desktop_recovery.py
    if ($LASTEXITCODE) { throw 'Desktop recovery fixture failed' }
    & ./scripts/launch/test_frame_release.ps1
} finally {
    $env:TEMP = $taskOldTemp
    $env:TMP = $taskOldTmp
}
```

Input/recovery fixtures use production code with fake OS/capture/encoder owners. Release fixtures use isolated packages and fake SSH/install commands. They do not establish real installation success and are unnecessary for unrelated documentation edits.

## Frame, Linux/WSL, ARM64 and shaders

Follow [WSL_BUILD.md](WSL_BUILD.md). `build_viewer.py --local` runs all three shader checks, gamma validation, compilation and offline CTest, then writes `validation.json`. Set temporary output in the same shell:

```bash
mkdir -p "$PWD/build/tmp"
export TMPDIR="$PWD/build/tmp"
export PYTHONDONTWRITEBYTECODE=1
```

For investigation in an existing native build:

```bash
ctest --test-dir build/frame/linux-x64 -N
ctest --test-dir build/frame/linux-x64 --output-on-failure -R '^(surfxr-wire|surfxr-loopback)$'
```

Rebuild changed source first. A filtered CTest run does not replace full release validation through the builder.

For timestamped poses, `current-tracking` injects fake OpenXR calls into the real sampler; `surfxr-wire` checks timing and bytes; `surfxr-loopback` exercises actual upload threads over isolated sockets. PC checks use `cargo test -p surfxr_server_core -p surfxr_server_openvr --lib --locked`. On Windows, include the matching `build/pc/surfxr_streamer_windows` in the test process PATH for OpenVR/libvpl DLLs.

Clock tests cover capability revision 2, first-reply startup, independent epochs, positive/negative rate error, unchanged mapping/rate under queue-only variation, wide uncertainty, 60-second reply gaps and continuous recovery. The isolated uploader keeps sending fresh poses through more than three seconds without calibration replies. PC accepts advisory quality evidence while retaining structural/identity/order/pose checks. Device `clock_*` fields are estimates, not proof of exact physical synchronization. See [FRAME_POSE_TIME.md](FRAME_POSE_TIME.md).

Standalone shader checks require the recorded glslang version:

```bash
python3 -B scripts/shaders/compile_foveation_shaders.py --check
python3 -B scripts/shaders/compile_desktop_shaders.py --check
python3 -B scripts/shaders/compile_image_enhancement_shaders.py --check
python3 -B scripts/shaders/validate_foveation_gamma2.py
```

ARM64/QEMU validates CPU behavior. Deployable packages need the target OS SDK/sysroot; Ubuntu multiarch is not automatically compatible. Installation checks dynamic dependencies. x86_64 reports cannot produce Frame releases.

## GPU and device acceptance

Use the existing color/transfer commands and targets from [TOOLS.md](TOOLS.md), including `--gpu-test`, `--software-test`, `--render-color-probe` and explicit Frame Vulkan fixtures. Build the target first and keep synthetic output under root `build/`.

`--render-pose-probe` invokes production `FrameRender`, CSOs and `PoseHistory`, comparing pixels with associated poses for both eyes, asymmetric FOVs and multi-axis direction changes, with a negative control. It does not start SteamVR or streaming; depth-dependent translation and wearer acceptance remain separate.

Record software Vulkan, physical GPU, PC encoding and target decoding separately. Do not feed fixtures to a real service or capture private desktop content for synthetic tests.

Before device checks, establish the target, matching versions, manifests, authorized operations and recovery plan. Test the relevant desktop/VR transitions, start/stop, reconnect, audio/microphone, input/haptics, accepted settings and persistence. Record only scenarios actually executed. Give wearers concrete observations, duration and versions; report stale pairs, invalid data, overflow and recovery counts honestly.

## Acceptance record

Store reports under root `build/reports/<task>/` or the selected build directory. Keep only sanitized summaries in PRs:

```text
Requirement/issue:
Source commit and any uncommitted runtime changes:
Build environment, toolchain/SDK, target architecture:
Actually deployed PC/Frame versions, or "not deployed":

Item | Evidence level | Command/action | Expected | Actual | Pass/fail/unrun/N/A
A1   | CPU            | ...            | ...      | ...    | ...

Reports, input/output hashes and logs:
Warnings, ignored/skipped tests, failures and environment limits:
Headset scenarios and observation time:
Wearer confirmation and date, or pending:
Runtime state changes and remaining acceptance steps:
```

Freeze source for final validation and commit selectively. Existing packagers verify that reports, current inputs and binaries match. Revalidate changed source or binaries; do not edit success fields, hashes or test output.

Runtime-internal adaptation follows [RUNTIME_ADAPTATION.md](RUNTIME_ADAPTATION.md). `runtime-forwarding` checks layout variations and rejection, PC `openvr-boolean-input` checks full handles/states/time offsets, and ARM64 `system-input` executes real forwarding instructions and verifies dynamic hook installation/restoration. These are offline evidence; report runtime observations and wearer acceptance separately.

Shortcut gate regression: `tracking-model` covers immediate independent doubles, third-click singles, System cancellation, stable-release re-arming, logical mode cancellation, sampling gaps and valid physical buttons without spatial poses. `vr-ui-model` covers keyboard commands without tracked rays while pointer/drag safety remains active. There is no post-switch 350 ms lockout; the 350 ms double-click recognition window and 30 ms edge debounce remain. Actual fast Menu/System sequences and occluded-controller behavior require separate wearer acceptance. See [gate decisions and evidence](GATE_REVIEW.md).

Dashboard installation/diagnostics regressions: `surfxr_dashboard` covers retained plan output, failure without trust, unique SSH identity receipts, saved-trust confirmation through the production session manager, collapsed driver lists/current-directory identity, quick log controls and quiet defaults. The isolated named-pipe test verifies authorization reuse after an operation error and revocation after transport loss, without SCM calls or UAC. `surfxr_session` verifies additive Warning-default migration with existing preferences preserved. `test_frame_release.ps1` uses fake SSH/SCP to check identity retrieval only after installation and side-effect-free Plan; public PowerShell signatures stay synchronized.

Installer-version/preview regressions: `surfxr_dashboard` exercises real command previews, pairing query, link JSON and Windows PowerShell module-path isolation, and rejects the old archive adapter as its entry. `surfxr_xtask` checks complete `frame-installer` copying without changing the ARM64 package. `test_frame_release.ps1` runs under PowerShell 5.1/7 with fake SSH/SCP to compare preview/execution commands and invoke the real public entry in a packaged layout, covering new arguments, installation/pairing receipts and startup-error reporting. `test_release_package.py` verifies the command helper hash in standalone ZIPs. Actual SSH prompts, headset installation and pairing remain separate acceptance.

`scripts/package/test_portable_windows.ps1` runs the production PC packager inside `build/tests`, checking all four adapter scripts/hashes, an unchanged older Frame archive, exclusion of user settings/unowned scripts, and rejection of missing helpers. Its fixture binaries are not executable, and the isolated ZIP never replaces or deploys a real release package.

Dashboard desktop-session regressions: `surfxr_dashboard` checks desktop rows/IP/disconnect controls without SteamVR and restoration of the original VR state after removal. Broker tests cover desktop/VR/idle projection, consumed-ticket/IP/single-watch checks, per-ticket disconnect, full-exit notifications, watch closure cleanup and reconnection. `surfxr-desktop-offline` runs `dashboard_watch_checks.cpp` against reserved ephemeral loopback endpoints, including production subscription/revocation, legacy unknown-opcode closure, EOF without explicit revocation and idle poll cost. `daemon-worker-ipc` and `desktop-video-lifecycle` cover existing retirement owners. Offline preview page 11 contains a fictional desktop connection and starts no real service.

Separate device acceptance: update Dashboard and service from the same PC package; connect desktop without SteamVR and inspect identity/IP/mode through desktop/VR/idle and headset disconnect. Disconnect session must stop media/input while retaining pairing and allow reconnection. Confirmed full exit must disconnect before SteamVR shutdown waits, while closing to tray keeps streaming. Measure request-to-disconnect and normal mode-switch latency separately. Offline notification tests do not establish capture/audio/input-release or wearer acceptance.

Interactive acceptance remains separate: readable/copyable plans, current Dashboard driver registration, sign-in/boot controls, one UAC across service operations until exit, cancellation/retry, visible SSH prompts and installation followed by confirmed pairing. Log-level changes must not restart SteamVR or stop performance updates. The offline preview includes Startup & diagnostics as capture page 14; it does not install or start streaming owners.

The release PowerShell fixture models dual-stack SSH returning a remote IPv6 interface scope unless the identity query explicitly requests `-4`. It checks matching preview argv, rejected invalid identity, and one precise partial-success error through the packaged entry. Record real read-only SSH identity checks separately from actual installation/pairing acceptance.

`surfxr-nvenc-color-test --bitrate-probe` is an explicit offscreen production HEVC/NVENC check: synthetic moving noise exercises 100→200→300→400→100→300 Mbps changes, followed by a static control showing that CBR without filler need not exhaust the budget. Encoded bytes at the configured FPS measure the frame budget, not real-time transport throughput or headset acceptance. No desktop capture, live settings changes or network traffic occur.

The explicit `surfxr-nvenc-color-test --quality-probe` compares production HEVC P1/P3/P5, single/quarter/full-resolution frame analysis and 200/300 Mbps on identical synthetic 3840x2160 grayscale translation/changing textures. Spatial AQ stays fixed. Software decoding checks frame/source association and reports luma PSNR/detail MSE alongside mean/p95 Transmit time. These grayscale metrics are not subjective quality; unpaced offscreen timing is not streaming end-to-end latency and does not justify a universal GPU preset change.

Dashboard `typed_bitrate_commits_on_enter_and_focus_loss` drives real egui text entry and Enter/click-away, applies requests through the production SessionManager, checks exactly two edits and recreates the settings page. Native acceptance must check both displayed and persisted values, not just the UI editing cache.
