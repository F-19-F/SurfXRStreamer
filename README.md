# SurfXR

SurfXR 从 **1.0.0** 开始独立版本管理，基于开源 **ALVR** 修改，保留其来源、版权及许可证。产品版本与底层 v21 协议版本分离；不会查询或安装 ALVR 更新。迁移与验证范围见 [SurfXR 迁移](surfxr/docs/SURFXR_MIGRATION.md)。

[English](README.en.md) | 简体中文

**SurfXR** 将 Windows PC 的桌面和 SteamVR 画面串流到 **Steam Frame**。PC 上运行 SurfXR 控制面板，头显上运行配套的 **SurfXR**；你可以在头显里操作桌面，也可以切换到 PC VR 游戏。使用完整发布包即可安装，不需要先配置编译环境。

本仓库将双端源码、构建入口和协议扩展放在一起维护。你可以手工贡献，也可以把需求交给自己的编程 agent；维护方式不依赖特定 agent 产品或维护者的历史聊天。

<a id="user-features"></a>

## 有哪些实用功能

| 功能 | 能帮你做什么、在哪里开启 |
|---|---|
| 安全区同步 | 将 Steam Frame 的有效安全区同步到 PC SteamVR，并匹配站立空间，减少两端重复设置。头显 `Devices → SAFETY BOUNDARY` 默认开启。 |
| 手柄关闭后完全断开、隐藏 | 关闭头显 `Devices → VR CONTROLLERS` 后，向 PC SteamVR 报告双手已断开并释放按键，避免手柄停在最后的位置；本地菜单和桌面操作仍可使用。实体手柄关机或断连，也会按 Frame 上报的连接状态同步。 |
| 桌面与 PC VR 随时切换 | 用同一个客户端操作 Windows 桌面、进入 SteamVR；串流中快速双击右手 `Menu` 即可切换，处理桌面事务后再回到 VR。 |
| 多屏桌面与虚拟键盘 | 将 PC 显示器摆成浮动面板，调整位置、大小和曲率；用手柄点击、右键、滚动及输入。键盘可固定在空间或附着到手柄，具体操作见[手柄与内置菜单](#controller-guide)。 |
| 注视区域串流 | 高分辨率区域跟随视线，外围保留低分辨率全景。PC“快捷设置 → 画面与性能 → SurfXR gaze streaming”和 Frame 连接界面 `Stream → Gaze streaming` 均需开启。Frame 端断开后修改，下次连接生效；PC 端改动按提示重启 SteamVR。不支持时使用全帧视频。 |
| PC 鼠标与 VR 光标优先级 | 在头显 `Devices → CURSOR PRIORITY` 选择 `VR HAND` 或 `PC MOUSE`，决定两边同时移动时谁优先，减少操作同一个桌面时的光标争抢。 |
| 可自定义的小 HUD | 在 `Diagnostics → OVERLAYS → MINI HUD` 开启 FPS、延迟估计等信息，调整内容、位置和大小，也可设为看向 HUD 时才显示。详见[客户端小 HUD](#mini-hud)。 |

安全区同步会更新 **PC SteamVR 当前的房间边界和站立原点**，保留坐姿校准及边界显示偏好。关闭同步只停止后续更新，**不会恢复 PC 原来的安全区**；需要恢复时，请在 PC SteamVR 重新设置。它同步的是已有边界，不会代替头显的安全区设置。

`VR CONTROLLERS` 是双手共用的转发开关。关闭后仍能用已连接的实体手柄操作 SurfXR 本地界面，重新打开即可恢复向 PC 转发；实体手柄的关机状态则以 Frame 检测结果为准。游戏自行绘制的手柄模型是否隐藏，由游戏处理。

<a id="user-guide"></a>

默认网络端口已与 ALVR 错开：串流使用 `19943–19952`，面板 API 使用 `18082`。升级需配套更新 PC 和 Frame；旧默认端口会自动迁移。详见[端口表与升级说明](surfxr/docs/NETWORK_PORTS.md)。

## 使用指南

### 从 PC 面板安装 Steam Frame

打开 **安装 → Steam Frame 客户端**。头显先开启 SSH，填写 `steamos@头显地址`，并指定包含 `release-manifest.json` 的已解压 SurfXR ARM64 包目录。选择直连或局域网，先预览安装计划，再点击安装/更新。在弹出的安装窗口完成主机密钥和密码提示，等待 Dashboard 显示安装并配对完成；安装完成不代表串流或佩戴验收已通过。

预览显示实际 SSH/SCP 命令、在 Frame 执行的 shell 内容和原生安装器 JSON 参数，不会连接或修改头显。远端临时目录以变量标明；实际执行时使用远端返回的路径。PC 更新应同时包含 Dashboard 与 `frame-installer` 目录，头显包目录仍为 `frame-client` 或你选择的其他包。

仍可使用根 `scripts/Install-Frame.ps1`，或发行包内的 `Install-SteamFrame.cmd` 安装。面板复用同一脚本、哈希检查和原生安装流程。

### 准备设备与安装包

- **PC**：Windows 10/11 x64、显卡驱动，以及能运行所选编码器的显卡。使用 PC VR 时还需要安装 Steam 和 SteamVR；这些驱动和程序不随 SurfXR 包提供。
- **头显**：Steam Frame，以及与 PC 版本配套、适用于头显系统的 SurfXR 客户端。安装客户端前，PC 需要能通过 SSH 登录头显；准备好实际地址、登录用户及密码或密钥。
- **网络**：PC 与头显之间需要可用的局域网或 6 GHz 直连链路。头显里选择的网络模式与 PC 地址应对应同一条链路。
- **文件**：选择同一版本的 PC Setup 安装包或 portable ZIP。PC 完整包内含 Frame 安装入口，也可使用配套的独立 `SURFXR-EXO-Frame` ZIP。保留整个文件夹和附带 DLL，不要只复制一个 EXE。GitHub 的源码 ZIP 需要自行编译，不能代替程序发布包。

### 首次安装与连接

1. **打开 PC 控制面板**。运行 Setup 后，从开始菜单打开 **SurfXR**；便携版先完整解压，再运行 `SurfXR Dashboard.exe`。程序可能驻留 Windows 通知区域，点击托盘图标可打开设置窗口。侧边栏可以切换中文或英文。
2. **完成 PC 设置**。进入“安装”（Installation），使用“添加防火墙规则”并按提示允许 Windows 管理员授权。桌面串流需要在同页“桌面串流服务”中安装或更新服务，再点击“启动服务”；进入 PC VR 前，确认 SurfXR 驱动已注册，未注册时使用“注册 SurfXR 驱动”。Setup 安装完成不代表服务已启动或头显已配对。
3. **安装头显客户端**。推荐在 PC Dashboard“安装 → Steam Frame 客户端”中预览计划并安装，计划结果会保留在页面，安装成功后自动将此 SSH 目标与当前 Dashboard 配对。也可运行 PC 包内的 `Install-SteamFrame.cmd`，或解压独立 Frame 包后运行 `scripts/Install-Frame.cmd`，选择安装/更新。安装器通过 SSH 连接头显，按提示确认设备并登录；默认的 `steamos@frame` 必须对应你的设备，主机名无法解析时会询问头显 IPv4 地址。安装成功后打开头显里的 **SurfXR**，此时串流仍处于空闲状态。自定义 SSH 用户或链路的参数见[安装入口说明](surfxr/docs/ONE_CLICK.md)。
4. **选择 PC 并信任设备**。保持 PC 控制面板运行。在 SurfXR 中选择 `WiFi`（局域网）或 `6 GHz link`，点击 `Find PCs`，选中自己的 PC/链路；也可在 `PC address` 填入该链路的 PC 地址。通过 Dashboard 安装并显示配对完成的设备无需再次信任；手动脚本安装时，发起连接后，在 PC“设备”页确认自己的 Frame 并点击“信任”。也可在准备连接时打开“待配对无线设备 → 一次性配对”，它会信任下一个新设备，并在该设备连接后自动关闭；已配对设备会保留。
5. **开始使用**。头显中选择 `Desktop` 显示并操作 PC 桌面，选择 `SteamVR` 进入 PC VR；PC 需要已安装 SteamVR 并具备可用的 SurfXR 驱动。按 `Disconnect` 停止串流。更换网络链路时先断开，再选择新链路和地址重新连接。

### 常用设置与日常操作

| 想调整什么 | 在哪里操作 |
|---|---|
| 清晰度、帧率、码率、桌面显示器数量 | PC“快捷设置 → 画面与性能”；头显 `Stream` 也可修改配套的串流设置。VR 分辨率/帧率修改后需手动重启 SteamVR，码率可在串流中调整。 |
| 桌面访问权限和桌面声音 | PC“SurfXR 功能 → 桌面串流”；桌面服务运行状态在“安装”页。 |
| 头显图像处理、设备和诊断选项 | SurfXR 的 `Image`、`Devices`、`Diagnostics`；小 HUD 的用法见下方[客户端小 HUD](#mini-hud)。 |
| 开机后准备桌面串流 | PC“快捷设置 → 启动与诊断 → 开机自动启动桌面串流服务”。它配置服务启动及登录后的托盘控制面板，需要管理员授权；立即使用仍需点击“启动服务”。 |
| Dashboard 登录自启动、日志级别 | PC“快捷设置 → 启动与诊断”。Dashboard 可独立在登录后启动到托盘；日志默认警告，调试级别显示详细信息，立即生效。 |
| 查看和断开桌面设备 | PC“设备 → 桌面与统一串流连接”显示设备、地址、桌面/VR/待机状态；“断开当前连接”保留配对，之后可从头显重新连接。不需要先启动 SteamVR。 |
| 隐藏或退出控制面板 | 有托盘图标时，关闭窗口只隐藏并继续串流；确认“完全退出”会先通知服务断开所有会话。此行为需要 Dashboard 和桌面服务一起更新；服务仍保持安装和运行。 |

<a id="controller-guide"></a>

### Steam Frame 手柄与内置菜单

以下操作针对 **Steam Frame 手柄**和已启动的 SurfXR 串流界面。其他设备的按键布局可能不同；PC VR 游戏中的普通按键由 SteamVR/游戏绑定决定。

**如何呼出 SurfXR 内置设置**：

1. 单击并松开**右手 `Menu`**，等待约 0.35 秒，显示虚拟键盘。
2. 用手柄射线指向键盘顶部的 `SETTINGS`，扣动扳机，打开内置菜单；选择 `STREAM`、`IMAGE`、`DEVICES` 或 `DIAGNOSTICS` 页面。
3. 点击菜单右上方 `CLOSE` 收起菜单，或点 `KEYBOARD` 回到键盘。若键盘仍显示，再单击右手 `Menu` 收起后恢复游戏操作。桌面模式还可将射线移到屏幕下方紧邻的空白区，在出现的控制条中点击最右侧的 `SETTINGS` 圆形按钮，直接打开设置。

进入 SteamVR 时会在后台等待 PC 运行时就绪，同时继续播放当前桌面。换源不再主动显示冻结桌面；媒体会话交接到首帧之间仍可能短暂没有视频。首次连接、桌面/VR 切换和恢复时会在右下方靠近视线中心的位置显示小提示，切换完成后自动隐藏，原有画面继续实时播放。点击 `X` 或单击右手 `Menu` 只关闭本次提示，下次切换仍可显示。在 Dashboard 或串流内设置的 **Diagnostics → Overlays → Switching HUD** 中可持续关闭或重新开启，默认开启；它与性能 Mini HUD 相互独立。

**快捷键**：

| 按键 | 操作 |
|---|---|
| 右手 `Menu` 单击 | 开关虚拟键盘；设置菜单已打开时，返回键盘。单击在松开后等待约 350 ms 才生效，用于区分双击。 |
| 右手 `Menu` 快速双击 | 切换桌面 / SteamVR 模式，双击判定窗口为 350 ms。 |
| 左手 `View` | 传给 PC VR，具体作用由 SteamVR 手柄绑定决定；Touch 兼容映射下用作远端 SteamVR 菜单键。打开 SurfXR 设置请使用上面的 `SETTINGS` 入口。 |

**桌面、键盘与面板操作**：先用手柄射线对准目标。以下鼠标别名用于桌面模式或本地键盘/设置可见时，不代表 PC VR 游戏的按键布局。

| 想做什么 | 手柄操作 |
|---|---|
| 选择按钮、桌面左键 | 扣动任一手扳机，或按肩键（bumper）；右手 `A`、左手方向键 `↓` 也可选择。对桌面内容按住并移动可拖拽。 |
| 桌面右键 | 右手 `B`、左手方向键 `←`，或按下任一摇杆。 |
| 桌面滚动 | 射线对准桌面，摇杆上下推；此时不要握住面板。 |
| 移动、倾斜桌面或键盘/菜单 | 对准面板并握住握把（grip），移动或转动手柄；松开握把放下。桌面底部 `MOVE` 条也支持按住扳机拖动。 |
| 调整面板远近、大小 | 保持握把抓取：摇杆上下推调远近，左右推调大小。 |
| 调整桌面屏幕大小、曲率 | 先指向屏幕下方空白区唤出控制条，再操作屏幕上方的 `SIZE` 或 `CURVE`：按住扳机并横向拖动。 |

桌面下边缘四个圆形按钮从左到右为 `RESET`（重置该面板）、`KEYBOARD`（开关键盘）、`CLOSE`（隐藏该面板）、`SETTINGS`（设置）。隐藏的屏幕可从键盘顶部 `SCREEN 1`–`SCREEN 4` 重新显示；只有 PC 提供的显示器才有画面。

键盘底部 `IN SPACE`、`LEFT HAND`、`RIGHT HAND` 用于选择空间固定或附着到左/右手；附着后用另一只手操作。顶部 `IME` 切换 PC 输入法，`RESET` 重置面板布局，`EXIT` 结束串流。如果打开了 Steam Frame 系统菜单导致本地输入失去焦点，先收起系统菜单再操作 SurfXR。

### 遇到问题时

| 现象 | 先检查 |
|---|---|
| 找不到 PC 或一直连接中 | PC 控制面板是否运行、所选链路和地址是否正确、设备是否已信任、防火墙规则是否已添加。更换 PC 安装目录后，重新从新控制面板添加规则，再运行 `Find PCs`。 |
| 桌面没有画面 | PC“安装”页的桌面服务是否运行；查看头显状态卡的错误及建议，再选择 `Desktop` 重连。 |
| 桌面可用，但 SteamVR 黑屏或启动失败 | 查看 PC SteamVR 状态、SurfXR 驱动注册及头显错误提示；修正后再次选择 `SteamVR`，也可先继续使用 `Desktop`。确认 PC 与客户端来自配套版本。 |
| 卡顿或画面延迟大 | 先降低码率，确认链路稳定；再调整分辨率或帧率并重启 SteamVR。使用 HUD 观察变化，避免一次改变多个设置。 |
| 没有声音或应用收不到麦克风 | 检查 PC 的桌面声音/音频设备设置、Windows 和头显音量及静音状态。使用默认 Steam 麦克风路由时，在游戏或语音应用中选择 `Steam Streaming Microphone` 输入。 |

升级时使用完整配套包，重新运行 PC 安装和 Frame 安装/更新流程；不要混用新旧 DLL 或仅替换单个程序。Frame 卸载菜单中的普通卸载保留设置，“完全卸载”会删除设置、布局和日志，不能当作日常重连步骤。若问题仍存在，请附上实际运行的 PC/Frame 版本、复现步骤和错误信息，按下方[报告问题](#report-problems)的说明提交。

## 从这里开始

| 你的目标 | 入口 |
|---|---|
| 我只想安装并使用 | [使用指南](#user-guide) |
| 我想了解实用功能和手柄操作 | [功能介绍](#user-features)、[手柄按键与内置菜单](#controller-guide) |
| 我想提需求，让自己的 agent 改 | [怎样写需求、提示词与验收条件](surfxr/docs/REQUESTS.md) |
| 我是接手项目的 agent | [面向 Agent 的维护入口](#agent-guide) |
| 我第一次编译或参与贡献 | [首次开发](surfxr/docs/GETTING_STARTED.md)、[贡献流程](CONTRIBUTING.md) |
| 我要添加功能或定位代码 | [源码地图与功能扩展路径](surfxr/docs/DEVELOPMENT.md) |
| 我要判断修改是否可靠 | [测试与验收](surfxr/docs/TESTING.md) |
| 我要安装或打包 | [一键入口](surfxr/docs/ONE_CLICK.md)、[发布](surfxr/docs/RELEASE.md) |
| 我要公开仓库 | [首次开源准备](surfxr/docs/OPEN_SOURCE.md)、[来源声明](NOTICE.md) |

完整索引见[文档导航](surfxr/docs/README.md)或 [English documentation](surfxr/docs/README.en.md)。README、贡献与维护指南提供中英文版本，底层架构/工具合约使用英文；Issue、PR 和需求可以使用中文或英文。

## 能力与开发范围

- PC：SurfXR Dashboard、设备信任与设置、SteamVR 驱动、视频编码、桌面捕获/输入/音频及持久连接服务。
- Frame：SurfXR 原生 launcher、桌面/VR 切换、OpenXR/Vulkan 呈现、解码、追踪/触觉、音频/麦克风和本地设置 UI。
- SurfXR 扩展：注视区域图集、设备状态、边界/IPD、设置接受状态、桌面输入及流生命周期；两端一起维护。
- 开发与验证：Windows PC 构建、Linux/WSL 本地与 ARM64 交叉构建、CPU/隔离回环测试、显式 GPU fixture 和发布校验。

唯一网络后端是 SurfXR，产品版本为 1.0.0，底层协议修订为 21.0.0-dev14；20.14.1 的 VR 路径仍可显式选择。

PC 产品构建面向 Windows x64，Frame 发布面向目标系统的 ARM64。Linux x86_64 与 QEMU 可用于离线贡献；它们不证明头显运行兼容。具体功能、生命周期和当前 8-bit SDR 色彩合约见[架构](surfxr/docs/ARCHITECTURE.md)。

<a id="agent-guide"></a>

## 面向 Agent：怎样维护与添加功能

请先读取根 [AGENTS.md](AGENTS.md)，以及 [ARCHITECTURE.md](surfxr/docs/ARCHITECTURE.md)、[TOOLS.md](surfxr/docs/TOOLS.md)、[PROTOCOL.md](surfxr/docs/PROTOCOL.md)。再按 [Agent 维护指南](surfxr/docs/AGENT_GUIDE.md)执行完整工作流。

1. 检查工作树和基线提交，保留已有修改，确认本机环境和操作授权。
2. 把需求整理为可观察行为，说明拥有模块、责任、受影响接口及验证计划。
3. 按[开发指南](surfxr/docs/DEVELOPMENT.md)在现有 owner 内实现；SurfXR 扩展同步维护 PC/Frame，配置变更覆盖迁移。
4. 使用已有工具，产物放根 `build/`；保持 500 行限制、协议字节、接受序号、有界队列、帧关联和资源生命周期。
5. 按[测试指南](surfxr/docs/TESTING.md)提供实际证据，明确区分源码/构建、CPU/回环、离屏 GPU、实机与佩戴验收。
6. 更新文档，完成必要检查，选择性提交，并交付提交 ID、未验证项和人工验收步骤。

可直接给 agent 这段提示词：

```text
请在当前仓库实现需求：【填写场景、当前行为、期望行为和验收条件】。
先阅读 AGENTS.md 和 surfxr/docs/AGENT_GUIDE.md 要求的架构、工具、协议文档。
保留无关修改，先说明 owner、受影响接口和验证计划，再完成实现与必要检查。
遵守 DEVELOPMENT.md 与 TESTING.md，更新相关文档，提交连贯且验证过的改动。
本次授权本地编辑、构建和离线测试；部署、服务变更、设备连接与发布另行约定。
最后给出行为变化、测试证据、未覆盖项、人工验收步骤和提交 ID。
```

填写需求及修改授权范围的方法见[完整模板与示例](surfxr/docs/REQUESTS.md)。没有头显也能贡献，但应明确留下待设备验收的项目。使用 agent 的贡献与手写贡献采用同样的[审查标准](CONTRIBUTING.md)。

## 目录

- `surfxr/pc/surfxr-server`：完整的 SurfXR Cargo 工作区、PC 驱动源码、资源、打包工具及 OpenVR SDK 子模块。
- `surfxr/pc/desktop`、`pc/daemon`：Windows 桌面捕获、输入、编码、服务和持久连接编排。
- `surfxr/frame`：Steam Frame 客户端、原生启动器、OpenXR/Vulkan、硬件解码和本地 UI。
- `surfxr/shared/surfxr`：两端共用的 SurfXR 编码、流、认证、FEC 和扩展。
- `surfxr/tests`：离线与隔离回环测试。`surfxr/docs`：架构、维护和操作文档。

客户端程序、服务、配置和安装目录使用 `surfxr-*` 名称。内部 `surfxr` 命名空间和 `surfxr/*.hpp` 公开头文件是稳定的模块接口；唯一网络后端是 `surfxr`。PC 端仍名为 SurfXR。

<a id="mini-hud"></a>

## 客户端小 HUD

在 Dashboard 或串流内设置的 `Diagnostics` → `Overlays` 打开 `Mini HUD`，VR 串流时默认在右上角常显 FPS 和延迟。原来的 Performance HUD 保留独立开关。

- `HUD content`：分别选择 FPS、延迟、帧间隔和解码耗时；全部关闭则隐藏小 HUD。
- `HUD layout`：调整横向位置（左 0 / 右 100）、纵向位置（上 0 / 下 100）和大小（50%–200%）。
- `HUD gaze`：打开仅注视显示后，看向小 HUD 区域约 80 毫秒显示，移开约 250 毫秒隐藏；区域标记可单独开关。需要有效眼动追踪，追踪失效时隐藏数据。关闭仅注视显示即可恢复常显。

这些设置只保存在客户端，重启后保留。FPS 统计新视频帧，帧间隔使用现有两秒窗口；`LAT UP->XR` 是追踪上传到 XR 提交的耗时，`DECODE` 是接收到解码输出的耗时，均不代表实际屏幕发光延迟。无有效数据时显示 `--`。

## 构建

PC 需要 Windows x64、Visual Studio 2022 C++ 工具、Windows SDK、CMake、Rust（工作区锁定工具链）和 Python 3。依赖下载和准备由 SurfXR 的 `cargo xtask prepare-deps` 管理。

```powershell
git submodule update --init --recursive
.\scripts\Build-PC.ps1 -PrepareDependencies
```

Frame 可在 WSL/Linux 本地构建或交叉编译 ARM64，详见 [WSL 构建](surfxr/docs/WSL_BUILD.md)。OpenVR SDK 由固定提交的 Git 子模块提供；先执行 `git submodule update --init --recursive`。OpenXR 头文件及系统库按文档准备。也支持显式选择 SSH 构建主机。`-BuildOnly` 仅编译和离线检查，不安装、不配对、不启动或停止用户服务。

编译、缓存、验证报告和打包暂存统一放在根目录 `build/`：PC 原生输出 `build/pc/native/Release`，PC 完整输出 `build/pc/surfxr_streamer_windows`，Cargo 缓存 `build/cargo`，WSL 客户端输出 `build/frame/<配置>`，SSH 构建报告 `build/reports/frame-ssh/validation.json`。便携 ZIP 和 Setup 安装包统一输出到 `release/`；`scripts/Build-PC.ps1 -Package -Setup -FrameArchive <客户端ZIP>` 同时生成两种 PC 包。详细用法见 [一键入口](surfxr/docs/ONE_CLICK.md)、[工具](surfxr/docs/TOOLS.md) 和 [发布](surfxr/docs/RELEASE.md)。

`scripts/Build-PC.ps1` 不自动完成 native CTest/Cargo 单元测试，具体命令见[测试指南](surfxr/docs/TESTING.md)。`scripts/Start-Frame.ps1 -NoStart` 仍可部署及配置服务，不等于 `-BuildOnly`。新贡献者优先使用本地 WSL 构建，只有明确要求时才在头显上编译。

<a id="report-problems"></a>

## 提需求、报告问题和验收

描述“在什么条件下，执行什么操作，现在发生什么，希望变成什么”，再列出正常、异常和回归验收项。版本请写实际运行的 PC/Frame 提交或发布包；源码已经改了不表示安装已更新。

可以使用 [功能需求模板](.github/ISSUE_TEMPLATE/feature_request.md)、[Bug 模板](.github/ISSUE_TEMPLATE/bug_report.md)和 [PR 模板](.github/pull_request_template.md)。不知道内部结构时使用[调查提示词](surfxr/docs/REQUESTS.md)，让 agent 先定位。日志、截图和附件先脱敏，不提交配对材料、密钥、用户配置或私人媒体。

验收时逐项记录预期与实际，并确认对应版本。构建通过不能证明画面、声音、输入或舒适性通过；历史 [VALIDATION.md](surfxr/docs/VALIDATION.md) 只支持它记录的提交。本次证据按[验收模板](surfxr/docs/TESTING.md)提供。

## 来源、许可与公开准备

项目自有代码和文档采用 [GNU GPL v3 (GPL-3.0-only)](LICENSE)，版权署名为 `Copyright (c) 2026 F-19-F`。分发须遵守 GPL v3（含对应源码要求），并保留版权与第三方许可声明。

SurfXR、OpenVR、shader 及其他第三方组件保留各自的许可证和署名，详见 [NOTICE.md](NOTICE.md)。GPL-3.0-only 不替代这些第三方条款；发布准备见[开源准备检查](surfxr/docs/OPEN_SOURCE.md)。

GitHub Actions 提供 **Build only**（仅构建）和 **Build and publish release**（自定义版本、选择 pre-release 并发布）；使用方法见[发布指南](surfxr/docs/RELEASE.md#github-actions)。
