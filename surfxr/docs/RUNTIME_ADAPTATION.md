# 运行时兼容与动态适配规范

## 强制规则

禁止通过写死产品版本/build、地址/RVA、私有对象偏移、私有接口版本或捕获的指令字节模板解决兼容问题。优先使用 SDK 声明的公开接口、导出符号和能力查询。不得把版本白名单换成特定 pattern、通配签名或运行时字符串搜索来规避本规则。

公开接口不足时才允许动态解析当前实现：入口必须尽量来自公开接口；偏移、槽位和对象关系必须从当前实现推导；验证内存范围、可执行权限、调用参数及转发语义。无法证明时保留公开路径或停用对应能力并给出诊断，禁止猜测、截断 handle 或强制启用。

维护报告必须单列 **DYNAMIC_RUNTIME_ADAPTATION（存在运行时内部适配）**，说明必要性、公开定位入口、解析内容、失败行为、实际验证环境及尚未验证的兼容边界。不能把一次运行成功描述成所有后续版本兼容。

ISA 指令编码、SDK 的公开 ABI/协议常量、项目自有 IPC 修订号、配置迁移版本、可复现依赖锁定以及历史取证记录不属于版本补丁。它们仍需由各自 owner 管理；不得借此保留私有版本特判。

## 2026-11-12 维护审计

**DYNAMIC_RUNTIME_ADAPTATION：PC Boolean 输入和 Frame 输入捕获仍包含运行时内部适配。** OpenVR 未公开第三方 detour 之前的原始转发目标，也未提供 Frame 系统键原始输入的通用订阅接口。当前方案因此仍有兼容边界，日志使用相同标签提示适配或拒绝结果。

| Owner | 原问题 | 当前处理 |
|---|---|---|
| `pc/surfxr-server/surfxr/server_openvr/cpp/surfxr_server/BooleanInput` | 原修复依赖捕获的 x64 字节模板、固定对象偏移及旧 host 接口版本 | 从 SDK 的公开输入接口取得入口；匹配当前运行映像与只读、不可执行的磁盘 PE，解释原始转发语义，并与当前 create 路径交叉验证；动态推导对象偏移和目标槽位 |
| `frame/input` | 请求固定的私有接口版本，并按固定 ARM64 模板识别对象 | 与 PC 共用仓库 SDK 头文件，不使用 sysroot 的旧 sample SDK；只请求 SDK 公开输入接口；解释其 Boolean/skeleton 四个方法的转发语义，证明参数保留、共同目标及不同槽位，再安装并恢复动态槽位的 hook |
| `pc/surfxr-server/surfxr/dashboard` | 手写 overlay 接口版本、函数表索引和 flag 值 | 构建时读取仓库 OpenVR SDK 的接口声明，bindgen 生成函数表及枚举；运行时通过公开 `VR_GetGenericInterface` 获取对应接口 |

实机确认当前公开 Host 的 pose 入口不一定等于旧 Host 的别名入口，因此不把 Host 入口相等作为条件，也不查询旧 Host 版本。安全条件是公开输入接口原始转发语义与当前 create 路径/目标对象的一致性。

共享语义分析器属于 `shared/platform`。它从已取得的方法入口最多解释 32 条指令，跟踪寄存器中的对象加载关系，支持透明加载/寄存器复制/尾跳转和无副作用 landing pad。对象偏移、槽位、寄存器分配不是匹配常量；实现中的位掩码是 ISA 解码字段，不是某个 SteamVR build 的签名。不扫描整个模块寻找相似代码。

未知指令、调用、写内存、栈修改、非易失寄存器修改、参数窄化/覆盖、循环、越界或不可读/不可执行目标均拒绝。PC 保留公开输入调用并告警；Frame 保留公开 hook，仅按实际注册/采样认定捕获就绪。改变编译器转发形式可能使解析拒绝，仍需后续实机验证和受控扩展。

同时检查了维护代码中的私有接口字符串、运行时版本判断、固定地址/偏移、vtable、memcmp 和 pattern/signature 用法。保留的项目包括：公开 proximity 能力检查、标准运行时可执行文件名、SurfXR 协商版本、项目自有 IPC/设置迁移、依赖锁定及导入 SDK。历史分析材料不作为生产定位逻辑。

## 验证要求

- CPU 测试使用不同对象偏移、寄存器分配和目标槽位，并覆盖参数截断、未知指令、循环和非法地址的拒绝。
- PC 测试覆盖两手 Boolean 路径、64 位 handle、状态和时间偏移；Frame ARM64 测试执行实际汇编转发 fixture，验证动态槽位捕获与卸载恢复。
- 全量构建、结构检查、安装包 manifest/hash 检查与实机状态分开记录。实机无报错不等于方向键/系统键的佩戴验收通过。
- 本次具体命令、结果及产物哈希保存在仓库根 `build/reports/runtime-adaptation-audit/`，私人日志和部署路径不进入提交。

ISA 参考：[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)、[Arm A64 指令集](https://support.arm.com/documentation/ddi0487/mb/-Part-C-The-AArch64-Instruction-Set)。

## 本轮验证记录

Windows CPU 19 项、PC 输入 3 项、PC OpenVR Rust 5 项、Frame 映射 Rust 3 项及 ARM64 CPU/隔离回环 33 项全部通过；ARM64 完整构建零警告。PC 实机两手 Boolean 目标解析成功，Frame 实机转发目标解析成功，公开 SDK overlay 查询成功。以上只证明已记录环境的构建、离线契约及运行时加载/解析，本轮没有新增人工按键或佩戴验收。发布包与已部署文件的最终哈希记录在上述维护报告目录。
