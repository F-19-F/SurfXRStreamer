# SurfXR PC / SurfXR client packaging

Build Frame ARM64 against the target OS SDK and run its CPU/offline tests locally in WSL as described in [WSL_BUILD.md](WSL_BUILD.md). Ubuntu multiarch output alone is not a deployment compatibility check. Freeze edits and commit validated sources before packaging. From the Windows repository root:

```powershell
python scripts/build/build_viewer.py --local --output build/frame/target-arm64/validation.json --package-only --package-release release/SurfXR-SteamFrame.zip
.\scripts\Build-PC.ps1 -Package -Setup -FrameArchive .\release\SurfXR-SteamFrame.zip
```

The local packaging path copies verified ARM64 output directly; it does not connect to a headset. Reports for x86_64 output are rejected. An explicitly selected SSH ARM64 builder remains supported with `--ssh user@build-host` instead of `--local`.

Installation verifies the release manifest, payload hashes and architecture without a system shared-library version precheck. The delivered binaries need compatible runtime dependencies, not equal SDK/OS versions. If OS library changes break ABI compatibility, refresh the SDK and rebuild in a fresh directory, or package compatible runtime dependencies.

SDK-age/version comparison and dependency preflight are advisory diagnostics, not installation or streaming admission gates. Never refuse a working older-SDK build merely because the headset OS differs. The following loader check is a maintainer verification step, not a new launcher/installer check; real missing dependencies must be repaired in the delivered build. See [ARCHITECTURE.md](ARCHITECTURE.md).

Before calling a deployment successful, run the installed viewer with `--help` on the target and check its exit status. This exercises the real system loader without opening XR or starting a stream:

```powershell
ssh -n steamos@frame '~/.local/share/surfxr-client/bin/surfxr-viewer --help'
```

If the loader reports a missing shared library, inspect the installed executable with `ldd`, refresh the target SDK, rebuild in a fresh directory and reinstall through the existing installer. Never alias a different FFmpeg ABI major with a symlink. Hash verification and offline QEMU tests only verify the selected SDK, not the currently installed headset OS. Then start through the installed launcher and check live viewer status for decoded/submitted frames; report wearer acceptance separately.

The Frame release bundles private FFmpeg shared libraries (with dav1d linked in), licenses, corresponding source archives and the rebuild recipe under `payload/build/runtime`. The installed viewer loads these from its own `lib` directory. The packager requires a validated media payload and checks its hashes; this is package completeness verification, not an SDK-version gate at installation or startup. Never ship a thin executable that requires users to keep the build SDK or its FFmpeg major installed. Check `ldd` on the installed viewer and confirm its media libraries resolve inside the application directory.

The Frame ZIP contains ARM64 binaries, embedded native installation assets, release manifest, SHA-256 hashes and bundled Windows SSH/Python tools. The sole installation profile is `surfxr`. It needs no source checkout on Frame. The PC portable package includes Dashboard, SteamVR driver, frame worker, desktop module/daemon, required runtime libraries and the selected Frame installer.

PC builds additionally own `frame-installer/Install-Frame.ps1` and its `launch/{Start-Frame,frame_release,frame_release_commands}.ps1` helpers (copied from repository `scripts/launch/`). Portable packaging requires all four and includes them in its file hashes; Setup follows that manifest. Deploy this directory with Dashboard, including binary-only maintenance updates. Do not rewrite an older `frame-client/release-manifest.json` or replace its hash-covered scripts to make new Dashboard flags work: the PC adapter accepts the independently selected, intact ARM64 package. New standalone Frame archives also include the shared command helper for manual Plan/execution.

PC packaging requires `SURFXR_MSVC_REDIST_DIR` to name the installed Visual Studio x64 `Microsoft.VC143.CRT` redistributable directory. Set `SURFXR_MAKENSIS` to the NSIS 3 Unicode compiler. `scripts/Build-PC.ps1 -Setup` includes portable packaging; `-FrameArchive` is resolved relative to the caller before entering the Cargo workspace. The equivalent existing low-level owner is `cargo xtask package-streamer --portable --setup --no-rebuild`. Both PC packages are written to root `release/`, with staging under `build/package/windows`; Frame staging and bundled-tool caches use `build/package/frame` and `build/deps/release-tools`. Build artifacts, downloaded dependencies and release archives are ignored by Git.

Installation is explicit: unpack the Frame ZIP and run `Install-Frame.cmd`, or the PC package's Frame installer entry. The installed service executes `%h/.local/share/surfxr-client/launcher/surfxr-client`. Its UI is named **SurfXR**; the PC UI remains **SurfXR**. The new installation uses its own configuration and state roots and does not migrate another installed client. Dashboard Installation pairs the successfully installed SSH target through the current session owner. Manual script installation still requires pair/trust in Devices. Both leave streaming idle.

When upgrading the PC service, install the complete matching payload through `surfxr-daemon.exe --install-service` with the intended service options, then verify that `SurfXRDesktop` executes the installed `surfxr-daemon.exe`. Service installation does not update Windows Firewall. After changing executable names or installation paths, refresh the firewall rules through the new Dashboard's existing firewall action and approve Windows UAC. Rules tied to a previous executable path do not cover the renamed program. For a manually restricted installed-daemon rule, desktop discovery and streaming use TCP 19950 and UDP 19950–19952; limit remote addresses to the intended headset subnet. Dashboard and SteamVR have separate rules. Verify discovery from the headset before diagnosing a streaming failure.

Build/offline validation does not certify headset rendering or wearer acceptance. Record those separately before a production release.

OpenXR is provided by SteamVR on the headset, not bundled in the release. The CI link reference uses SteamVR's unversioned `libopenxr_loader.so` name; linking Ubuntu's `libopenxr_loader.so.1` produces a package that passes tests on Ubuntu but exits 127 on Frame before connecting. The local release builder checks the final ELF dependencies to prevent that mismatch. See [WSL_BUILD.md](WSL_BUILD.md) for the build-only reference; no binary patching or additional installed library is involved.

## GitHub Actions

The repository provides two manual Actions. Push the workflows to the default branch before opening **Actions → Run workflow**.

- **Build only** (`build.yml`): optional product SemVer; builds/tests Frame ARM64 and Windows, then uploads packages as run artifacts. It has read-only repository permissions and never creates tags or releases.
- **Build and publish release** (`release.yml`): required version without `v` (for example `1.2.0` or `1.2.0-rc.1`) and a **Mark as pre-release** checkbox. It builds the same artifacts, verifies all three packages and their SHA-256 files, then creates `v<version>` at the selected workflow commit. Existing tags/releases are not overwritten. A version with a prerelease suffix requires the checkbox.

Both call `build-packages.yml`, which reuses `build_viewer.py`, `scripts/Build-PC.ps1`, Cargo xtask and the existing package owners. Assets are `SurfXR-SteamFrame.zip`, `SurfXR-Windows-portable.zip`, `SurfXR-Windows-setup.exe` and their `.sha256` files. Frame validation reports are separate run artifacts. Only the final publish job has `contents: write`; it uses the built-in `GITHUB_TOKEN`, with no personal token required.

The public ARM64 runner is `ubuntu-24.04-arm`; Windows uses `windows-2022`. `scripts/ci` prepares runner tools, not another application builder. Frame builds private FFmpeg/dav1d from the existing pinned sources through `SURFXR_BUNDLE_MEDIA=ON`. Its system-library baseline is Ubuntu 24.04 ARM64; this is not evidence of loader compatibility or headset acceptance on every Steam Frame OS. Target-SDK builds remain available for a different ABI baseline.

Windows caches Cargo downloads/compiled outputs and the license scanner under `build/`. Cargo keys include the toolchain/configuration, lockfile and source revision, with fallback to compatible earlier builds; Cargo still checks fingerprints and release-version environment changes. Native dependency caches require an exact runner-image and preparation-recipe match; only a hit skips `-PrepareDependencies`. Delete the corresponding Actions cache to force a dependency refresh (including the upstream FFmpeg `latest` asset). Regressions use `cargo test --release` to reuse the application dependency profile instead of building the Dashboard graph again in debug. A cold cache still requires the real application dependencies and native compilation.

Windows xtask downloads the official `cargo-about 0.9.3` binary through its private `scripts/build/prepare_cargo_about.py` helper, checking pinned archive and executable SHA-256 values, including cached executables. It no longer compiles the tool's dependency tree with `cargo install`. License generation uses the locked Windows target graph and fails on missing/unresolved licenses; `about.toml` uses the current scanner schema. Non-Windows local packaging retains the pinned Cargo installation path. Portable packaging uses .NET ZIP APIs available in both Windows PowerShell 5.1 and PowerShell 7; the xtask child also clears inherited `PSModulePath`. CI exercises both shells with isolated ZIP/Setup fixtures and never runs the generated installer.

`SURFXR_RELEASE_VERSION` supplies the Dashboard product version and Frame manifest without editing committed sources. PC packaging receives the same value as `SURFXR_RELEASE_ID`. Local builds default to the Cargo workspace version; wire revision `21.0.0-dev14` stays independent. The source commit in each manifest identifies the actual workflow checkout.
