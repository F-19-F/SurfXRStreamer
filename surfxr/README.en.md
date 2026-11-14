# SurfXR source modules

English | [简体中文](README.md)

This directory contains PC, Frame and shared source. See the [project README](../README.en.md) for features and build entry points, and the [documentation index](docs/README.en.md) for the complete reading map.

- Contributors: [contributing](../CONTRIBUTING.en.md) and [getting started](docs/GETTING_STARTED.en.md).
- Coding agents: [repository rules](../AGENTS.md) and [agent guide](docs/AGENT_GUIDE.en.md).
- Requirement authors: [requirements and prompts](docs/REQUESTS.en.md) and [testing](docs/TESTING.en.md).
- Maintainers: [development](docs/DEVELOPMENT.en.md), [architecture](docs/ARCHITECTURE.md), [tools](docs/TOOLS.md) and [SURFXR contracts](docs/PROTOCOL.md).

`pc/` owns Windows PC implementation, `frame/` owns headset implementation, and `shared/` contains genuinely shared code. Keep generated output in the parent repository's root `build/`; do not add build directories inside this source tree.
