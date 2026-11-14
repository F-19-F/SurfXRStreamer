# 测试与验收

偶发 bug 的修复必须同时记录修复前后的正常路径耗时/响应性和故障复现结果；正常交互明显变慢即不满足验收，不能用“没有再报错”掩盖。相关场景须覆盖真实排队波动、回复丢失、调度间断和连续操作，不能只测理想时序。门控决策记录在 [GATE_REVIEW](GATE_REVIEW.md)。

[English](TESTING.en.md) | 简体中文

本文规定如何为一次改动提供证据。历史 [VALIDATION.md](VALIDATION.md) 只说明其记录版本；测试数量、硬件和结果不能直接搬到当前提交。所有命令从仓库根执行，另有说明除外。

## 证据层次

| 层次 | 能证明什么 | 不能证明什么 |
|---|---|---|
| 源码/文档检查 | 结构、链接、参数、静态一致性 | 能编译、能运行 |
| 构建 | 指定平台和工具链产物成功生成 | 功能正确、设备 ABI/运行时可用 |
| CPU/离线/隔离回环 | 模型、解析、状态机、格式、模拟生命周期 | 真实硬件解码、GPU 输出、佩戴体验 |
| 离屏 GPU/硬件编码 fixture | 该 GPU/驱动上的指定计算、编码或像素性质 | 真实 OpenXR 会话、设备端显示 |
| 真实头显运行 | 安装版本在目标设备上的解码、提交、切换/恢复 | 主观舒适度、其他硬件和长时间稳定性 |
| 佩戴验收 | 指定人员在指定版本/场景下的体验确认 | 未测模式、未测时长和以后的提交 |

按需求选择必要层次，不是每个 PR 都要做全部层次。只修改文档时检查文档即可；修改渲染时不能止步于“编译通过”。

## 按改动选择检查

| 改动 | 自动检查起点 | 需要补充的证据 |
|---|---|---|
| 文档/提示词/模板 | 结构、diff、相对链接、命令/路径与实现核对 | 无需无关运行时构建 |
| PC 设置/Dashboard | `surfxr_session`/`surfxr_dashboard` Rust tests、完整 PC build | 实际 UI；原生托盘测试需要交互桌面 |
| 本地偏好/设置 UI | `settings-store`、`input-preferences`、`local-image-quality`、`vr-ui-model` | 相关 viewer-status/launcher；实机布局和操作 |
| HUD/追踪/触觉/电量 | `mini-hud`、`performance-overlay`、`tracking-model`、`motion-sync`、`haptics-queue`、`device-battery` 按需选择 | 相应 runtime 采样与佩戴体验 |
| SurfXR/shared/扩展 | `surfxr-wire`、`surfxr-loopback`，PC 消费者测试，两端 build | 新旧能力协商及真实连接（如需求需要） |
| PC desktop/daemon | Windows CTest；现有输入/恢复 Python fixture | 真实捕获/输入/服务生命周期按需求选择 |
| 解码/颜色 | `h264-sequence`、`hevc-sequence`、`video-color-metadata`、`video-decoder`、`async-video-decoder` | 生产编码器 fixture 与目标 decoder |
| XR/shader/帧资源 | shader `--check`、`foveation-math`、`image-enhancement-math`、`rgba-upload`、`presentation-pacing`、`late-timewarp` | 相应离屏 GPU、真实 XR、佩戴验收 |
| 安装/启动/发布 | `launcher-native`、release Python/PowerShell fixture、匹配架构 build | 包哈希、实际安装/升级按授权验证 |

启动链路回归使用 `launcher-native` 的隔离部署/IPC fixture：保存 LAN 配置后明确请求 direct，检查实际子进程的 host、绑定接口和 active profile；同链路启动保持会话，跨链路活动请求不得修改配置或假报成功。实机版本对比必须记录实际链路与新画面帧率，不能仅凭启动参数或 XR 提交帧率判断。

`surfxr-loopback` 的切换准备夹具使用预留回环端点，检查等待 SteamVR 就绪不会取消当前桌面接收、取消后迟到的就绪无效、启动失败保留桌面，以及已准备的启动只消费一次。`mini-hud` 同时检查切换 HUD 的右下偏中心几何、完成后隐藏、后续切换再次显示、取消/反向切换、全局开关、单次菜单键隐藏；切换提示不再包含关闭按钮或扳机热区；偏好测试覆盖 v6→v7 迁移、独立开关持久化及两种设置面板的入口。它们不证明两个媒体代次之间零间隙；实机还需检查双向切换、取消、失败恢复和 HUD 位置/操作。

这些是现有 CTest 名称，不保证每个平台都注册全部项目。具体注册在 `surfxr/cmake/`；先 `ctest -N` 列出实际测试，再选择。匹配到零项不是通过。

`tracking-model` 覆盖慢速模式切换完成后立即按系统键、焦点重置、尾随 Menu 边沿，以及稳定释放后重新双击；无有效输入的时间不能计入释放保护期。`system-input` 使用隔离本地 socket 检查桌面模式能读取系统键按下/释放，同时始终不请求接管。实机验收需分别在 VR→桌面、桌面→VR 的画面切换瞬间按系统键或 Menu，确认不误切回，并检查正常双击和头显系统菜单仍可用。

偶发切换问题可通过现有 `timing 1` / `timing 0` viewer 命令采集 `.timing.json`。固定 512 帧记录附带快捷键 active/down 位掩码（左手 bit 0、右手 bit 1）、控制器 profile 枚举、系统键快照及其采样时间、焦点、手势保护/重置序号/双击结果，以及输入时的请求/呈现模式。记录不含输入文字、设备标识或画面。`viewer-status` 检查这些字段的异步写出和有界大小；采集必须覆盖复现瞬间，历史通过的状态机测试不能替代该证据。

## 最低检查：每次提交

```powershell
python scripts/build/check_structure.py
git diff --check
git status --short
```

结构检查校验维护代码 500 行上限及导入大文件的字节基线，不验证 Markdown 链接。文档修改另外检查相对链接、锚点、代码路径、shell 类型及参数；引用的脚本以实际签名/`--help` 为准。纯文档检查结果不得记作产品运行验证。

## Windows PC：构建与 native CTest

前提见 [GETTING_STARTED](GETTING_STARTED.md)。首次依赖准备和完整构建：

```powershell
.\scripts\Build-PC.ps1 -PrepareDependencies
```

依赖已准备时使用 `./scripts/Build-PC.ps1`。scripts/ 入口构建产品目标，不会自动构建所有测试目标或运行 CTest。需要 native 回归时继续：

```powershell
cmake --build build/pc/native --config Release --parallel 2
ctest --test-dir build/pc/native -C Release -N
ctest --test-dir build/pc/native -C Release --output-on-failure
```

不要对移动过的 CMake cache 直接构建，重新使用已有 owner 配置。可选的离线 ABI 检查是在完整 payload 构建后运行：

```powershell
.\build\pc\surfxr_streamer_windows\surfxr-daemon.exe --self-test
```

该检查验证匹配模块/driver 导出；它不是服务安装、真实捕获或流验证。

PC 原生按键回归使用 `cmake --build build/pc/native --config Release --target surfxr-openvr-boolean-test surfxr-openvr-frame-input-test`，再运行 `ctest --test-dir build/pc/native -C Release --output-on-failure -R '^openvr-(boolean|frame)-input$'`。前者先复现共享姿态钩子截断 64 位按键句柄，再验证双手 40 个布尔组件（含方向键和 Steam 系统键）的按下/释放、proximity、时间偏移与未知 ABI 回退；后者验证 48 路原生映射与骨骼缓存。两者不连接真实 SteamVR。实机验证另外检查修复驱动的加载哈希、已验证转发目标日志、布尔更新错误是否消失，并由佩戴者测试方向键和 Take Over 后的 Steam 系统键；右手 Menu 不等于系统键。

## PC Rust 测试

在 `surfxr/pc/surfxr-server` 工作区中执行，使用锁定 Rust 工具链和 `.cargo/config.toml` 指定的根 `build/cargo`：

```powershell
Push-Location surfxr/pc/surfxr-server
try {
    cargo test -p surfxr_session --locked
    if ($LASTEXITCODE) { throw 'surfxr_session tests failed' }
    cargo test -p surfxr_dashboard --locked
    if ($LASTEXITCODE) { throw 'surfxr_dashboard tests failed' }
} finally {
    Pop-Location
}
```

选择受影响 crate，不把 `cargo test --workspace` 当作无条件通用命令；导入工作区含其他平台组件。记录 ignored/skipped 测试。托盘的 `native_startup_stays_hidden_until_explicit_activation` 默认 ignored，需要交互 Windows 桌面和 native renderer；单独在该工作区执行：

```powershell
cargo test -p surfxr_dashboard --locked native_startup_stays_hidden_until_explicit_activation -- --ignored --nocapture --test-threads=1
```

它会创建隔离原生 UI，但不启动 streaming owner、不修改启动注册；不能把它说成无 UI 的 CPU 测试。

单实例失败路径的离线回归使用 `python -B scripts/tests/pc/test_dashboard_startup.py`。它通过既有 `source_fixture.py` 提取生产启动/销毁方法，用假 Win32 API 检查托盘窗口或图标创建失败时保留实例锁、mutex 创建失败时拒绝启动、重复启动的激活策略，以及完全退出后的重新进入；不创建真实窗口、托盘、注册表或服务对象。生成的 Rust 夹具和结果保存在根 `build/reports/dashboard-startup-test/`。

## Python/PowerShell fixture 的临时产物

现有部分测试使用系统临时目录。Windows 同一进程内可先把临时目录定向到根 `build/tmp`，完成后恢复环境；`-B` 避免源码树生成 Python 字节码：

```powershell
$taskTemp = Join-Path (Get-Location).Path 'build/tmp'
$null = New-Item -ItemType Directory -Path $taskTemp -Force
$taskOldTemp = $env:TEMP
$taskOldTmp = $env:TMP
try {
    $env:TEMP = $taskTemp
    $env:TMP = $taskTemp
    python -B -m unittest discover -s scripts/build -p 'test_release_*.py' -v
    if ($LASTEXITCODE) { throw 'Release fixture tests failed' }
    # 按改动选择下面的既有 fixture；输入/恢复测试需要 Windows C++ 工具。
    python -B scripts/tests/pc/test_desktop_input.py
    if ($LASTEXITCODE) { throw 'Desktop input fixture failed' }
    python -B scripts/tests/pc/test_desktop_recovery.py
    if ($LASTEXITCODE) { throw 'Desktop recovery fixture failed' }
    & ./scripts/launch/test_frame_release.ps1
} finally {
    $env:TEMP = $taskOldTemp
    $env:TMP = $taskOldTmp
}
```

输入/恢复 fixture 使用生产源码和 fake 系统/捕获/编码 owner；release fixture 使用隔离包及假 SSH/安装命令。它们不表示真实服务或目标设备安装通过。无需每次修改文档都跑这些测试。

## Frame：Linux/WSL、ARM64 与 shader

依赖和完整构建命令以 [WSL_BUILD.md](WSL_BUILD.md) 为准。Linux/WSL 本地路径使用 `build_viewer.py --local`，自动执行三个 shader `--check`、gamma 检查、构建和离线 CTest，并输出 `validation.json`。

同一终端先让临时输出留在根 build 下：

```bash
mkdir -p "$PWD/build/tmp"
export TMPDIR="$PWD/build/tmp"
export PYTHONDONTWRITEBYTECODE=1
```

已有 `linux-x64` 构建下，调试时可列出/选择测试：

```bash
ctest --test-dir build/frame/linux-x64 -N
ctest --test-dir build/frame/linux-x64 --output-on-failure -R '^(surfxr-wire|surfxr-loopback)$'
```

修改源文件后需先通过现有 builder 重建；不可测试旧 binary 后声称新代码通过。最终报告仍由 builder 生成，单次筛选 CTest 不替代完整发布验证。

`frame_pose_time` 的 `current-tracking` 测试使用 fake OpenXR 检查每次上传重新定位头显和双手、同一采样时间、Stage/失效/停止边界；`surfxr-wire` 检查时钟与精确字节，`surfxr-loopback` 通过隔离 socket pair 检查实际上传线程。PC 运行 `cargo test -p surfxr_server_core -p surfxr_server_openvr --lib --locked`，Windows 测试进程的 PATH 需包含匹配构建的 `build/pc/surfxr_streamer_windows`（OpenVR/libvpl DLL）。`frame_clock_calibration:2` 检查首回复即启动、不同时间原点、正负频率差、排队波动不改变映射/速率、宽误差区间、60 秒回复缺失和连续恢复；隔离上传测试丢弃三秒以上校准回复仍发送新鲜姿态，PC 将质量证据作为遥测而非准入门控；真机状态中的 `clock_*` 字段属于测量估计，不能当作精确同步证明。完整合约与佩戴步骤见 [FRAME_POSE_TIME.md](FRAME_POSE_TIME.md)。

shader 快速检查（需记录版本的 glslang）：

```bash
python3 -B scripts/shaders/compile_foveation_shaders.py --check
python3 -B scripts/shaders/compile_desktop_shaders.py --check
python3 -B scripts/shaders/compile_image_enhancement_shaders.py --check
python3 -B scripts/shaders/validate_foveation_gamma2.py
```

ARM64/QEMU 是 CPU 仿真。部署包必须使用目标系统 SDK/sysroot 构建；Ubuntu multiarch 输出不自动兼容头显。目标安装会核对动态依赖。原生 x86_64 报告不能打成 Frame 发布包。

## GPU 与真实设备

颜色/transfer fixture 的具体命令和目标在 [TOOLS.md](TOOLS.md)：`surfxr-nvenc-color-test --gpu-test`、`--software-test`、`--render-color-probe`，以及桌面编码与 Frame foveation GPU 检查。先构建相应目标，明确 synthetic 输出目录在根 `build/`。

同一 PC fixture 的 `--render-pose-probe` 直接调用生产 `FrameRender`、CSO 和 `PoseHistory`，比较游戏图层／悬浮窗口的像素与其关联姿态。它覆盖双眼、非对称 FOV、多轴旋转及换向，并保留旧目标姿态作为负对照。仅显式运行离屏 GPU 检查；不连接 SteamVR 或真实串流服务。无深度的平移误差与佩戴验收仍须单列。

CPU CTest 的通过不能代替这些显式 GPU 检查。软件 Vulkan、物理 GPU、PC 编码和 Frame 设备解码分别记录。不要把 fixture 喂入真实流服务，也不要为了获取测试画面捕获私人桌面。

真实设备检查前确认授权、目标 PC/Frame、匹配版本、安装清单和恢复方式。按需要逐项验证桌面、VR、双向切换、停止/启动、重连、音频/麦克风、输入/触觉、设置接受和重启保存。只有实际操作了的项目才记通过；应用重启不能代表无线掉线或 PC 重启恢复。

对佩戴者给出具体步骤和观察项，记录版本、模式、时长与反馈。无有效数据、stale pair、队列溢出和恢复计数应如实报告，不能因画面出现就写“零丢帧、零延迟”。

## 验收记录模板

报告文件存根 `build/reports/<任务>/` 或对应构建目录；PR 中保留脱敏摘要。模板可贴到 PR，不需要另建报告框架：

```text
需求/Issue：
源码提交；是否有未提交的运行代码：
构建执行环境；工具链/SDK；目标架构：
PC/Frame 实际部署版本（未部署则写未部署）：

验收项 | 证据层次 | 命令或操作 | 预期 | 实际 | 结论
A1     | CPU     | ……         | ……   | ……   | 通过/失败/未测/不适用

构建报告、输入/输出哈希、日志位置：
警告、ignored/skipped、失败与环境限制：
真实头显项目/观察时长：
佩戴者确认及日期（未获得则写待确认）：
运行状态变化；下一步验收动作：
```

最终验证时冻结源代码，再选择性提交。发布前让现有打包器检查报告与当前输入/产物是否匹配；源或 binary 有变化必须重新验证。不要在这一步编辑 success、哈希或测试输出。

运行时内部适配另遵守 [RUNTIME_ADAPTATION.md](RUNTIME_ADAPTATION.md)。`runtime-forwarding` 检查跨布局解析与安全拒绝，PC 的 `openvr-boolean-input` 检查完整 handle/状态/时间偏移，ARM64 的 `system-input` 执行真实转发指令并检查动态 hook 安装与恢复。三者均是离线证据；报告须另外列出实机日志与佩戴验收状态。

Streaming-menu regression uses `vr-ui-model` for Dashboard without exit, input release, unavailable/active Take Over and capture capability gating. Shared capture codec/state checks run in both existing Windows daemon-worker and Linux transport tests: malformed/truncated/trailing records, all-off, secondary-only resume, bounded coalescing and stale-layout ACK rejection. Isolated desktop loopback checks transmit and acknowledge negotiated capture requests. For headset acceptance, toggle each capture button and compare viewer status requested/accepted masks, verify remaining screens stay live and resume returns an image; all-off must keep the session and Dashboard usable. Confirm PC producer per-source metrics cease for disabled sources. Actual wearer feedback is required for placement, translation, Dashboard opening and Take Over usability.

Keyboard shortcut coverage: production model fixtures exercise unnamed default chords, footer attachment placement, absence of reset/named IME actions, add/edit/delete/cancel, local-only recording, empty-save rejection, key toggle/removal, ordered save events, replay, arrow/stick scrolling, list capacity and tracking-loss cancellation. Preferences tests cover v9 empty/full/custom list roundtrips, v1–v8 migration, malformed scan codes/counts and asynchronous persistence/permissions. Production queue tests retain neutral/down/reverse-up/neutral and atomic overflow all-up; candidate image/token/click tests remain active. Production Chinese raster previews include shortcut-bar, recorder and settings; Close Settings and Reset Position behavior have model coverage. Actual stick ergonomics, shortcut effects, candidate streaming/clicking and saved-list restoration require wearer acceptance, separately for desktop/VR.

Reset model coverage customizes two displays and keyboard geometry, then checks initial click does not mutate, Cancel is a no-op, confirmation restores only the selected panel, other display/keyboard remain identical, tracking loss cancels, underlying header actions are blocked, and confirmed full reset restores all geometry/space keyboard while keeping shortcuts. Existing layout persistence tests remain active. Production previews include simple display confirmation, complete-reset scope text and the bottom settings action. Actual headset aiming/confirmation and capture resume after full reset still require wearer acceptance.

Inline display reset checks additionally verify settings/keyboard visibility stays unchanged, the local control strip persists after hover timeout, both controllers are captured, clicks on other surfaces are ignored, and local Cancel/Confirm hit the same production strip geometry. The display-reset preview rasterizes the real bottom control surface rather than the keyboard.

Live switching regression: `tracking-model` checks rapid independent doubles, third-click singles, System cancellation/stable-release re-arming, logical mode cancellation and pose-independent physical buttons (presentation must not reset gestures); `mini-hud` checks the simplified notice. `surfxr-loopback` exercises negotiated desktop candidate images and rejection of old generations over the authenticated connection. The desktop input Python fixture checks candidate-only dispatch leaves ordinary mouse/key/all-up messages for their original owner. Rust `surfxr_sockets` checks increased/decreased pacing rates and debt reset; `surfxr_server_core` runs the actual video send worker and UDP callback writer with a changed bitrate. Headset acceptance remains: change VR bitrate repeatedly up/down, check new-picture cadence and recovery counters, immediately press Menu after switching, and type/select candidates in both desktop and VR (including desktop captures all off).

Dashboard 安装与诊断回归：`surfxr_dashboard` 测试覆盖计划结果持续保留、失败不配对、唯一 SSH 回执校验、经生产会话管理器保存信任后确认、快捷设置日志控件、默认警告过滤，以及隔离命名管道的服务命令失败后复用和连接丢失后撤销。`surfxr_session` 检查旧日志设置补入 Warning 且保留原偏好。`test_frame_release.ps1` 以假 SSH/SCP 检查安装后才读取配对身份、Plan 不接触设备；根 PowerShell 参数必须同步。隔离管道只返回假结果，不访问 SCM、不触发 UAC。

安装器版本/预览回归：`surfxr_dashboard` 用生产入口检查真实命令预览、配对查询、直接链路 JSON 和 Windows PowerShell 模块隔离；拒绝把旧 `frame-client` 适配脚本作为 Dashboard 入口。`surfxr_xtask` 检查完整 `frame-installer` 复制且不改旧头显包。`test_frame_release.ps1` 在 PowerShell 5.1/7 下用假 SSH/SCP 对比执行命令与预览，调用打包布局中的真实根入口验证新参数、安装配对回执及入口加载失败的错误回传。`test_release_package.py` 检查 standalone ZIP 内命令 helper 的哈希。实际 SSH 密码提示、头显安装和配对仍需单独验收。

Windows CI 打包回归：在 PowerShell 7 和 Windows PowerShell 5.1 运行 `scripts/package/test_portable_windows.ps1 -CompileSetup`，使用隔离假 payload 和真实 NSIS 编译器，只编译、不执行安装器。fixture 禁用 Archive cmdlet，检查 ZIP 生成/覆盖/解压、路径越界拒绝、适配脚本哈希和私有文件排除。`test_prepare_cargo_about.py` 检查离线缓存复用、损坏缓存修复、下载/可执行文件校验失败拒绝及只提取指定文件；`cargo xtask check-licenses` 对真实依赖图生成 `build/reports/dependencies.html`。CI 的 Rust 回归使用 `--release --locked`；本地检查通过不代表已测得 GitHub runner 缓存命中率或总耗时。

`scripts/package/test_portable_windows.ps1` 在 `build/tests` 内运行生产 PC 打包器，检查四个适配脚本及清单哈希、保留旧 Frame 包、排除用户设置和额外脚本，以及缺少 helper 时拒绝打包。夹具程序不能运行，生成的测试 ZIP 只在隔离目录内，测试不覆盖或部署真实发行包。

Dashboard 桌面会话回归：`surfxr_dashboard` 覆盖无 SteamVR 时设备行显示桌面连接、IP 与断开按钮，消失后恢复 SteamVR 原状态；broker 覆盖桌面/VR/待机、消费前拒绝通知订阅、IP 匹配、重复订阅拒绝、按票据断开、所有会话退出通知、通知连接关闭清理和重新连接。`surfxr-desktop-offline` 的 `dashboard_watch_checks.cpp` 使用预留随机回环端点执行原生订阅与撤销，检查旧 Dashboard 关闭未知操作码以及 EOF 不等于主动撤销，记录无消息时轮询开销。`daemon-worker-ipc` 和 `desktop-video-lifecycle` 验证既有资源退出路径。离线 UI 捕获第 11 页包含虚构桌面设备，不连接真实服务。

实机另验：更新同一 PC 包的 Dashboard 与桌面服务后，在 SteamVR 未启动时连接桌面，检查设备/IP/模式；切换 VR、回到桌面、停流待机和头显主动断开时检查状态。点击“断开当前连接”后应停止画面/声音/输入并保留配对，可重新连接；确认完全退出应在 SteamVR 关闭等待前断开，关闭窗口到托盘则继续串流。分别测量请求到连接断开的时延和正常模式切换耗时；离线通知测试不代表捕获、音频、输入释放或佩戴体验已经验收。

实机验收另行记录：安装计划可滚动复制；完整驱动列表默认折叠，换一个安装目录启动 Dashboard 时正确识别注册路径；快捷设置中的登录自启动与服务启动选项；首次服务操作批准一次 UAC 后连续安装/启动/停止/更新、取消 UAC 和退出后的重新授权；Frame SSH 登录/主机密钥提示可见，安装完成即出现在已配对设备中。日志级别切换应立即生效且不重启 SteamVR，性能页仍刷新。构建/CPU 测试不能代替这些交互和设备验证。

安装 PowerShell fixture 覆盖双栈 SSH 默认返回头显侧 IPv6 scope、身份查询显式 `-4` 才返回 IPv4、预览与执行参数一致，以及非法 hostname/地址不发出配对回执。打包入口失败回归检查安装已成功但配对失败时的准确提示与单条错误。真实只读 SSH 身份查询与真实安装/配对验收分别记录。

`surfxr-nvenc-color-test --bitrate-probe` 是显式离屏 GPU 检查，调用生产 HEVC/NVENC 编码器，以合成高细节移动画面覆盖 100→200→300→400→100→300 Mbps 动态调节，再用静态画面对照未开启 filler 时不会凑满目标码率。按配置帧率换算每帧输出预算，不代表实时传输吞吐量或头显验收。不会捕获桌面、更改用户设置或发送网络数据。

显式 `surfxr-nvenc-color-test --quality-probe` 复用生产 HEVC 编码器，比较 P1/P3/P5、单遍/四分之一/全分辨率两遍、200/300 Mbps。固定 Spatial AQ，使用相同 3840×2160 灰度平移细节与变化纹理序列，软件解码后统计亮度 PSNR/细节 MSE，并记录 Transmit 平均/p95 耗时；验证逐帧源关联与零额外帧缓冲。灰度合成误差指标不等于主观画质，未节流的离屏耗时不等于串流端到端延迟，不据此修改所有 GPU 的默认预设。

Dashboard 的 `typed_bitrate_commits_on_enter_and_focus_loss` 驱动真实 egui 数字框，验证输入300后回车、输入450后点击其它区域均经生产 SessionManager 提交，且重建设置页保留值。实机同时核对显示与 session.json，避免把只更新UI缓存当成保存成功。

Dashboard 编码设置回归覆盖中英文快捷设置中的 SurfXR 默认/自定义、多遍编码三个明确选项、全分辨率两遍选择后只修改原 multi_pass 字段、重建页面保留选择、默认按钮恢复单遍组合，以及打开页面和切换语言不写配置。高级设置不重复展示已移至快捷设置的 Multi pass；实际安装界面的选择与保存值另行核对，不代表头显画质验收。
