# 第一次参与开发

[English](GETTING_STARTED.en.md) | 简体中文

从仓库根目录执行本文命令；Linux 命令在 WSL/Linux 终端执行。不要照抄历史验证记录中的机器路径。完整维护规则见 [AGENTS.md](../../AGENTS.md)，完整构建参数见 [TOOLS.md](TOOLS.md)。

## 先选择你需要的环境

| 工作 | 需要 | 不足以证明 |
|---|---|---|
| 阅读、文档、源码结构检查 | Git、Python 3.10+、文本编辑器 | 编译或运行正确 |
| PC 产品构建 | Windows x64、VS 2022 C++ x64 工具、Windows SDK、CMake、Rust、Python | 真实捕获、编码、头显渲染 |
| Frame 原生 CPU/回环验证 | WSL/Linux，具体依赖见 [WSL_BUILD](WSL_BUILD.md) | ARM64 ABI、硬件解码、佩戴体验 |
| Frame ARM64 CPU 交叉验证 | ARM64 工具链、multiarch 库、QEMU | 目标头显系统兼容 |
| Frame 可部署包 | 目标 OS SDK/sysroot、ARM64 构建和验证报告 | 已部署或实机通过 |
| 实机验收 | 匹配的 PC/Frame 部署、运行时、设备及操作授权 | 其他硬件或其他版本也通过 |

不需要头显即可改文档、检查模型/格式、运行离线测试并提交 PR。缺少的设备验证应明确列为待办。

## 获取与确认源码

从 [SurfXRStreamer](https://github.com/F-19-F/SurfXRStreamer) 克隆或 fork，克隆时使用 `--recurse-submodules`。OpenVR SDK 是固定提交的 Git 子模块，不需要维护者的兄弟仓库。已有 checkout 在拉取更新后初始化子模块：

```powershell
git submodule update --init --recursive
git status --short
git rev-parse HEAD
python --version
python scripts/build/check_structure.py
```

先阅读[架构](ARCHITECTURE.md)、[工具](TOOLS.md)、[SurfXR 合约](PROTOCOL.md)。开发输出放在此 checkout 的根 `build/`，不要向已安装版本覆盖文件。

## Windows PC 首次构建

安装 VS 2022 的 C++ x64 工具与 Windows SDK，并让 `cmake`、`cargo`、`rustup`、`python` 可用。Rust 版本由 `surfxr/pc/surfxr-server/rust-toolchain.toml` 锁定，进入 Cargo 工作区后由 rustup 选择；不要用修改锁定版本来绕过环境问题。

```powershell
.\scripts\Build-PC.ps1 -PrepareDependencies
```

首次运行会通过现有 `cargo xtask prepare-deps --platform windows` 下载/准备依赖，然后构建 native 模块及完整 streamer。后续构建可运行 `./scripts/Build-PC.ps1`。这些步骤可能需要网络，不安装或重启应用服务。

主要输出：

- `build/pc/native/Release`：原生桌面、模块和守护进程。
- `build/pc/surfxr_streamer_windows`：PC 完整构建暂存。
- `build/cargo`：Rust 编译缓存。

构建入口不自动替你执行完整 CTest 或 Rust 单元测试。按[测试指南](TESTING.md)补上本次改动需要的检查。只修改 native 模块时可使用既有 `scripts/build/build_surfxr_windows.ps1`；涉及 Dashboard/驱动或交付时仍需完整 PC 构建。

## Frame 首次构建

优先按 [WSL_BUILD.md](WSL_BUILD.md)准备 Ubuntu/Linux 依赖、OpenXR 头文件和记录版本的 glslang。先跑 x86_64 本地构建，再根据工作目标准备 ARM64 和目标 SDK。

本地构建显式使用 `build_viewer.py --local`。输出目录必须是根 `build/frame/` 下单层命名目录，例如 `linux-x64` 或 `target-arm64`。构建器执行 shader 检查、编译、CPU/隔离回环 CTest，并产生该目录下的 `validation.json`。

编译 shader 的临时目录也应放在根 `build/`。在 WSL/Linux 的同一终端中，先运行：

```bash
mkdir -p "$PWD/build/tmp"
export TMPDIR="$PWD/build/tmp"
export PYTHONDONTWRITEBYTECODE=1
```

然后执行 [WSL_BUILD](WSL_BUILD.md) 对应的完整构建命令。`--local` 编译只在 Linux/WSL 执行；Windows 可使用验证后的本地报告打包，见 [RELEASE](RELEASE.md)。

不要把 SSH 选项指向头显作为默认开发方式。若明确选择独立 SSH 构建主机，它需要自己的工具链；只有用户明确要求时才在头显上编译。

## 构建、安装和运行是不同操作

| 入口 | 效果 |
|---|---|
| `scripts/Build-PC.ps1` | 构建 PC；不安装/重启服务 |
| `build_viewer.py --local` | 本地 Frame 构建及离线验证；不连接头显 |
| `scripts/Start-Frame.ps1 -Plan` | 打印所选源代码编排计划 |
| `scripts/Start-Frame.ps1 -BuildOnly -FrameSsh <构建主机>` | 在指定 SSH 主机编译及离线验证；不部署 |
| `scripts/Start-Frame.ps1 -NoStart` | 源码路径仍可停止旧 viewer、安装并配置服务；仅不请求开始串流 |
| `scripts/Install-Frame.ps1 -ReleaseDirectory <解包目录> -Plan` | 检查并显示预编译安装计划 |
| 实际安装/启动命令 | 修改目标安装或运行状态，使用 [ONE_CLICK](ONE_CLICK.md)/[RELEASE](RELEASE.md) 并确认目标与授权 |

产品运行时由 PC Dashboard 信任设备，Frame Dashboard 选择桌面或 SteamVR。客户端安装根与构建目录分离；安装不等于已配对，配对也不等于已验收。

## 常见阻塞怎么处理

| 现象 | 先检查 | 避免 |
|---|---|---|
| VS/Windows SDK 找不到 | VS 2022 C++ x64 组件、脚本错误和 CMake 选择结果 | 改用另一个自制编译入口 |
| Rust/libclang/FFmpeg 缺失 | 锁定工具链和 `-PrepareDependencies` 日志 | 手动复制不明版本 DLL |
| `--local requires Linux` | 是否在 WSL/Linux 终端中编译 | 去掉 `--local` 后意外连接默认 SSH 地址 |
| OpenXR 扩展定义缺失 | WSL 文档指定头文件和 include 路径 | 随意删除扩展代码 |
| shader `--check` 失败 | glslang 版本、生成命令和源码变化 | 直接编辑 SPIR-V 数组 |
| ARM64 CTest 无法启动子进程 | QEMU、binfmt、目标 loader 与 sysroot | 把未运行的测试记成通过 |
| 安装提示动态库缺失 | 目标系统 ABI，是否错误使用 Ubuntu 构建 | 在头显上随意替换系统库 |
| 构建通过但找不到设备 | 匹配的安装、Dashboard 信任、网卡/地址、防火墙规则 | 向真实服务注入测试包 |
| 打包拒绝报告 | 输入哈希、架构、报告路径及完整构建产物 | 修改报告的 success 或哈希字段 |

求助时提供提交 ID、命令、平台/工具版本、第一处有效错误和脱敏后的短日志；不要只提供最后一行“构建失败”。网络受限或缺少 SDK 时，记录环境阻塞并继续完成独立的文档/源码检查，不宣称构建已通过。
