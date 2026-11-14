# Source and license notices

Project-owned SurfXR code and documentation are licensed under the [GNU GPL v3 (GPL-3.0-only)](LICENSE), copyright (c) 2026 F-19-F. Redistribution is governed by GPL v3, including its corresponding-source requirements; retain copyright and third-party notices.

SurfXR includes a modified, server-only derivative of the ALVR workspace and its license at `surfxr/pc/surfxr-server/LICENSE`. The OpenVR SDK Git submodule retains `surfxr/pc/surfxr-server/openvr/LICENSE`. Vendor headers, shaders, assets and libraries retain their own accompanying licenses, copyright notices and redistribution conditions. The root GPL license does not replace or remove those third-party terms.

Shader attribution and redistribution terms are in `surfxr/frame/xr/shaders/image_enhancement.LICENSE`. Existing ALVR xtask and Frame release tooling include runtime and dependency notices. When distributing project-owned code in source or binary form, include the root `LICENSE` together with the applicable third-party notices; verify the contents of the actual distribution.

Source provenance and imported-file maintenance ownership are recorded in `surfxr/docs/ARCHITECTURE.md`.

Frame embeds a renamed subset of Noto Sans CJK SC 2.004, © 2014–2021 Adobe (http://www.adobe.com/), under the SIL Open Font License 1.1. The retained license is `surfxr/frame/ui/fonts/NotoSans.LICENSE`; the release and installed client include it. The subset is named SurfXR UI Sans. Its pinned source and offline rebuild command are documented in `surfxr/docs/LOCALIZATION.md`.
