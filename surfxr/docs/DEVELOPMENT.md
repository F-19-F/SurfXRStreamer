# 开发与功能扩展指南

[English](DEVELOPMENT.en.md) | 简体中文

兼容性维护必须遵守[运行时动态适配规范](RUNTIME_ADAPTATION.md)：优先公开接口，禁止产品版本、私有偏移或特定指令 pattern 补丁；必要的动态解析须在维护报告中明确标注、说明验证和失败边界。

本文是源码导航与变更检查表。开始编辑前必须阅读[架构](ARCHITECTURE.md)、[工具 owner](TOOLS.md)和[SurfXR 合约](PROTOCOL.md)。以下源码路径除特别说明外均相对 `surfxr/`；测试命令见 [TESTING.md](TESTING.md)。

## 模块与责任

| 模块 | 拥有的责任 | 主要相邻接口 |
|---|---|---|
| `pc/surfxr-server/surfxr/dashboard` | PC UI、信任、设置提交、托盘与入口 | session、服务/设备编排 |
| `pc/surfxr-server/surfxr/session` | PC 会话设置 schema、默认值和迁移 | Dashboard、server_core、Frame 同步值 |
| `pc/surfxr-server/surfxr/server_core` | PC SurfXR 会话、网络与流编排 | 数据包、driver、Dashboard |
| `pc/surfxr-server/surfxr/server_openvr` | SteamVR 驱动、纹理、编码与 worker | OpenVR、共享纹理、私有 GPU IPC |
| `pc/daemon` | SYSTEM 服务、设备准入、backend 切换、worker lease | Dashboard、desktop module、VR worker |
| `pc/desktop` | 桌面捕获、输入仲裁、音频、编码和媒体交付 | Windows API、daemon、认证桌面协议 |
| `shared/surfxr` | wire codec、认证、流、FEC 与 SurfXR 扩展记录 | PC 与 Frame 双端 |
| `shared/protocol` | 内部几何、语义输入、客户端命令 | native 模块；不是第二个网络后端 |
| `shared/platform` | 真正共用的平台辅助能力 | status 文件、网络绑定等 |
| `frame/surfxr` | SurfXR 协商、会话、控制与媒体接收 | session callback、shared wire、tracking |
| `frame/session` | callback/options 公共界面 | 网络会话与 app 装配 |
| `frame/app` | viewer 装配、receiver worker、状态与命令 | launcher、session、media、XR |
| `frame/tracking` | Stage、姿态身份、预测、元数据配对、电量、触觉 | runtime、SurfXR、渲染 |
| `frame/media` | 有界异步解码、音频/麦克风、帧资源 | 接收、decoder lease、XR 采样 |
| `frame/xr` | OpenXR/Vulkan、呈现、timewarp、重建、fence | runtime、media、tracking、UI |
| `frame/ui` | 本地 UI 模型、指针/键盘、HUD | XR 绘制、偏好与输入 |
| `frame/preferences` | 本地配置格式、迁移与远端设置缓存 | Dashboard、launcher、viewer |
| `frame/dashboard` | 头显上的设置/状态 UI | launcher 与相同偏好/设置接口 |
| `frame/input` | 系统输入桥接及本地运行时驱动 | runtime 与显式输入捕获 |
| `frame/launcher/native` | 原生安装、服务、连接和生命周期 | 安装资产、viewer IPC、配置 |
| `scripts/launch` | Windows 侧操作编排和远端 launcher RPC | 根入口、构建报告、安装包 |
| `scripts/build`、`cmake` | 源清单、构建、结构检查和打包 | 构建输入和验证产物 |

各子系统的私有头留在子系统内。公共 `surfxr/*.hpp` 的根由 [public_includes.cmake](../cmake/public_includes.cmake) 声明；不要添加一个绕开 owner 的平铺公共目录。

## 先沿数据流定位

VR 的主线是 SteamVR → PC driver/encoder → SurfXR 会话 → Frame receiver → 解码 → 元数据/眼睛配对 → Vulkan/OpenXR 呈现。追踪和反馈由 Frame 反向送回 PC；时间戳还用于身份关联，不能随意当作同一时钟上的测量值。

桌面的主线是 Windows capture/input/audio → desktop module → daemon 管理的连接 → Frame 会话/解码/UI。桌面与 VR 切换由持久连接和媒体 generation 管理；不能只替换显示画面而留下旧流状态。

设置有两类：PC 权威值通过请求、回执和接受序号同步；Frame 本地偏好由本地格式和 IPC 保存/应用。找准权威 owner 后再设计 UI，避免在界面上创建一个与实际运行状态脱节的开关。

## 添加 Frame 本地设置或 UI 功能

适合：本地 HUD、布局、图像处理、输入捕获等不改变 PC 会话的功能。

1. 明确默认值、范围、有效模式、失效/隐藏条件及是否持久化。纯临时导航状态不必存入配置。
2. 在 `frame/preferences` 使用对应设置 owner。需要新字段时显式处理格式版本和历史值；保留旧字段顺序、严格验证与保留槽位。
3. 检查 `frame/app/include/surfxr/viewer_commands.hpp`、`viewer_status.hpp` 和 `frame/launcher/native` 的传递路径。命令已入队不代表已应用，沿用接受序号规则。
4. 根据功能同步维护 `frame/dashboard` 与 `frame/ui` 的串流内入口。相同设置应使用相同 ID、范围和默认值。
5. 行为放在拥有它的模块。例如 UI 决定指针交互，XR 绘制它；HUD 使用现有 telemetry，不能新建计时采集链。
6. 扩展已有偏好、UI、viewer-status、launcher 测试，覆盖旧格式、上下限、无效输入、重启保存及拒绝/过时命令。
7. 更新架构契约和用户操作说明。视觉布局或舒适性由实机/佩戴验收补齐。

参考路径：`frame/preferences/input_preferences.cpp`、`frame/ui/mini_hud.cpp`、`tests/frame/preferences/input_preferences_test.cpp`、`tests/frame/ui/mini_hud_test.cpp`。它们是现有完整路径示例，不是要求把新功能塞入这些文件。

## 添加 PC 权威设置

适合：由 PC 决定的 FPS、编码参数、桌面策略等。

1. 从 `pc/surfxr-server/surfxr/session` 定义 schema、默认值、持久化和历史配置行为。不要仅修改 Dashboard 文案。
2. 明确即时生效还是需要显式重启；UI 应显示真实接受状态。FPS/质量档等现有重启要求不能被“本地点击成功”抹去。
3. 沿 Dashboard、PC 实际消费者、Frame 同步 request/snapshot 追踪完整链路。若当前 wire 不容纳新值，先设计有协商的双端扩展。
4. Frame 的缓存和初始值不能主动冒充用户编辑，也不能覆盖较新的 PC 回执。首次默认值的一致性用实际 PC 默认值与 Frame 初始化器的测试检查。
5. 涉及实时输入/编码线程时，经现有 worker 发布快照或使用现有同步机制；不要在热路径进行设置网络调用。
6. 验证旧配置加载、无字段、冲突设置、乱序回执、拒绝、重连及重启后行为。需要时同步 native module ABI 与调用者。

参考 `frame/preferences/include/surfxr/remote_settings.hpp`、`tests/frame/surfxr/settings_sync_checks.cpp` 与 `pc/surfxr-server/surfxr/session/src/tests.rs`。当前 pointer priority 的具体请求布局在 [PROTOCOL.md](PROTOCOL.md)，不要从 UI 值类型自行推导 wire 格式。

## 添加或修改 SurfXR 网络扩展

唯一客户端 backend 是 SurfXR，默认 `21.0.0-dev14`；显式 v20 路径也需保持既有合约。不要把添加能力变成另起一个 transport。

头显/手柄的 `frame_pose_time` 扩展参见[时间戳姿态合约](FRAME_POSE_TIME.md)：当前采样、时钟映射、SteamVR 预测和实际图像姿态必须一起维护。`server_core/connection` 已按协商、音频、控制、会话生命周期拆分；OpenVR 的事件、设置和图像姿态分别归现有模块所有。

先写出消息/能力设计：

| 必须说明 | 检查内容 |
|---|---|
| 协商与降级 | 谁声明能力，未声明时保持什么旧行为，何时明确拒绝 |
| 字节布局 | 字段宽度、顺序、端序、单位、合法范围、长度上界 |
| 时序 | 请求/响应、接受序号、generation、超时、重放和乱序 |
| 安全边界 | 认证会话、方向密钥、重放窗口，禁止绕过现有校验 |
| 两端消费者 | PC 编码/解析/实际应用，Frame 编码/解析/UI 状态 |
| 生命周期 | 断连、取消、模式切换和退出如何清理 |
| 兼容证据 | 原消息精确字节、旧能力路径、新能力路径、非法输入 |

先保留现有字节级 fixture，再扩展 `tests/frame/surfxr` 中的 wire/loopback 检查。回环端点必须在测试中预留，不能连真实串流服务。涉及 Rust/native relay 时同时测试实际中继，不要只让两个相同的新 parser 互相通过。

PC 与 Frame 的实现、测试和文档作为同一个可审查变更提交。同步升级策略写入 PR；协议兼容不能靠“用户大概同时更新”保证。

## 修改解码、渲染、颜色或预测

先区分修改对象：像素语义、色彩矩阵/范围、transfer、空间采样、姿态预测还是呈现节奏。它们不是可以互换的优化手段。

- 每帧的两只眼、远端时间戳、tracking ledger 与 Stage epoch 必须关联一致；reset 不能留下旧关联。
- decoder allocation 在 GPU 读取期间保持不可变且被持有；上传完成不等于重建阶段也已完成。
- 队列上界和恢复请求必须保留；不能用无限缓存掩盖吞吐不足。
- shutdown 先退休 GPU 工作再释放 decoder/device；异常路径也应满足。
- 当前颜色合约是 8-bit SDR，RGB sRGB；matrix/range 与 transfer 分开处理。SPS/decoder fallback、CPU RGBA 和 GPU NV12 保持一致，详情以架构为准。
- shader 修改通过 `scripts/shaders/compile_*_shaders.py` 和 `embed_spirv.py` 生成，并用 `--check` 校验。PC FrameRender CSO 的既有 `fxc` 流程见 [TOOLS](TOOLS.md)。
- 现有时间戳与 telemetry 的定义应保留；不要把推算时间或 XR submit 宣称为 photon latency。

验证顺序通常是数学/元数据/生命周期 CPU 测试 → 相关 native 构建 → 显式 offscreen GPU fixture → 实机设备解码/呈现 → 佩戴验收。记录 GPU、驱动、软件 Vulkan 或物理设备及输入素材。不要在 live stream 注入 synthetic fixture。

## 修改桌面输入或交互

`frame/ui` 拥有指针稳定、点击/拖拽识别，`frame/xr` 绘制指针；`pc/desktop` 拥有 PC/VR 物理指针仲裁与实际输入。先确定问题属于哪个阶段。

检查 press/release/all-up、焦点失效、另一只手、来源切换、显示器切换、追踪过期、断连及退出。不能把一帧坐标跳变当成拖拽，也不能把被拒绝的移动缓存起来稍后回放。释放已按下按键的路径不能被所有权变更挡住。

使用现有 `tests/pc/test_desktop_input.py`、`test_desktop_recovery.py` 和 `tests/frame/ui` 模型；原生输入测试使用 fake 系统调用。真实桌面输入和舒适度检查另行约定，报告中区分“模型符合规则”与“用户觉得好用”。

## 修改启动、安装或包

使用scripts/ 入口 → `scripts/launch` → `frame_remote.py` / native launcher 的已有路径。转发脚本使用显式绑定参数，签名要同步。PC 构建由 `scripts/Build-PC.ps1` 与 `cargo xtask` 负责，不另写 launcher。

必须核对：

- `-BuildOnly` 无安装、配对、启停副作用；`-Plan` 与真实分支相符。
- 已安装 Frame 服务的 ExecStart 是 `%h/.local/share/surfxr-client/launcher/surfxr-client`。
- 源码路径、构建暂存、发布 ZIP 和实际安装目录相互独立。
- 安装资产被嵌入/打包且可解析，目标动态依赖通过检查，release manifest 与哈希一致。
- 路径含空格、参数转义、缺失文件、损坏哈希、失败回滚、升级和保留用户数据均有适当验证。
- PC 服务使用成套匹配的 executable/module/driver；不要仅替换一个文件。
- 服务权限、Windows Run 注册、防火墙、driver 注册各有 owner，构建不能偷偷修改这些状态。

优先使用 `launcher-native`、release Python 测试及 `scripts/launch/test_frame_release.ps1`。NSIS 的真实安装 fixture 会修改安装注册/快捷方式，属于单独的安装检查，不混入普通离线测试。实际部署后按 [RELEASE](RELEASE.md) 核对安装清单及运行路径。

## 新依赖与上游维护

引入依赖前说明现有代码为何不够、构建/运行平台、版本来源、下载 owner、缓存位置和许可证位置。保留导入树的声明，避免把第三方包复制到 shared。首次公开前的授权与来源检查见 [OPEN_SOURCE](OPEN_SOURCE.md)。

升级 SurfXR/OpenVR 时单独说明来源 revision、保留的本地扩展、生成资源及双端兼容验证。`UPSTREAM_LINE_BASELINE.json` 是已批准导入的精确例外，不是任意放大文件的许可。提交前运行结构检查并审查所有 baseline 变化。
