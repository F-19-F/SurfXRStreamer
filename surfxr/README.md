# SurfXR source modules

SurfXR starts its independent product version at **1.0.0** and is based on **ALVR**, with original attribution and licenses retained. Product and wire revisions are separate; SurfXR never queries or installs ALVR updates.

[English](README.en.md) | 简体中文

本目录包含 PC、Frame 与共享源码。产品介绍和构建入口见[仓库 README](../README.md)，完整阅读路线见[文档导航](docs/README.md)。

- 贡献者：[贡献流程](../CONTRIBUTING.md)、[首次开发](docs/GETTING_STARTED.md)。
- 编程 agent：[仓库规则](../AGENTS.md)、[Agent 指南](docs/AGENT_GUIDE.md)。
- 提需求的人：[需求与提示词](docs/REQUESTS.md)、[测试与验收](docs/TESTING.md)。
- 维护者：[开发与扩展](docs/DEVELOPMENT.md)、[架构](docs/ARCHITECTURE.md)、[工具](docs/TOOLS.md)、[SURFXR 合约](docs/PROTOCOL.md)。

`pc/` 拥有 Windows PC 端；`frame/` 拥有头显客户端；`shared/` 只容纳真正共用的部分。构建产物放上一级仓库根的 `build/`，不要在此目录中新建构建树。
