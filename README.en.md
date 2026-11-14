# SurfXR

SurfXR starts its independent product version at **1.0.0** and is based on **ALVR**, with original attribution and licenses retained. Product and wire revisions are separate; SurfXR never queries or installs ALVR updates.

English | [简体中文](README.md)

**SurfXR** streams a Windows PC desktop and SteamVR to **Steam Frame**. Run the SurfXR Dashboard on your PC and the companion **SurfXR** client on your headset to use your desktop or switch to PC VR games. A complete release package lets you install without setting up a compiler.

Both endpoints, their build tools and SurfXR protocol extensions are maintained together. You can contribute directly or work with your own coding agent; development does not depend on a particular agent product or the maintainer's chat history.

<a id="user-features"></a>

## Useful features

| Feature | What it does and where to find it |
|---|---|
| Safety boundary synchronization | Synchronizes Steam Frame's valid safety boundary with PC SteamVR and aligns the standing space, reducing duplicate room setup. Headset `Devices → SAFETY BOUNDARY` is on by default. |
| Controllers disconnect and disappear when disabled | Turning off headset `Devices → VR CONTROLLERS` reports both controllers as disconnected to PC SteamVR and releases held buttons, avoiding controllers frozen at their last position. Local menus and desktop controls remain usable. Physical power-off or disconnection also follows the connection state reported by Frame. |
| Switch between desktop and PC VR | Use Windows and SteamVR from the same client. During streaming, quickly double-click the right `Menu` button to switch modes, handle a desktop task and return to VR. |
| Multiple desktop panels and a virtual keyboard | Arrange PC monitors as floating panels with adjustable position, size and curvature. Use controller clicks, right-clicks, scrolling and typing; keep the keyboard in space or attach it to a hand. See [controllers and built-in menus](#controller-guide). |
| Gaze streaming | A high-resolution region follows your gaze, with a lower-resolution image covering the periphery. Enable both PC **Quick setup → Picture & performance → SurfXR gaze streaming** and `Stream → Gaze streaming` on Frame's connection screen. Disconnect before changing the Frame setting; it applies on the next connection. For PC changes, restart SteamVR as prompted. Unsupported configurations use full-frame video. |
| PC mouse / VR pointer priority | Headset `Devices → CURSOR PRIORITY` offers `VR HAND` or `PC MOUSE` priority when both move at once, reducing cursor contention on the same desktop. |
| Customizable Mini HUD | Enable `Diagnostics → OVERLAYS → MINI HUD` for FPS, estimated latency and other statistics. Choose its content, position and size, or reveal it only while looking at its area. See [Mini HUD](#mini-hud). |

Boundary synchronization updates **the PC SteamVR live room boundary and standing origin**, preserving seated calibration and boundary visibility preferences. Turning it off stops further updates; it **does not restore the PC's previous boundary**. To restore that boundary, configure it again in PC SteamVR. Synchronization copies an existing boundary and does not replace headset safety setup.

`VR CONTROLLERS` controls forwarding for both hands together. Connected physical controllers still operate the local SurfXR interface while forwarding is off; turn it back on to resume PC input. Physical power-off detection depends on Frame's reported state. Games that draw their own controller models control whether those models are hidden.

<a id="user-guide"></a>

## User guide

### Prepare your devices and packages

- **PC:** Windows 10/11 x64, graphics drivers and a GPU capable of running the selected encoder. PC VR also requires Steam and SteamVR. These drivers and applications are not included with SurfXR.
- **Headset:** Steam Frame and a matching SurfXR release built for its operating system. The PC must be able to log into the headset over SSH for installation; have its actual address, login name and password or key ready.
- **Network:** a working LAN or 6 GHz direct link between PC and headset. The selected headset network mode and PC address must refer to the same link.
- **Files:** choose the PC Setup installer or portable ZIP for the same release. The complete PC package includes a Frame installation entry; a matching standalone `SURFXR-EXO-Frame` ZIP is another option. Keep the entire folder and accompanying DLLs together. GitHub source archives require compilation and are not application release packages.

### Install and connect for the first time

Installation preview shows the actual SSH/SCP arguments, remote shell commands and native installer JSON input without contacting Frame. The remote staging path is explicitly unresolved until preparation returns it. PC updates must include Dashboard and its `frame-installer` directory; the ARM64 package remains independently selected under `frame-client` or another chosen folder.

1. **Open the PC Dashboard.** After running Setup, launch **SurfXR** from the Start menu. For the portable package, extract the entire ZIP and run `SurfXR Dashboard.exe`. The application may stay in the Windows notification area; click its tray icon to open settings. Select English or Chinese in the sidebar.
2. **Set up the PC.** In **Installation**, select **Add firewall rules** and approve the Windows administrator prompt. For desktop streaming, use **Install / update service**, then **Start service** in the same page's **Desktop streaming service** section. Before using PC VR, check that the SurfXR driver is registered; use **Register SurfXR driver** if needed. Completing Setup does not start the service or pair the headset automatically.
3. **Install the headset client.** Use PC Dashboard **Installation → Steam Frame client** to preview and install. The plan stays visible, and successful installation pairs the SSH target with this Dashboard. Alternatively, run `Install-SteamFrame.cmd` from the PC package, or extract the standalone Frame package and run `scripts/Install-Frame.cmd`. Choose install/update. The installer connects over SSH; confirm the device and log in when prompted. The default `steamos@frame` must identify your device; if the hostname cannot be resolved, the installer asks for the headset's IPv4 address. Open **SurfXR** on the headset after installation; streaming remains idle. See the [installation entry guide](surfxr/docs/ONE_CLICK.md) for a custom SSH login or link.
4. **Select the PC and trust the headset.** Keep the PC Dashboard running. In SurfXR, choose `WiFi` for LAN or `6 GHz link`, select `Find PCs`, and choose your PC/link. You can also enter that link's PC address using `PC address`. After Dashboard installation confirms pairing, no additional Trust action is needed. For manual script installation, initiate a connection, then identify your Frame in the PC **Devices** page and select **Trust**. Alternatively, enable **New Wireless Devices → One-time pairing** when you are ready to connect: it trusts the next new device and turns off once it connects. Paired devices remain trusted.
5. **Start streaming.** Select `Desktop` on the headset to view and control your PC desktop, or `SteamVR` for PC VR. SteamVR must be installed on the PC with a usable SurfXR driver. Select `Disconnect` to stop streaming. To change network links, disconnect first, then select the new link/address and reconnect.

### Everyday controls

| What to change | Where to find it |
|---|---|
| Resolution, frame rate, bitrate and desktop monitor count | PC **Quick setup → Picture & performance**. The headset's `Stream` page also edits the corresponding stream settings. VR resolution/frame-rate changes require a manual SteamVR restart; bitrate can change during streaming. |
| Desktop access and audio | PC **SurfXR features → Desktop streaming**. Check desktop service status under **Installation**. |
| Headset image processing, devices and diagnostics | SurfXR `Image`, `Devices` and `Diagnostics`. See [Mini HUD](#mini-hud) below for the small statistics overlay. |
| Prepare desktop streaming at boot | PC **Quick setup → Startup & diagnostics → Start desktop streaming automatically at boot**. This configures service startup and the tray Dashboard at sign-in, with administrator approval. Use **Start service** to start it immediately. |
| Dashboard sign-in startup and log level | PC **Quick setup → Startup & diagnostics**. Dashboard can start independently in the tray. Logs default to Warning; Debug enables detail and severity changes apply immediately. |
| View and disconnect desktop devices | PC **Devices → Desktop / unified connections** shows devices, addresses and desktop/VR/idle state without requiring SteamVR. **Disconnect session** keeps pairing; reconnect from the headset when needed. |
| Hide or exit Dashboard | With a tray icon, closing the window hides it and keeps streaming. Confirming **Exit completely** first notifies the service to disconnect all sessions. Update both Dashboard and the desktop service for this behavior; the service remains installed/running. |

<a id="controller-guide"></a>

### Steam Frame controllers and built-in menus

These controls apply to **Steam Frame controllers** in an active SurfXR streaming session. Other devices may have different layouts. Ordinary PC VR game controls follow SteamVR/game bindings.

**Open the SurfXR settings menu:**

1. Click and release the **right `Menu`** button, then wait about 0.35 seconds for the virtual keyboard.
2. Aim a controller ray at `SETTINGS` at the top of the keyboard and pull the trigger. Select `STREAM`, `IMAGE`, `DEVICES` or `DIAGNOSTICS`.
3. Select `CLOSE` at the top right to hide settings, or `KEYBOARD` to return to the keyboard. If the keyboard remains visible, click right `Menu` once more to hide it and restore game controls. In desktop mode, you can also aim at the empty area immediately below a screen to reveal its control strip and select the rightmost `SETTINGS` circle directly.

Entering SteamVR prepares the PC runtime in the background while the current desktop keeps playing. Switching no longer deliberately displays a frozen desktop; the media-session handoff can still have a brief gap before the next source's first frame. A small notice near the center on the lower right appears during initial waiting, desktop/VR transitions and recovery, over the live source when available. It hides when the transition completes. Its `X` or one right `Menu` click dismisses only that transition; a later transition can show it again. **Diagnostics → Overlays → Switching HUD** in Dashboard or in-stream settings persistently disables or enables it (default on), independently of the performance Mini HUD.

**Shortcuts:**

| Button | Action |
|---|---|
| Right `Menu`, single click | Toggle the virtual keyboard; when settings are open, return to the keyboard. A single click waits about 350 ms after release to distinguish it from a double-click. |
| Right `Menu`, quick double-click | Switch between desktop and SteamVR modes. The double-click window is 350 ms. |
| Left `View` | Forwarded to PC VR; its action follows SteamVR controller bindings. In the Touch-compatible mapping it acts as the remote SteamVR menu button. Use `SETTINGS` as described above to open SurfXR settings. |

**Desktop, keyboard and panel controls:** aim the controller ray at the target first. These mouse aliases apply in desktop mode or while the local keyboard/settings are visible; they do not define PC VR game bindings.

| Action | Controller input |
|---|---|
| Select a control or left-click the desktop | Either trigger or bumper; right `A` and left D-pad `↓` also select. Hold and move over desktop content to drag it. |
| Right-click the desktop | Right `B`, left D-pad `←`, or click either thumbstick. |
| Scroll the desktop | Aim at the desktop and move the thumbstick up/down without gripping the panel. |
| Move or tilt a desktop, keyboard or settings panel | Aim at the panel, hold grip, then move or rotate the controller. Release grip to place it. You can also hold the trigger on a desktop panel's bottom `MOVE` strip to drag it. |
| Change panel distance or size | While holding the panel with grip, move the thumbstick up/down for distance or left/right for size. |
| Change desktop screen size or curvature | First aim at the empty area below the screen to reveal its controls. Then hold the trigger on `SIZE` or `CURVE` above the screen and drag horizontally. |

The four circles below a desktop screen are, left to right: `RESET` (reset that panel), `KEYBOARD` (toggle keyboard), `CLOSE` (hide that panel), and `SETTINGS`. Restore hidden screens with `SCREEN 1`–`SCREEN 4` at the top of the keyboard; only monitors supplied by the PC have an image.

At the bottom of the keyboard, `IN SPACE`, `LEFT HAND` and `RIGHT HAND` select a fixed position or attachment to a controller; use the other hand to interact with an attached keyboard. At the top, `IME` switches the PC input method, `RESET` resets the panel layout and `EXIT` ends streaming. If the Steam Frame system menu has taken input focus, close it before interacting with SurfXR.

### Troubleshooting

| Symptom | Check first |
|---|---|
| PC not found or connection never completes | Dashboard is running, the link/address is correct, the device is trusted and firewall rules were added. After changing the PC installation folder, add rules again from the new Dashboard, then run `Find PCs`. |
| No desktop picture | Check the desktop service under PC **Installation**. Read the headset status card's error and suggested action, then select `Desktop` to reconnect. |
| Desktop works but SteamVR is black or fails to start | Check PC SteamVR status, SurfXR driver registration and the headset error. After addressing the error, select `SteamVR` again, or continue with `Desktop`. Confirm that PC and headset use matching releases. |
| Stuttering or high delay | Reduce bitrate first and check link stability. Then adjust resolution or frame rate and restart SteamVR. Observe changes with the HUD and change one setting at a time. |
| No sound or no microphone in an application | Check PC desktop audio/audio-device settings, Windows and headset volume, and mute state. With the default Steam microphone route, select `Steam Streaming Microphone` as the input in your game or voice application. |

Upgrade using complete matching packages and rerun PC installation and Frame install/update. Keep executable and DLL versions together. The ordinary Frame uninstall option preserves settings; **fully uninstall** deletes settings, layouts and logs and is not a routine reconnection step. If the issue persists, include the running PC/Frame versions, reproduction steps and error message using the [problem-reporting guidance](#report-problems) below.

## Start here

| Your goal | Read |
|---|---|
| Install and use the application | [User guide](#user-guide) |
| Explore features and controller controls | [Useful features](#user-features), [buttons and built-in menus](#controller-guide) |
| Describe a feature for your coding agent | [Requirements, prompts and acceptance criteria](surfxr/docs/REQUESTS.en.md) |
| Take over maintenance as an agent | [Agent entry point](#agent-guide) |
| Build or contribute for the first time | [Getting started](surfxr/docs/GETTING_STARTED.en.md), [contributing](CONTRIBUTING.en.md) |
| Add a feature or locate its owner | [Development guide](surfxr/docs/DEVELOPMENT.en.md) |
| Evaluate a change | [Testing and acceptance](surfxr/docs/TESTING.en.md) |
| Install or package a release | [Build and launch entries](surfxr/docs/ONE_CLICK.md), [release packaging](surfxr/docs/RELEASE.md) |
| Prepare a public repository | [Open-source preparation](surfxr/docs/OPEN_SOURCE.en.md), [notices](NOTICE.md) |

See the [English documentation index](surfxr/docs/README.en.md) for the complete reading map. Chinese and English are both welcome in issues, pull requests and requirements.

## Features and supported development

- **PC:** SurfXR Dashboard, device trust and settings, SteamVR driver, video encoding, desktop capture/input/audio and persistent connection service.
- **Frame:** native SurfXR launcher, desktop/VR switching, OpenXR/Vulkan presentation, decoding, tracking/haptics, audio/microphone and local settings UI.
- **SurfXR extensions:** gaze atlases, device state, boundary/IPD synchronization, accepted settings, desktop input and stream lifecycle, maintained at both endpoints.
- **Development:** Windows PC builds; Linux/WSL native and ARM64 cross builds; CPU and isolated loopback tests; explicit GPU fixtures; release integrity checks.

SurfXR is the only network backend. The default protocol version is **21.0.0-dev14**; the **20.14.1** VR path remains explicitly selectable.

PC product builds target Windows x64. Frame releases target the headset OS on ARM64. Linux x86_64 and QEMU support offline development and do not establish headset compatibility. See the [architecture](surfxr/docs/ARCHITECTURE.md) for lifecycle rules and the current 8-bit SDR color contract.

<a id="agent-guide"></a>

## Maintaining and extending the project with an agent

Read [AGENTS.md](AGENTS.md), [ARCHITECTURE.md](surfxr/docs/ARCHITECTURE.md), [TOOLS.md](surfxr/docs/TOOLS.md) and [PROTOCOL.md](surfxr/docs/PROTOCOL.md), then follow the [agent maintenance guide](surfxr/docs/AGENT_GUIDE.en.md).

1. Inspect the working tree and baseline commit, preserve unrelated changes, and establish the environment and authorized actions.
2. Define observable behavior, the owning module, affected interfaces and validation plan.
3. Implement within existing owners. Maintain both endpoints of SurfXR extensions and test configuration migrations.
4. Reuse existing tools. Keep generated output under root `build/`, maintained code within 500 lines per file, and preserve protocol bytes, acceptance serials, queue bounds, frame association and resource lifetimes.
5. Provide evidence using the [test guide](surfxr/docs/TESTING.en.md). Separate builds, CPU/loopback tests, offscreen GPU checks, headset operation and wearer acceptance.
6. Update documentation, commit coherent validated changes, and report commit IDs, untested areas and manual acceptance steps.

You can give your agent this prompt:

```text
Implement this requirement in the current repository:
[Describe the scenario, current behavior, expected behavior and acceptance criteria.]
Read AGENTS.md and the documents required by surfxr/docs/AGENT_GUIDE.en.md.
Preserve unrelated changes. State the owner, affected interfaces and validation plan,
then finish the implementation and necessary checks.
Follow DEVELOPMENT.en.md and TESTING.en.md, update relevant documentation and commit
coherent validated changes. This task authorizes local edits, builds and offline tests;
device deployment, service changes and publication require separately established scope.
Report behavior changes, evidence, untested areas, acceptance steps and commit IDs.
```

Adjust the authorization paragraph to your actual task. See [requirements and examples](surfxr/docs/REQUESTS.en.md). Contributors without a headset can still help and should identify outstanding device checks. Agent-assisted changes use the same [review standards](CONTRIBUTING.en.md) as other contributions.

## Repository layout

- `surfxr/pc/surfxr-server`: the full SurfXR Cargo workspace, PC driver, assets, packaging tools and OpenVR SDK submodule.
- `surfxr/pc/desktop`, `pc/daemon`: Windows capture, input, encoding, service and persistent connection orchestration.
- `surfxr/frame`: Steam Frame client, native launcher, OpenXR/Vulkan, hardware decoding and local UI.
- `surfxr/shared/surfxr`: shared SurfXR codecs, framing, authentication, FEC and SurfXR extensions.
- `surfxr/tests`: offline and isolated loopback tests. `surfxr/docs`: architecture, development and operation guides.

Client executables, services, configuration and installation roots use `surfxr-*`. Internal `surfxr` namespaces and public `surfxr/*.hpp` includes are stable module interfaces. The only network backend is `surfxr`; PC branding remains SurfXR.

<a id="mini-hud"></a>

## Mini HUD

Enable **Mini HUD** under **Diagnostics → Overlays** in Dashboard or the in-stream settings. When enabled, its default layout continuously shows FPS and latency in the upper-right corner during VR streaming. The existing Performance HUD remains independently selectable.

- **HUD content:** independently select FPS, latency, frame interval and decode time; disabling all rows hides the HUD.
- **HUD layout:** horizontal and vertical position from 0 to 100, and size from 50% to 200%.
- **HUD gaze:** gaze-only mode shows values after about 80 ms of gaze and hides them after about 250 ms away. It requires valid eye tracking; invalid tracking hides values. The target marker has a separate switch. Disable gaze-only mode for continuous display.

These settings are local to the client and survive restarts. FPS counts new video images; frame interval uses the existing two-second window. `LAT UP->XR` measures tracking upload to XR submission, and `DECODE` measures reception to decoder output. Neither measures physical display latency. Missing data is shown as `--`.

## Build

PC builds require Windows x64, Visual Studio 2022 C++ tools, Windows SDK, CMake, Python 3 and the Rust toolchain pinned in the workspace. SurfXR's existing `cargo xtask prepare-deps` owns dependency preparation:

```powershell
git submodule update --init --recursive
.\scripts\Build-PC.ps1 -PrepareDependencies
```

Build Frame locally in WSL/Linux or cross-compile ARM64 using the [WSL guide](surfxr/docs/WSL_BUILD.md). Initialize the pinned OpenVR submodule with `git submodule update --init --recursive`; prepare OpenXR headers and system libraries as documented. An explicitly selected SSH build host is also supported. `-BuildOnly` compiles and runs offline checks without installation, pairing or service changes.

Compilation, caches, validation and packaging staging stay under root `build/`: native PC output at `build/pc/native/Release`, complete PC output at `build/pc/surfxr_streamer_windows`, Cargo at `build/cargo`, local client output at `build/frame/<configuration>`, and SSH reports at `build/reports/frame-ssh/validation.json`. Final archives and installers go under `release/`.

`scripts/Build-PC.ps1 -Package -Setup -FrameArchive <Frame ZIP>` creates the portable ZIP and Setup from the same payload. See [release packaging](surfxr/docs/RELEASE.md). The PC builder does not automatically run all native CTest or Cargo unit tests; follow [TESTING.en.md](surfxr/docs/TESTING.en.md). `scripts/Start-Frame.ps1 -NoStart` can still deploy and configure services. Prefer local builds; compilation on a headset requires an explicit request.

<a id="report-problems"></a>

## Requirements, bug reports and acceptance

Describe the conditions, action, current result and expected result, followed by normal, failure and regression checks. Identify the PC/Frame versions actually running; edited source does not update an installation.

Use the [feature template](.github/ISSUE_TEMPLATE/feature_request.md), [bug template](.github/ISSUE_TEMPLATE/bug_report.md) and [PR template](.github/pull_request_template.md), or the [investigation prompts](surfxr/docs/REQUESTS.en.md). Sanitize logs and attachments. Do not commit pairing material, keys, user settings or private media.

Record expected and actual results for each acceptance item and the corresponding version. A build does not establish correct visuals, audio, input or comfort. [VALIDATION.md](surfxr/docs/VALIDATION.md) does not replace evidence for the selected public commit; use the [acceptance template](surfxr/docs/TESTING.en.md).

## Provenance, licensing and public preparation

Project-owned code and documentation are licensed under the [GNU GPL v3 (GPL-3.0-only)](LICENSE), with the notice `Copyright (c) 2026 F-19-F`. Distribution must comply with GPL v3, including its corresponding-source requirements; retain copyright and third-party notices.

SurfXR, OpenVR, shaders and other third-party components retain their own licenses and attribution; see [NOTICE.md](NOTICE.md). GPL-3.0-only does not replace those third-party terms. See [open-source preparation](surfxr/docs/OPEN_SOURCE.en.md) for release preparation.

GitHub Actions provides **Build only** and **Build and publish release**, with a custom version and pre-release option; see the [release guide](surfxr/docs/RELEASE.md#github-actions).
