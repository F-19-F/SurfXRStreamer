# Getting started

English | [简体中文](GETTING_STARTED.md)

Run these commands from the repository root. Run Linux commands inside WSL/Linux. Use your own checkout paths rather than paths from historical reports. Read [AGENTS.md](../../AGENTS.md) for required workflow and [TOOLS.md](TOOLS.md) for authoritative command owners.

## Choose an environment

| Work | Requirements | Does not establish |
|---|---|---|
| Reading, documentation, structure checks | Git, Python 3.10+, editor | Successful compilation or execution |
| PC product build | Windows x64, VS 2022 C++ x64 tools, Windows SDK, CMake, Rust, Python | Real capture, encoding or headset rendering |
| Native Frame CPU/loopback tests | WSL/Linux and [documented dependencies](WSL_BUILD.md) | ARM64 ABI, hardware decoding or comfort |
| ARM64 CPU cross validation | ARM64 toolchain, multiarch libraries, QEMU | Headset OS compatibility |
| Deployable Frame package | Target OS SDK/sysroot, ARM64 build and validation report | Deployment or device acceptance |
| Device acceptance | Matching PC/Frame installations, runtime, hardware and authorization | Other hardware or versions |

You can contribute documentation, models, format checks and offline tests without a headset. List outstanding hardware checks explicitly.

## Obtain and inspect the source

Clone or fork [SurfXRStreamer](https://github.com/F-19-F/SurfXRStreamer) with `--recurse-submodules`. OpenVR is a pinned Git submodule; no private sibling checkout is required. After pulling an existing checkout, initialize its submodules:

```powershell
git submodule update --init --recursive
git status --short
git rev-parse HEAD
python --version
python scripts/build/check_structure.py
```

Read [architecture](ARCHITECTURE.md), [tools](TOOLS.md) and [SurfXR contracts](PROTOCOL.md). Keep generated output in this checkout's root `build/`, separate from installed applications.

## First Windows PC build

Install VS 2022 C++ x64 tools and Windows SDK, and make `cmake`, `cargo`, `rustup` and `python` available. `surfxr/pc/surfxr-server/rust-toolchain.toml` pins Rust; rustup selects it inside that workspace. Do not change the pin to hide an environment issue.

```powershell
.\scripts\Build-PC.ps1 -PrepareDependencies
```

The existing `cargo xtask prepare-deps --platform windows` downloads/prepares dependencies, then the public entry builds native modules and the complete streamer. Later builds can use `./scripts/Build-PC.ps1`. These steps may need network access and do not install or restart application services.

| Output | Location |
|---|---|
| Native desktop, module and daemon | `build/pc/native/Release` |
| Complete PC build | `build/pc/surfxr_streamer_windows` |
| Cargo compilation cache | `build/cargo` |

The PC builder does not run the entire CTest or Rust test suite. Add relevant checks from [TESTING.en.md](TESTING.en.md). The existing `scripts/build/build_surfxr_windows.ps1` can build native-only changes; Dashboard/driver changes and delivery still require the complete PC build.

## First Frame build

Prepare Linux dependencies, OpenXR headers and the recorded glslang version using [WSL_BUILD.md](WSL_BUILD.md). Start with native x86_64 validation, then prepare ARM64 and the target SDK as needed.

Local builds explicitly use `build_viewer.py --local`. A build directory must be one named child of root `build/frame/`, such as `linux-x64` or `target-arm64`. The builder checks shaders, compiles, runs CPU/isolated-loopback CTest and writes `validation.json` there.

Keep shader temporary output inside the repository too:

```bash
mkdir -p "$PWD/build/tmp"
export TMPDIR="$PWD/build/tmp"
export PYTHONDONTWRITEBYTECODE=1
```

Then run the appropriate full command from [WSL_BUILD.md](WSL_BUILD.md). Compilation with `--local` runs on Linux/WSL; Windows can package a validated local report using [RELEASE.md](RELEASE.md).

Do not select the headset as the default SSH build host. An explicitly selected independent builder needs its own toolchain. Build on a headset only when explicitly requested.

## Build, install and run are separate operations

| Entry | Effect |
|---|---|
| `scripts/Build-PC.ps1` | Builds PC software; no service installation/restart |
| `build_viewer.py --local` | Local Frame build and offline validation; no headset connection |
| `scripts/Start-Frame.ps1 -Plan` | Prints the selected source orchestration plan |
| `scripts/Start-Frame.ps1 -BuildOnly -FrameSsh <builder>` | Compiles and validates on the selected SSH host; no deployment |
| `scripts/Start-Frame.ps1 -NoStart` | May stop the managed viewer, install and configure services; only streaming startup is omitted |
| `scripts/Install-Frame.ps1 -ReleaseDirectory <unpacked-release> -Plan` | Validates and displays the precompiled installation plan |
| Actual installation/start commands | Change the target system; follow [ONE_CLICK.md](ONE_CLICK.md)/[RELEASE.md](RELEASE.md) within the authorized scope |

PC Dashboard owns device trust; Frame Dashboard selects Desktop or SteamVR. Installation roots remain separate from build output. Installation, pairing and acceptance are different milestones.

## Troubleshooting

| Symptom | Check first | Avoid |
|---|---|---|
| VS/Windows SDK not found | VS 2022 components, script error and CMake selection | Creating another build entry |
| Missing Rust/libclang/FFmpeg | Pinned toolchain and dependency-preparation log | Copying unknown DLL versions |
| `--local requires Linux` | Whether compilation runs inside WSL/Linux | Removing `--local` and accidentally selecting SSH |
| Missing OpenXR extensions | Documented headers and include path | Deleting extension code |
| Shader `--check` fails | glslang version, generator and source changes | Hand-editing SPIR-V |
| ARM64 tests cannot start children | QEMU, binfmt, target loader and sysroot | Claiming unexecuted tests passed |
| Missing target shared libraries | Target ABI and correct SDK | Replacing headset system libraries |
| Device discovery fails | Matching installations, Dashboard trust, interface/address and firewall | Injecting fixtures into a real service |
| Packaging rejects a report | Input hashes, architecture, report path and complete outputs | Editing success/hash fields |

When asking for help, provide the commit, command, platform/tool versions, first useful error and a short sanitized log. Record environment blockers and continue independent checks without claiming a successful build.
