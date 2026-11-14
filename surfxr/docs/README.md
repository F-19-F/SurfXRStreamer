# 文档导航

[English](README.en.md) | 简体中文

本仓库同时维护 Windows PC 端 **SurfXR** 与 Steam Frame 客户端 **SurfXR**。文档面向需求提出者、人工贡献者和任意能够阅读仓库的编程 agent，不依赖某个 agent 产品、私有插件或维护者机器上的固定路径。

## 按角色阅读

| 角色 | 推荐顺序 |
|---|---|
| 只想说明需求 | [项目 README](../../README.md) → [需求与提示词](REQUESTS.md) → [验收方法](TESTING.md) |
| 第一次开发 | [贡献指南](../../CONTRIBUTING.md) → [首次开发](GETTING_STARTED.md) → [开发指南](DEVELOPMENT.md) |
| 接手仓库的 agent | [AGENTS.md](../../AGENTS.md) → [架构](ARCHITECTURE.md) → [工具](TOOLS.md) → [SurfXR 合约](PROTOCOL.md) → [Agent 指南](AGENT_GUIDE.md) |
| 审查者 | 需求/PR → [开发指南](DEVELOPMENT.md) → [测试与验收](TESTING.md) → 受影响合约 |
| 发布维护者 | [开源准备](OPEN_SOURCE.md) → [WSL 构建](WSL_BUILD.md) → [发布](RELEASE.md) → 对应提交的验证报告 |

## 每份文档负责什么

| 文档 | 责任 |
|---|---|
| [README](../../README.md) | 产品介绍、快速入口、面向 agent 的维护入口 |
| [CONTRIBUTING](../../CONTRIBUTING.md) | 分支、提交、PR、审查与贡献边界 |
| [AGENTS](../../AGENTS.md) | 仓库必须遵守的维护规则 |
| [GETTING_STARTED](GETTING_STARTED.md) | 开发前提、首次构建、常见环境问题 |
| [AGENT_GUIDE](AGENT_GUIDE.md) | 接收需求、定位、实现、验证、交付和后续维护 |
| [REQUESTS](REQUESTS.md) | 面向人的需求模板、可复制提示词和验收例子 |
| [DEVELOPMENT](DEVELOPMENT.md) | 源码地图、功能扩展路径和跨模块检查表 |
| [TESTING](TESTING.md) | 检查命令、按改动选择测试、证据等级、验收记录 |
| [ARCHITECTURE](ARCHITECTURE.md) | 模块所有权、生命周期、线程/资源及配置合约 |
| [SurfXR](PROTOCOL.md) | 版本协商、网络扩展及 PC/Frame 共同约束 |
| [TOOLS](TOOLS.md) | 构建、部署、shader、打包等现有工具的唯一 owner |
| [ONE_CLICK](ONE_CLICK.md) | scripts/ 入口参数、计划、构建与部署的区别 |
| [WSL_BUILD](WSL_BUILD.md) | Linux 原生、ARM64/QEMU 与目标 SDK 构建 |
| [RELEASE](RELEASE.md) | 验证后打包、安装路径、服务和防火墙 |
| [OPEN_SOURCE](OPEN_SOURCE.md) | 首次公开前的许可、来源、历史、发布与社区准备 |
| [VALIDATION](VALIDATION.md) | 指定历史版本的证据，不是当前提交的合格证 |
| [UPSTREAM_LINE_BASELINE.json](UPSTREAM_LINE_BASELINE.json) | 导入大文件的精确路径、行数和哈希例外 |

## 维护文档的规则

稳定接口和不变量放入架构/协议文档，执行方式放入工具/构建/测试文档，用户可见行为放入 README 或相应操作说明。导航、提示词和 PR 模板链接这些来源，避免复制多个逐渐不一致的规范。

路径必须说明相对仓库根还是 `surfxr/`。命令标明 PowerShell 或 Bash、工作目录、前置条件和副作用；不要将维护者的用户名、IP、绝对目录或 SDK 快照路径当成通用默认值。

README、贡献指南和本目录的中文维护指南均提供 `.en.md` 英文版，两版顶部互相链接。修改命令、默认值、维护规则或发布流程时同步更新对应语言版本；原有英文架构/协议/构建合约保持单一来源。

修改参数时核对实际脚本签名；修改测试说明时核对 CMake/Cargo 注册；修改安装说明时核对打包与服务资产。维护新文档时也尽量控制在 500 行以内，按读者任务拆分，不复制整段源码。

历史验证里的旧路径、测试数量和哈希只解释当时的运行。当前输出统一在根 `build/`，发行物在 `release/`。新的验证记录必须对应实际源码、构建环境和执行范围。
