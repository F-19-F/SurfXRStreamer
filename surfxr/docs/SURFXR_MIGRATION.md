# SurfXR 1.0.0 migration

SurfXR is an independently maintained derivative of ALVR.

## Product and ownership

- PC Dashboard, SteamVR driver, native desktop/module/daemon, Cargo crates, CMake targets, public includes/namespaces, service/registry/firewall identities and packaging use SurfXR / `surfxr` / `SURFXR_`.
- Frame uses `surfxr-client`, `surfxr-client.service`, `surfxr-dashboard.service`, and `%h/.local/share/surfxr-client/launcher/surfxr-client`. Source, build output and installed applications remain separate.
- Cargo, native CMake, Frame UI and package metadata start at product version `1.0.0`. The ALVR-derived default wire revision remains `21.0.0-dev14`; historical v20 vectors remain offline fixtures. Product version never selects an encoder layout or changes the discovery protocol ID.
- Fixed-size discovery markers, authentication domain separators and historical preference header readers retain their exact compatibility values. ALVR source URLs, copyright/license texts and pinned dependency URLs remain truthful. They do not brand SurfXR, implement an ALVR updater, or install an original ALVR client.

## Removed unused implementations

The original Android/OpenXR/mock clients, their graphics renderer, ADB crate/runtime, Android hardware probing, Android build/package/dependency actions, upstream release launcher, updater worker/popup and developer page are removed. OpenVR samples, regeneration scripts, unused C# bindings and SDK implementation source are removed; required public headers and native libraries/notices remain.

The old alternate server C API and its build action are removed. The seven logging/hash callbacks actually used by the native SteamVR driver remain in `server_openvr/src/native_log.rs`. Compatibility-only session schema fields remain serialized and hidden; deleting them would change stored/configuration formats. Runtime encoders, transport extensions, driver assets and tests are retained when they have actual consumers.

Modified oversized maintained files were split into encoding/video/audio/tracking/controller/connection/extra schemas and field defaults; automatic input mapping and gesture detection owners; driver input, Linux image allocation/worker, and AMF encoder configuration owners. No imported hash exception was enlarged or recomputed. Surviving third-party exceptions retain their original bytes and hashes.

## Panel and installation

About shows **SurfXR v1.0.0**, the SurfXR project link and project license. The About page offers manual background checks of https://github.com/F-19-F/SurfXRStreamer/releases and a release-page link. It compares stable semantic product versions, reports missing releases/request failures, and does not automatically download or install. Project-owned code uses GPL-3.0-only; original ALVR MIT attribution remains.

**Installation → Steam Frame client** guides SSH setup, release extraction, network selection, plan preview, installation and pairing. The UI forwards individual arguments to existing `scripts/Install-Frame.ps1`; it does not assemble shell commands or implement SSH/file transfer. SSH credentials are entered in the interactive installer window. Missing scripts/manifests, invalid targets and failed exits are reported. Packaged installs use `frame-installer/Install-Frame.ps1`; development builds locate the existing repository entry. Manual scripts remain supported. Plan does not deploy or start/stop services.

New service and configuration roots isolate SurfXR from the prior installation. Existing installations are not uninstalled or modified by this source migration. If manually transferring local preference files, historical headers remain readable and subsequent writes use SurfXR headers. Do not copy pairing secrets or arbitrary user settings into source/release packages.

## Maintenance and validation boundary

Use [TESTING.md](TESTING.md) to validate the selected public commit. Source checks, offline builds and actual headset acceptance are separate.

