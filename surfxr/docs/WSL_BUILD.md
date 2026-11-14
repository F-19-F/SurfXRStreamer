# WSL / Linux builds

The Frame client can be built without connecting to a headset. Use the existing `scripts/build/build_viewer.py --local` owner. Local execution neither opens SSH nor installs/starts application services. The PC Windows capture, driver and Dashboard build still uses `scripts/Build-PC.ps1` and MSVC/Rust.

## Environment

Use Ubuntu 24.04 or an equivalent Linux development environment with Python 3.10+, CMake 3.16+, Ninja, GCC 13+, pkg-config, OpenSSL 3, Vulkan, FFmpeg 6, PulseAudio, GTK3, cairo and json-c development packages. The pinned OpenVR SDK submodule supplies headers and matching Linux libraries. Run `git submodule update --init --recursive` from the repository root before building; GitHub source ZIPs do not contain submodule contents.

Ubuntu packages: `cmake ninja-build g++ python3 pkg-config libopenxr-dev libvulkan-dev libavcodec-dev libavutil-dev libavformat-dev libswscale-dev libpulse-dev libgtk-3-dev libjson-c-dev libssl-dev libxml2-dev glslang-tools spirv-tools`.

OpenXR headers must include `XR_META_performance_metrics` (the distribution loader can be older). The verified header revision is Khronos OpenXR-SDK `release-1.1.36`. Download `openxr.h`, `openxr_platform.h` and `openxr_platform_defines.h` from that revision's `include/openxr/` into the ignored `surfxr/deps/openxr/include/openxr/` directory, then pass that include root below. The official source is https://github.com/KhronosGroup/OpenXR-SDK/tree/release-1.1.36/include/openxr .

Generated shader headers use glslang 15.1.0 (generator 11). The builder runs all three existing shader compilers with `--check`, plus the gamma math checks. A different compiler may generate different bytecode: use the recorded compiler, or regenerate through those tools and review/commit the generated headers before validation. Never edit SPIR-V words manually.

## Native Linux CPU validation

From the repository root inside WSL/Linux:

```bash
mkdir -p "$PWD/build/tmp"
export TMPDIR="$PWD/build/tmp"
export PYTHONDONTWRITEBYTECODE=1
python3 scripts/build/build_viewer.py --local \
  --build-directory "$PWD/build/frame/linux-x64" \
  --cmake-argument="-DOPENXR_INCLUDE_DIR=$PWD/surfxr/deps/openxr/include" \
  --jobs 8 --build-timeout 1200
```

This produces x86_64 executables and runs the full offline CTest suite, including staged native installation with fake service commands. x86_64 output cannot be packaged as a Frame release.

Keep the temporary-directory environment above for all build variants and standalone shader checks: existing shader tools use the process temporary directory for intermediate SPIR-V. This keeps compiler output under repository-root `build/`. For check selection and evidence limits, see [TESTING.md](TESTING.md).

## ARM64 cross build for offline checks

Prepare Ubuntu multiarch `arm64` development libraries corresponding to the packages above, plus `g++-aarch64-linux-gnu` and `qemu-user-static`. Point arm64 package sources to Ubuntu Ports. Configure the distribution's QEMU binfmt support for ARM64: CTest invokes QEMU explicitly, and launcher tests also execute ARM64 children. This runs CPU emulation only, not headset rendering. On distributions with conflicting multiarch GIR metadata, use a dedicated cross-build environment rather than replacing workstation libraries.

```bash
python3 scripts/build/build_viewer.py --local \
  --build-directory "$PWD/build/frame/linux-arm64" \
  --cmake-argument="-DCMAKE_TOOLCHAIN_FILE=$PWD/surfxr/cmake/toolchains/linux-arm64.cmake" \
  --cmake-argument="-DOPENXR_INCLUDE_DIR=$PWD/surfxr/deps/openxr/include" \
  --jobs 8 --build-timeout 1200
```

Ubuntu multiarch output links Ubuntu's library versions. Passing CPU tests does not make that output compatible with the headset's runtime ABI.

## ARM64 build for deployment

Use a read-only system SDK snapshot from the target OS as a sysroot. Include `/usr/include`, `/usr/lib/pkgconfig`, `/usr/share/pkgconfig`, top-level shared libraries and their symlink targets, startup objects, `libc_nonshared.a`, GLib/DBus generated include directories, PulseAudio support libraries, and the SteamVR OpenVR headers and ARM64 OpenVR/OpenXR loaders. Preserve paths and symlinks, including `/lib/ld-linux-aarch64.so.1`. Store SDK files outside tracked sources; never copy user data or credentials. The headset only supplies system development files; compilation runs in WSL.

Use the existing cross-build command with `--build-directory "$PWD/build/frame/target-arm64"` and add `--cmake-argument=-DSURFXR_EXO_SYSROOT=/path/to/target-sdk`. Omit the OpenXR include override when the SDK contains the required OpenXR headers. If the target OS omits those development headers, use the pinned Khronos headers from the Environment section with `--cmake-argument="-DOPENXR_INCLUDE_DIR=$PWD/surfxr/deps/openxr/include"`; keep the target SDK runtime loaders. The toolchain resolves headers, pkg-config metadata and libraries against that SDK and forwards the sysroot selection into CMake compiler checks. QEMU must use the same target loader/libraries for offline tests.

SDK freshness or version equality must not become a hard installation/startup check. An older SDK with compatible runtime dependencies remains valid. Any SDK-age/version diagnostic is advisory, and deployment must resolve actual missing/incompatible dependencies rather than asking the user to bypass a version gate. See the [architecture compatibility contract](ARCHITECTURE.md).

The installer verifies the release manifest, file hashes and ARM64 architecture; it does not preflight system shared-library versions. Build against an ABI-compatible SDK so the system loader can resolve the application's dependencies. A headset OS upgrade alone does not invalidate a working build. If its dependencies are no longer available/compatible, refresh the SDK and use a fresh build directory to avoid cached library paths from the previous SDK.

Target-SDK ARM64 builds now prepare private media libraries automatically through `build_viewer.py --local`; install `make`, `meson` and Python 3.12+ in the build environment in addition to the tools above. Verified upstream source archives are cached under `build/deps/frame-ffmpeg`; dependency compilation and staging stay under the selected `build/frame/<configuration>`. FFmpeg 7.1.1 and dav1d 1.5.1 are reproducible source pins, not required headset system versions. Media runtime libraries, licenses, source archives and rebuild configuration are recorded in `runtime_sha256` and shipped by the existing packager. The headset does not need FFmpeg development files or the same system FFmpeg major.

The report lives in the selected build directory as `validation.json`. It records actual ELF architecture, input/output hashes, CTest output, warnings and command results. Freeze sources during final validation; any input change requires another build. Local reports store a repository-relative build directory so the Windows packager can verify the same mounted files. See [RELEASE.md](RELEASE.md) for packaging.

Native Linux release builders can pass `--cmake-argument=-DSURFXR_BUNDLE_MEDIA=ON` to build the same private FFmpeg/dav1d libraries without cross compilation. Install `make`, `meson`, `nasm` and Python 3.12+. The release packager still requires ARM64 binaries and all offline checks; native x86_64 is useful for validation only. GitHub Actions use this option on Ubuntu 24.04 ARM64. Source package hashes and required decoder checks are identical to the existing target-SDK path.

For native media-bundled builds, CTest supplies the pinned OpenVR library directory to the dashboard/viewer offline tests. This supports clean CI hosts without SteamVR; packaged RPATHs still contain only application-relative and installed runtime locations, never the build SDK directory.
