# 双端语言与品牌维护

PC Dashboard 的 `i18n.rs` 默认跟随 Windows 用户地区语言（公开 GetUserDefaultLocaleName API）；Linux 使用 LC_ALL、LC_MESSAGES、LANG。中文地区使用简体中文词表，其他语言使用英文；未知条目保留英文。语言下拉框支持“系统语言 / English / 简体中文”，明确保存过的手动选择继续有效。缺失、未知或损坏的 UI 配置回到系统语言。语言选择不改变串流设置键。

Frame 自动语言以 **SteamVR 公布的界面语言**为准，不读取 LC_ALL、LC_MESSAGES、LANG 或 Linux 地区配置。读取 OpenVR 标准 openvrpaths.vrpath 中的 log 目录，解析当前 vrserver.txt 公布的 `Web language is <token>`；schinese/tchinese 使用中文，其他或无法确认时回退英文。头显原协议切换按钮现在是“跟随 SteamVR → 简体中文 → English”的语言选择；GTK 连接窗口也提供同样的语言选项，手动设置保存到独立 UI 偏好 `surfxr-client/ui-language`，不改变连接配置、IPC 或串流参数。各显示进程每秒最多刷新一次语言来源，已打开的 Dashboard 与串流设置浮层会重绘。

运行时诊断适配：项目附带 OpenVR SDK 的公开原生接口未提供与 SteamVR Web UI 的 GetSteamLanguage 等价的语言查询，因此读取 SteamVR 自身已经解析并公布的诊断值，不猜测私有接口或对象布局。配置最多 64 KiB、最多 8 个绝对日志路径；日志读取有界尾部 1 MiB，启动行不在尾部时读前 64 KiB。只接受长度不超过 32 的小写字母/连字符/下划线 token，最后一条有效公布值优先；未知语言、缺失或解析失败回退英文，不回退 Linux 语言。该适配依赖 SteamVR 继续公布同一诊断字段；若未来字段改变，自动模式安全回退英文，手动语言仍可用。当前头显实测 SteamVR 公布 schinese，Linux 为 en_US，二者不再混用。

SurfXR 只提供当前一个产品协议。头显不再提供协议切换，新建连接和公开客户端/Viewer CLI 仅使用当前协议；保存过的历史版本配置自动迁移到当前版本。历史 wire 解码/回归向量保留，不作为可选择的产品协议。

显示名称统一为 **Dynamic Foveated Streaming／动态注视点串流**。设置键、IPC 字段和协议能力标识保持原字节，不因 UI 名称更换而迁移。

Frame 使用 Pango/Cairo 做 UTF-8 排版与字形渲染。内嵌的 OFL 字体子集经进程私有 Fontconfig 注册，不修改系统字体，不依赖头显预装中文字库。浮层保留原始 ASCII 路径，中文文字缓存为有界 alpha 遮罩；文字和按钮统一按实际墨迹边界测量，扣除 Pango bearing/行高留白，不再按原英文长度、UTF-8 字节数或估算列宽居中。Dashboard 根据实际文字宽度居中及裁剪，截断不拆分 UTF-8 字符。

字体来源：[Noto Sans CJK SC 2.004](https://github.com/notofonts/noto-cjk/tree/Sans2.004)，原始 `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf` 的 SHA-256 为 `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`。保留 Adobe 版权与 SIL OFL 1.1，将子集重命名为 SurfXR UI Sans。重建前将原始字体放入根 build/deps/fonts，安装 fontTools 4.58.0，然后在根运行：

```powershell
python scripts/fonts/subset_font.py build/deps/fonts/NotoSansCJKsc-Regular.otf
```

子集包含词表所需中文、ASCII 与省略号。更改词表必须同步重建；原始字体哈希不匹配则拒绝。CMake 将字体嵌入现有二进制，源码清单包含 .otf；发行包和原生安装器保留字体许可证。

项目早期的界面标签、帮助文本、私有设置类和函数、测试标签现统一为 SurfXR。测试中保留的 ALVR 字样只用于准确验证既有 wire magic、认证域和密码学参考向量；旧配置 `EVO_VIDEO` 与既有 IPC `evo_video_controls` 是兼容契约，私有成员使用新名称。不要通过改测试期望值掩盖协议变动。上游署名、许可证、依赖网址和历史来源说明继续保留。

本次无新增运行时内部适配；语言与字体均采用公开 OS/库 API。语言测试覆盖地区匹配、优先级、英文回退、未知地址保持不变及真实中文像素缓存一致性；离线预览与佩戴验收仍需分别记录。

## Public validation

Current keyboard/menu/reset ownership is in [ARCHITECTURE.md](ARCHITECTURE.md). Run the language, glyph and UI checks in [TESTING.md](TESTING.md) against the selected public commit.
