# 项目脚本

所有命令从仓库根目录运行。产物、缓存、临时文件和验证报告写入根 `build/`，最终发行包写入 `release/`。

| 目录或入口 | 职责 |
| --- | --- |
| `Build-PC.ps1` | PC 完整构建；调用原有 native build 与 Cargo xtask |
| `Start-Frame.ps1` / `.cmd` | Frame 构建、部署和启动；`-Plan` 预览，`-BuildOnly` 只构建 |
| `Install-Frame.ps1` / `.cmd` | 使用已解压 ARM64 发行包安装、卸载；SSH 负责认证 |
| `Start-FrameSingle.ps1` / `.cmd` | 保留原有显式参数转发入口 |
| `build/` | Frame 本地/WSL 构建、唯一源码清单、结构检查和发行包工具 |
| `launch/` | 公共入口调用的 SSH、部署、安装实现 |
| `package/` | Cargo xtask 调用的 Windows portable/Setup 脚本与 NSIS 模板 |
| `shaders/`、`fonts/` | 原有 shader 数组与字体生成工具 |
| `tests/`、`transport/` | 隔离 PC 回归及生产源码 fixture 辅助工具 |
| `ci/` | GitHub runner 环境准备和校验后发布；复用现有构建与打包入口 |
| `diagnostics/` | 显式诊断辅助工具 |

常用命令：

```powershell
git submodule update --init --recursive
.\scripts\Build-PC.ps1
python scripts/build/check_structure.py
python -B -m unittest discover -s scripts/build -p 'test_*.py' -v
.\scripts\Install-Frame.ps1 -Action install -ReleaseDirectory <解压目录> -FrameSsh <用户@地址> -Link lan -Plan
```

Frame 推荐在 Linux/WSL 使用 `python3 scripts/build/build_viewer.py --local`；完整参数和依赖见 [WSL 构建](../surfxr/docs/WSL_BUILD.md)。安装与发布见 [发布指南](../surfxr/docs/RELEASE.md)。

源码树的旧根入口已移入这里；发行包仍在包根提供 `Install-Frame.ps1` / `.cmd`，并携带 `launch/` 辅助脚本。Cargo `build.rs` 编译钩子、导入平台集成资源及 native launcher 嵌入的运行时安装资源留在消费它们的模块中。
