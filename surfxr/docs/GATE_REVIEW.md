# 门控审查与正常体验约束

本清单按同一 owner、同一故障后果归并门控，不把每个 `if` 当成独立门控。审查范围为 SurfXR 自有 PC/Frame/shared 的时钟、切换、输入、串流、呈现、生命周期与发布入口，并沿 ALVR 派生 PC 姿态接收路径核对。第三方 FFmpeg/OpenXR/驱动内部全部条件分支不在此源码审查范围内；清单不是对整个依赖栈的无遗漏证明。

维护规则见 [AGENT_GUIDE](AGENT_GUIDE.md)：偶发故障不得以每次正常操作明显变慢为代价；不以理想网络假设增加严格门控；报告必须说明原因、方法、证据和代价。新增或调整门控须更新这里的必要性和实际失败后果。表中的“候选”是待验证决策，不代表已经删除或允许跳过正确性检查。

## 本次时钟修复的原因与方法

旧实现以时钟质量中止健康串流，扩大启动测量范围又拖慢正常 VR 进入。当前实现以首条有效回复启动，校准质量仅用于估计与诊断，不能中止健康串流。

替代方案是第一条有效回复即可建立时间原点，继续后台测量；后续仅在测量区间排除现有映射时才连续校正，不追随排队引起的区间中点移动。短期相位修正最多运行两秒，回复丢失时不会无限持续；其后沿已有基准速率运行。误差和回复年龄如实上报，不能触发串流退出、模式变化或额外显示等待。PC 同时接受这类姿态证据，通过 `frame_clock_calibration:2` 明确协商；四时间戳与 80 字节后缀不变。详细算法、兼容性与测试见 [FRAME_POSE_TIME](FRAME_POSE_TIME.md)。

这不承诺网络物理时延恒定或无测量时仍有完美同步。真正的断链、认证撤销、错误内存访问和失效资源不能用“少门控”掩盖。它承诺的是校准质量波动不再直接改变软件等待、断开健康串流或切换模式。

## 时钟与姿态

| ID | 位置 / 条件 | 必要性与正常使用代价 | 决策 / 删除边界 |
|---|---|---|---|
| C01 | [PoseClock](../frame/surfxr/pose_clock.hpp)：累计至少八个回复才启动 | 仅为估计置信度；每次正常启动都付等待成本 | **已删除**；第一条有效回复建立原点，revision 从 1 开始 |
| C02 | 同上：观测跨度至少 200 ms | 与 C01 重复延迟正常路径 | **已删除**；频率估计后台进行 |
| C03 | 同上：三秒预测区间必须小于 5 ms | 把尚未发生的网络情况变成启动条件；本次用户明确感知退化 | **已删除**，不再用更长等待治疗偶发失败 |
| C04 | 同上：15 秒校准获取超时抛异常 | 与真实连接/媒体生命周期重复；测量质量不等于连接死亡 | **已删除**；没有首个有效回复时不伪造时间原点 |
| C05 | 同上：三个自然秒没有校准回复就过期/拒发姿态 | 健康媒体可继续，而测量回复可能延后 | **已删除**；连续 holdover，年龄仅遥测 |
| C06 | 同上：误差大于 5 ms 抛异常 | 区间变宽不是时钟跳变；会直接误切桌面 | **已删除**；真实误差区间照常编码 |
| C07 | 同上：RTT 大于 100 ms 拒收测量 | 排队只应给出较宽约束；丢掉可能唯一回复无益 | **已删除**；保留时间戳顺序/处理时间物理一致性 |
| C08 | [PC calibration](../pc/surfxr-server/surfxr/server_core/src/frame_clock.rs)：revision≥8、误差≤5 ms、回复年龄≤3 s | PC 再次把测量质量变成姿态断供条件 | **已删除这些质量门控**；非零/不回退 revision、合法单位、非未来校准时间保留 |
| C09 | [PC PoseTime](../pc/surfxr-server/surfxr/server_core/src/frame_pose.rs)：映射时间只许领先 5 ms | 对不对称网络的初始估计过严；与独立误差限制重复 | **已调整**为与过期方向一致的 ±200 ms 物理合理性范围；单包异常不停止会话 |
| C10 | [配置协商](../frame/surfxr/wire.hpp)：能力版本匹配，第一条测量存在 | 不同进程时钟原点不能猜；旧 PC 会丢弃新 holdover 姿态 | **保留**；显式能力协商，不依赖产品版本/固定地址；不增加固定等待 |
| C11 | [PoseClock](../frame/surfxr/pose_clock.hpp)、PC pose：nonce、会话 generation、顺序、数值范围、有限有效姿态、Stage/sequence、真实样本年龄 200 ms | 防串会话、重放、非法运动、把旧姿态当新姿态；正常包不等待 | **保留**；只拒绝相应坏测量/姿态，不把不确定性当非法数据 |

## 输入与正常交互

| ID | 位置 / 条件 | 必要性与正常使用代价 | 决策 / 删除边界 |
|---|---|---|---|
| I01 | [shortcut_buttons](../frame/tracking/shortcut.cpp)：Frame 只接收右 Menu，System 独立取消手势 | 防止左 Steam/View 或通用绑定被当成切换键；无等待 | **保留**，这是输入身份修复 |
| I02 | 同上：30 ms 边沿消抖、30–500 ms 点击、350 ms 双击窗口 | 消抖与单/双击语义；单击键盘必须等待双击判定 | **保留语义，阈值可测量调优**；不能用额外全局锁定代替身份判断 |
| I03 | 同上：原切换/System 后持续释放 350 ms；presentation 到达再重置 | 固定静默会吞掉已释放后的新点击；图像到达与按钮身份无关 | **已删除额外静默和 presentation 重置**；逻辑模式变化消费旧手势，已稳定释放立即接收新点击；System/focus/profile 或失效输入后仍需观察 30 ms 稳定释放 |
| I04 | 同上：原快捷按钮要求 pose active，采样间断>100 ms 清 pending | 按钮有效性不依赖空间定位；旧半个手势与无效射线仍须清除 | **已删除快捷按钮的 pose 前置条件**；键盘命令独立于射线；保留按钮身份/有效性/焦点、采样间断取消以及指针/拖拽姿态检查；真实遮挡按键体验待佩戴验收 |
| I05 | [capture policy](../frame/input/capture_policy.cpp)、[input constants](../frame/input/include/surfxr/system_input.hpp)：250 ms 捕获租约、100 ms 状态新鲜度 | 进程挂掉后不能永久吞系统键；过期即释放捕获，不关闭串流 | **保留**；本地 IPC 规则，不使用网络时延驱动 |
| I06 | 同上：四击 Take Over 退出捕获 | 用户可靠夺回本机控制；仅显式手势触发 | **保留**；不应与模式双击共享计数 |
| I07 | [tracking upload](../frame/surfxr/upload.cpp)：失焦/UI 捕获时释放游戏输入 | 防止系统菜单中误操作游戏；头部新鲜姿态仍上传 | **保留输入隔离**，不可把失焦扩展成断流或停头部追踪 |

## 传输、身份与生命周期

| ID | 位置 / 条件 | 必要性与正常使用代价 | 决策 / 删除边界 |
|---|---|---|---|
| T01 | [desktop crypto](../shared/surfxr/include/surfxr/surfxr_desktop_crypto.hpp)、[media](../shared/surfxr/include/surfxr/surfxr_unified_media.hpp)：认证、cookie、序号/重放、保护模式锁定 | 防未授权控制、跨会话包和重复输入；不是质量门控 | **保留**；坏 UDP 包局部丢弃，不能关闭认证求流畅 |
| T02 | [unified records](../shared/surfxr/include/surfxr/surfxr_unified.hpp)：控制/包大小与类型边界 | 防越界、无限分配与错误解析；正常数据无固定等待 | **保留** |
| T03 | [unified receiver](../frame/surfxr/unified.cpp)：generation/来源切换隔离 | 旧源不能读走或发布新源图像 | **保留**；取消应及时，不能靠延长冷却期保证 |
| T04 | [unified](../shared/surfxr/include/surfxr/surfxr_unified.hpp)、[PC session](../pc/daemon/session.cpp)：15 秒可靠控制完全静默/EOF | 回收真正失联的控制和输入所有权；正常活动不计超时 | **保留有界失联处理**；不能用单个校准回复年龄代替连接活性 |
| T05 | [PC session](../pc/daemon/session.cpp)：Dashboard admission 租约撤销/失活 | 服务不能在授权 owner 撤销后继续接受控制 | **保留**；明确退出/断开通过每会话回环通知立即撤销，统一会话心跳失联仍保留 15 秒容错，不以缩短超时修复退出 |
| T06 | [backend](../pc/daemon/backend.cpp)：有界控制队列，背压等待/失败 | 防内存无界增长；满队列会影响响应 | **保留容量，局部化恢复候选**：区分可合并状态与不可丢边沿；不能一律扩大队列制造延迟 |
| T07 | [media lease](../pc/desktop/media_lease.hpp)：媒体 owner 租约与取消 | 防旧、新编码/捕获 owner 同时占资源；仅竞争时等待 | **保留**；优化释放交接，不能直接删互斥 |
| T08 | [session](../frame/surfxr/session.cpp)、[deadline](../frame/surfxr/session_deadline.hpp)：连接/握手/首帧建立期限、恢复预算 | 避免坏后端永久等待；正常数据到达即通过 | **保留真实建立失败边界，合并候选**：审查外层重复截止时间，按实际进展与取消处理；不能沿用为校准质量超时 |
| T09 | [startup monitor](../frame/surfxr/start_status.cpp)、[mode startup](../frame/surfxr/mode_startup.hpp)：真实 SteamVR ready/失败、启动安全期限 | 防尚未就绪就撤掉桌面；后台准备不阻塞 XR | **保留明确 ready/失败**；减少重复探测，不能新增固定睡眠 |
| T10 | [Modes](../frame/surfxr/include/surfxr/surfxr_modes.hpp)：匹配请求 serial 才完成准备 | 防止旧异步结果覆盖用户刚切回桌面的请求 | **保留**；非时间门控、正常路径无等待 |
| T11 | [fallback](../frame/surfxr/modes.cpp)：真实 VR 源失败而 unified 仍健康时回桌面 | 避免永久黑屏；若上游错误分类太宽则误切换 | **保留源失败恢复**；本次从源头移除校准质量异常，不删除整个恢复能力 |
| T12 | [launcher](../frame/launcher/native/controller.cpp)：`mode_deadline_` 赋值 40 s | 搜索仅有声明和赋值，没有消费路径；不是正在生效的门控 | **可清理的死状态**；删除不构成耗时优化，不能当作已消除 40 s 等待来汇报 |
| T13 | [lifecycle](../frame/launcher/native/lifecycle.cpp)：退出升级 SIGINT→SIGTERM→SIGKILL、有界等待 | 防死进程阻止卸载/重启；正常退出即结束 | **保留**；已授权 stop/install 才走此流程，不是每次串流切换强杀 |

## 图像、呈现与资源

| ID | 位置 / 条件 | 必要性与正常使用代价 | 决策 / 删除边界 |
|---|---|---|---|
| V01 | [session](../frame/surfxr/session.cpp)：损坏参考链后等 IDR、限制重复 IDR 请求 | 防用坏参考图持续花屏/请求风暴；丢包时有恢复成本 | **保留**；完整可解码图不应被额外丢弃 |
| V02 | [async decoder](../frame/media/async_decoder_private.hpp)：包/字节预算与 try-submit 背压 | 防无限积压和延迟；满时局部恢复 | **保留界限**；优化消费和旧工作淘汰，不加无界缓存 |
| V03 | [tracking ledger](../frame/surfxr/tracking_ledger.cpp)、[VR state](../frame/tracking/vr_client_state.cpp)：精确图像/姿态/眼/Stage 关联、1024 项历史 | 错配导致空间跳动、左右眼不一致；通常查找无等待 | **保留**；不能拿最新姿态给旧图换标签 |
| V04 | [decoder output](../frame/media/decoder_private.hpp)：200 ms 后跳过过时转换/发布 | 防耗时处理明显过时图像；不破坏参考解码 | **保留局部丢旧工作**；不是整条串流退出条件 |
| V05 | [VR playout](../frame/tracking/vr_client_state.cpp)：两帧 reserve，最多 25 ms，三槽队列 | 平滑到帧抖动但每帧有实际延迟成本 | **调优/移除候选**；须对比低抖动直连与丢包场景的重复帧和延迟，不能只凭“更稳”保留；本次未改 |
| V06 | [presentation pacer](../frame/xr/presentation_pacing.hpp)：仅将重复的帧在当前周期余量内等待 | 可能等到新图，减少重复；消耗当前周期余量，不改 predictedDisplayTime | **保留有界、可提前结束的等待，持续测量候选**；已有新图立即走，不能据网络 RTT 扩大周期 |
| V07 | [frame loop](../frame/xr/frame_loop.cpp)：OpenXR 生命周期/shouldRender、有效 stereo pair | 遵守运行时契约和有效图像要求 | **保留**；不能把暂时不可呈现升级为传输断开 |
| V08 | [foveation renderer](../frame/xr/foveation_renderer.cpp)：GPU fence/图像资源完成与 decoder lease | 防 GPU 使用中覆盖/释放资源和设备错误 | **保留正确性**；改流水线消除不必要同步，不能直接删 fence/lease |
| V09 | [desktop pacing](../pc/desktop/pacing.hpp)：按目标帧率/包计划等待 | 用户设置帧率与带宽调度所需；错误截止时间会卡流 | **保留正确的调度**；变更码率应更新调度，不能追加固定恢复等待 |

## 非运行时检查

构建/打包的源文件大小、哈希、匹配安装清单、shader 生成一致性和隔离测试属于开发正确性检查，不会增加玩家每次操作耗时，不能因“少门控”而删掉。反过来，文档修改不得要求无关运行时构建。验证工具 owner 见 [TOOLS](TOOLS.md)。

## 后续优先级与证据

时钟修复移除 C01–C08，调整 C09；本轮继续删除 I03/I04 中无必要的固定静默、呈现重置和快捷按钮姿态前置。V05/V06（播放 reserve/周期内等待）仍待验证。T06/T08 可按真实失败记录局部化恢复、合并重复超时；T12 是死状态清理。其余项目的保留依据是身份、安全、资源或实际协议语义，不是抽象的“多检查总更安全”。

I03/I04 原因与方法：旧状态机在切换或 System 后反复等待 350 ms，再要求释放消抖；新点击会延长等待，首张 VR 图像又重置一次。现改为按键生命周期取消：逻辑模式变更清 pending/second，只有已确认稳定释放的手保留 armed；System 按住时持续取消，失效/焦点/profile/采样中断后重新观察释放。已确认释放后的新点击无需额外锁定。30 ms 消抖、30–500 ms 点击长度与 350 ms 单/双击区分语义不变。Frame 只接受原始右 Menu，System/View 和通用 Menu 别名不能充当它。时钟 holdover 和串流失败处理保持上一轮修复。

离线证据：`tracking-model` 覆盖连续双击立即反向、第三击只算新单击、System 持续按住/20 ms 假释放、逻辑模式取消半个或按住的旧点击、采样中断、无空间姿态的原始按钮；`vr-ui-model` 覆盖无手部姿态开关键盘/退出设置且只释放旧输入，不发指针事件。确定性时间轴在第二次释放后 30 ms 触发切换，下一毫秒的新按下可参与新手势；System 后观察到 30 ms 稳定释放即可接收新按下，取消原额外 350 ms 静默。此证据是 CPU 模型时间，不是实机显示延迟；物理按键、遮挡与主观快速操作仍需佩戴验收。本轮不新增或修改运行时内部适配。

比较切换耗时时使用相同直连路径、画质/码率/帧率、同一原生 launcher 命令到首次已呈现 VR 帧；冷启动 SteamVR 与热切换分别报告。保存中位数、最大值、回退次数和版本/哈希。网络故障测试通过独立时钟模型和隔离 socket pair，不能向真实用户串流服务注入伪造包。物理 System/Menu 验收与主观流畅度必须另行标注。


## Dashboard installation and privilege session (2026-11-14)

- Dashboard installer adapter selection is an interface check: the PC build ships its matching `frame-installer/` entry instead of calling an independently versioned archive script with unsupported parameters. Missing adapter reports a repair instruction before launching; it does not block existing streams. Portable packaging requires its four scripts. Plan now performs the same local manifest/hash checks as execution, preventing a command preview from silently describing an incomplete/tampered package; no remote availability or SDK/version gates are added. Command generation is shared, and `${REMOTE_ROOT}` is resolved only by actual execution. This adds local hashing only when explicitly previewing/installing, never on the streaming path.

- Desktop session notification: opcode 11 requires loopback, the consumed ticket, matching client IP, current trust/policy and at most one watcher per ticket (within the existing 64-ticket bound). This prevents attaching an unrelated notification owner; rejection uses the existing lease path. One loopback exchange is added at session admission, never at desktop/VR switching. Steady-state polling is nonblocking on the existing control loop; there are no added sleeps or quiet periods. Explicit disconnect sends a byte and retires that session; normal watch closure clears its UI/ticket. Native watch EOF alone retains heartbeat fallback, so Dashboard scheduler stalls do not look like explicit revocation. Trust remains available for a new connection. Full exit sends before SteamVR shutdown and joins; service lifetime is unchanged. Native isolated-loopback tests cover notification, EOF, legacy opcode closure and idle poll cost; actual headset latency requires separate measurement.

- `dashboard/service_elevation.rs`: local-only first-instance pipe, exact executable/parent/peer PID checks and fixed opcodes prevent another process from borrowing the worker for arbitrary elevated commands. The parent handle prevents PID reuse; closure/parent exit revokes future operations. The 30 s connection deadline applies only after first UAC approval, not ordinary commands or streaming. Subsequent operations reuse the worker without added waits. A command failure retains authorization; broken transport reports failure and lets the user explicitly retry authorization. Existing stopped-service update/uninstall checks still protect active streams.
- `frame_installer.rs` / `frame_release.ps1`: only successful installation with one valid SSH-derived hostname/IP can submit trust. Plan, failed install and malformed/duplicate receipts never trust a discovery candidate. Saved-session acknowledgement controls the success label; after 10 s without confirmation the UI reports incomplete pairing, without disconnecting a stream or undoing valid trust. One extra SSH exchange is installation-only; source BuildOnly stays side-effect free. Captured plan pipes are drained continuously; retained output is at most 128 KiB per stream/result file. Interactive installation keeps console handles intact and removes its reserved temporary result file after completion.
- Driver registration summary is diagnostic, not a stream admission gate. Quiet logging uses atomic severity checks; it neither gates media nor suppresses required state/performance events. No runtime-internal adaptation is introduced.

### 安装后的配对身份

PC release adapter 保留 SSH 认证、合法 hostname 与非回环/非未指定 IPv4 地址校验，防止写入错误的设备信任。只对既有的身份查询增加 `-4`：双栈主机的 IPv6 本地链路回执可能带头显侧 `%wlanap`，不能作为 Windows 地址使用。传输/安装的地址族选择不变，不增加 SSH 调用、等待或重试；抖动仍使用原 SSH 超时。IPv4 不通或身份不合法时保留已完成的客户端安装，明确报告配对失败且不发出信任回执，修复连接后可重试。不会去掉 scope 后猜测地址或以 DNS 结果替代认证回执。

## Dashboard release actions (2026-11-15)

Uninstall requires explicit UI confirmation to prevent accidental client removal; cancellation launches nothing. The existing verified package, SSH authentication and native ownership/lifetime checks remain authoritative, and ordinary uninstall preserves settings. Busy admission prevents overlapping operations on the same UI; failure releases it and permits retry. No streaming or normal-input wait is added.

Manual update checks allow one background request at a time, with a 15-second network deadline and 1 MiB response bound; notes retain at most 16000 characters. These protect the UI from stalled/oversized replies. HTTP failures and malformed/non-stable versions report errors, 404 reports no available stable release, and the user can retry. These checks never gate startup, media or installation and require no session locks or runtime-internal adaptation.

Release build validation rejects a versioned OpenXR DT_NEEDED entry: this specifically prevents the observed exit 127 when CI links Ubuntu's `.so.1` but Frame provides SteamVR's unversioned loader. The existing builder supplies a matching public link reference; rebuilding is the recovery. The check runs only on build artifacts, with no installed library, OS/version precheck, network retry or steady-state latency cost.
