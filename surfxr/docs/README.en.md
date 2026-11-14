# Documentation

English | [简体中文](README.md)

This repository maintains **SurfXR** for Windows PC and **SurfXR** for Steam Frame together. These guides serve requirement authors, human contributors and coding agents without depending on a particular agent product, private plugin or fixed maintainer path.

## Suggested reading

| Role | Reading order |
|---|---|
| Requirement author | [Project README](../../README.en.md) → [requirements](REQUESTS.en.md) → [acceptance](TESTING.en.md) |
| New contributor | [Contributing](../../CONTRIBUTING.en.md) → [getting started](GETTING_STARTED.en.md) → [development](DEVELOPMENT.en.md) |
| Coding agent | [AGENTS.md](../../AGENTS.md) → [architecture](ARCHITECTURE.md) → [tools](TOOLS.md) → [SurfXR](PROTOCOL.md) → [agent guide](AGENT_GUIDE.en.md) |
| Reviewer | Requirement/PR → [development](DEVELOPMENT.en.md) → [testing](TESTING.en.md) → affected contracts |
| Release maintainer | [Public preparation](OPEN_SOURCE.en.md) → [WSL builds](WSL_BUILD.md) → [release packaging](RELEASE.md) → evidence for the selected commit |

## Document responsibilities

| Document | Responsibility |
|---|---|
| [Project README](../../README.en.md) | Features, quick entry points and agent maintenance |
| [Contributing](../../CONTRIBUTING.en.md) | Branches, commits, PRs, review and contribution scope |
| [AGENTS](../../AGENTS.md) | Required repository workflow |
| [Getting started](GETTING_STARTED.en.md) | Prerequisites, first builds and environment troubleshooting |
| [Agent guide](AGENT_GUIDE.en.md) | Requirement intake, ownership, implementation, validation and handoff |
| [Requirements](REQUESTS.en.md) | Templates, prompts and acceptance examples |
| [Development](DEVELOPMENT.en.md) | Source map and feature development paths |
| [Testing](TESTING.en.md) | Commands, test selection, evidence levels and acceptance records |
| [Architecture](ARCHITECTURE.md) | Module ownership, lifecycle, threads, resources and configuration |
| [SurfXR](PROTOCOL.md) | Negotiation, wire extensions and endpoint contracts |
| [Timestamped poses](FRAME_POSE_TIME.md) | Clock calibration, prediction and returned-image pose association |
| [Tools](TOOLS.md) | Authoritative build, deployment, shader and packaging owners |
| [Build and launch entries](ONE_CLICK.md) | Root scripts and build/deployment distinctions |
| [WSL builds](WSL_BUILD.md) | Native Linux, ARM64/QEMU and target-SDK builds |
| [Release packaging](RELEASE.md) | Verified packaging, installation, services and firewall |
| [Open-source preparation](OPEN_SOURCE.en.md) | Licensing, provenance, private data, publication and collaboration |
| [Validation](VALIDATION.md) | Evidence boundaries; not certification of the current commit |
| [Imported line baseline](UPSTREAM_LINE_BASELINE.json) | Exact paths, line counts and hashes of imported-file exceptions |

## Maintaining documentation

Put stable interfaces and invariants in architecture/protocol documents, procedures in tool/build/test guides, and user-visible behavior in the README or operating instructions. Navigation pages, prompts and PR templates should link to those owners rather than duplicate changing contracts.

The README, contributing guide and Chinese maintenance guides have `.en.md` English counterparts with links in both directions. Update both editions when changing commands, defaults, maintenance rules or release procedures. Existing English architecture, protocol and build contracts remain shared sources.

State whether paths are relative to the repository root or `surfxr/`. Identify shell, working directory, prerequisites and side effects. Do not use maintainer usernames, addresses or private SDK locations as public defaults. Check script signatures, CMake/Cargo test registration, installation assets and packaging when updating related instructions.

Keep maintained documents within 500 lines where practical and split by reader task. Historical paths, counts and hashes describe their original evidence only. Current generated output belongs under root `build/` and final distributions under `release/`; new evidence must identify its actual source, environment and scope.
