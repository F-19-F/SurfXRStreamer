# SurfXR PC workspace

This is the retained PC server and Dashboard of **SurfXR 1.0.0**, based on [ALVR](https://github.com/alvr-org/ALVR). Original ALVR licensing is preserved in [LICENSE](LICENSE); third-party terms remain with their owning sources.

The original Android/OpenXR/mock clients, Android graphics and ADB installation, upstream download launcher and update check are removed. The native Steam Frame client lives in [../../frame](../../frame); the repository maintains both protocol endpoints together.

Build from the repository root with `scripts/Build-PC.ps1`. It delegates to this workspace's `cargo xtask` and the existing native build owner. Cargo output belongs in repository `build/cargo`; streamer staging belongs in `build/pc/surfxr_streamer_windows`. Running Cargo from this directory uses the included `.cargo/config.toml`.

The PC Dashboard includes Steam Frame installation guidance and invokes the existing `scripts/Install-Frame.ps1` entry. Use matching SurfXR PC and Frame packages; this fork does not download original ALVR releases. The independent update feed is not configured.

See the [project README](../../../README.md), [architecture](../../docs/ARCHITECTURE.md), [protocol](../../docs/PROTOCOL.md), [maintenance guide](../../docs/AGENT_GUIDE.md) and [migration record](../../docs/SURFXR_MIGRATION.md).
