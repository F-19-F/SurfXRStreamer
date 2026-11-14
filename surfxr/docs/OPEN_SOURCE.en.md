# Preparing the repository for public release

English | [简体中文](OPEN_SOURCE.md)

This checklist helps maintainers prepare the project for public collaboration. It records available entry points and work that must actually be completed. It does not claim a completed licensing review, security audit, clean-environment build or device acceptance of the current version.

The preparation baseline recorded on 2026-11-07 (Asia/Shanghai) is:

| Area | Available evidence | Remaining work |
|---|---|---|
| Both endpoints and ownership | PC, Frame and shared code are together with architecture/tools guides | Validate the chosen release in a fresh environment |
| Contribution and agent entry points | README, contributing, development/test/requirement guides and issue/PR templates | Verify links on the selected hosting platform |
| Provenance and notices | Root NOTICE and imported ALVR/OpenVR/shader notices | Review the entire distribution and new dependencies |
| Project-wide license | Root [LICENSE](../../LICENSE) uses GPL-3.0-only for project-owned code and documentation; third-party terms remain | Verify that distributions include GPL-3.0-only and relevant third-party notices |
| Validation | Evidence must identify its own revision and environment | Generate evidence for this release; do not reuse old conclusions |
| Hosting and support | No invented URL, contacts or maintenance commitments | Select actual hosting, issue intake and private security reporting |

## 1. Establish rights and provenance

Project-owned code and documentation use the root [GNU GPL v3 (GPL-3.0-only)](../../LICENSE). Read [NOTICE.md](../../NOTICE.md), [architecture provenance](ARCHITECTURE.md) and imported notices. Cargo license fields identify the GPL derivative; the imported workspace LICENSE retains the upstream ALVR MIT notice; the original SurfXR, OpenVR, vendor and shader licenses remain in effect.

- [x] Add the rights holder's chosen GPL-3.0-only license for project-owned code and documentation, with links from README/contributing.
- [ ] Verify that distributions include the root LICENSE and establish rights for newly added images, fonts and other assets.
- [ ] Review SurfXR, OpenVR, vendor and shader sources, versions and retained notices.
- [ ] Record source, version and license for new dependencies/copied code, with relevant notices in binary packages.
- [ ] Review optional encoders, runtimes and bundled tools for the actual distribution configuration.
- [ ] Resolve and record `cargo-about` metadata diagnostics; a successful packaging exit does not complete the review.
- [ ] Exclude private SDK/sysroot snapshots, user data and assets without established distribution rights.

The rights holder has selected GPL-3.0-only for the project. This does not replace individual checks of third-party components or newly added assets, nor authorize changes to their attribution or licensing terms.

## 2. Review what will be public

Review the working tree, commits/branches/tags to be pushed, attachments, logs and final archives/installers. `.gitignore` does not remove already tracked history.

- [ ] Exclude credentials, pairing records, trust stores, sessions/user settings, private host lists and SDK snapshots.
- [ ] Sanitize account/host identifiers, local paths and raw packet payloads. Retain only evidence needed to reproduce the issue.
- [ ] Exclude private desktop captures, recordings, audio and test media without distribution authorization.
- [ ] Keep generated files, Cargo caches, downloaded dependencies, staging and reports under root `build/`; keep final distributions under `release/`, outside source commits.
- [ ] Use suitable scanning tools and manually review their findings. One keyword search cannot prove that no secrets remain.

If an exposed secret is found in history, stop distributing it and have the maintainer handle revocation/rotation and cleanup. Deleting the current file is insufficient. History rewriting and force-pushing require explicit scope and are not incidental documentation maintenance.

## 3. Validate a new contributor's workflow

Use a clean checkout/worktree and an identified environment rather than depending on maintainer caches or sibling projects:

- [ ] [Getting started](GETTING_STARTED.en.md) and [WSL builds](WSL_BUILD.md) describe adequate prerequisites without private default paths.
- [ ] `scripts/Build-PC.ps1 -PrepareDependencies` creates the full PC payload, with relevant native/Cargo test results recorded separately.
- [ ] `build_viewer.py --local` generates the selected configuration's report, distinguishing x86_64, QEMU ARM64 and target-SDK ARM64.
- [ ] Shader tools/versions are documented and `--check` matches tracked assets.
- [ ] Structure checks pass without silently expanding imported-file exceptions.
- [ ] Build commands do not install, pair or change application services; users understand the deployment semantics of `-NoStart`.
- [ ] Contributors without devices can run document/CPU/loopback checks and clearly identify remaining hardware acceptance.

Mark only checks actually performed and put their evidence in the chosen version's reports. Runtime source changes invalidate assumptions from older results.

## 4. Produce a traceable release

Use existing owners from [RELEASE.md](RELEASE.md). Freeze source, validate and commit before packaging.

- [ ] PC and Frame match; record source, toolchain, target OS/runtime and known limits.
- [ ] Build ARM64 against the target SDK and verify manifests, SHA-256 values and package assets.
- [ ] Exclude private configuration/material and preserve required notices.
- [ ] Record build, offline, offscreen GPU, device and wearer checks separately, with unrun items identified.
- [ ] Provide installation/upgrade/uninstall and user-data preservation evidence, or report those limits.
- [ ] Provide installation instructions, changes, compatibility requirements and rollback guidance.

Build output remains separate from running installations. Creating packages does not upgrade an installation. Do not describe unverified outputs as stable or universally compatible.

## 5. Open collaboration channels

- [ ] Select actual hosting and verify README, navigation and relative links.
- [ ] Decide whether issues/PRs are open. The `.github` templates can be adapted for other platforms.
- [ ] State maintenance scope, platforms and available device-acceptance resources.
- [ ] Establish a usable private security-reporting channel before publishing contact details.
- [ ] Configure actual review/merge permissions and automated checks; guidance is not an enabled CI or branch-protection policy.
- [ ] Have someone without maintainer context, or an agent without prior chat history, complete a small reviewable contribution using the requirement template.

This document does not promise existing CI, dedicated staffing, a CLA/DCO, support deadlines or vulnerability response times. Update it when those policies and configurations actually exist.
