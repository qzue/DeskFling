# 桌面集成验证 · 2026-11-11

本轮以当前源码增量开发，未改变产品定义。环境：Apple M1 Pro arm64、32 GiB、macOS 15.7.5 (24G624)、Xcode 26.3 / Apple Clang 17 / macOS SDK 26.2。Windows/远程/CI 均无可用且已授权的执行环境。前一轮证据原样保留。

## 同日复核通过：两项只读位置对齐

用户明确要求继续人工复核。使用未改动的最终 Release 安装包（build `1488cfde57e99d03386b56ea3b70d7f355b925f75288c8b20f8f90ad470b3938`，CDHash `49c8d6eb5173a588df38241273877736c9359412`），新进程 PID 8868。应用内无提示权限检查为 0，重新读取用户选定两项，25 次查询成功、0 失败，系统图标 2/2；原始 dpos 仍为 `(1662,892)` 与 `(102,89)`。

按面板实际顺序先 #2（右下）、再 #1（左上）完成两次用户指针采样。当前单屏 1728×1117 points 上，候选转换得到 AppKit 全局中心 `(1662,225)` 与 `(102,1028)`；实采点分别为 `(1659.7456,226.3964)` 与 `(103.2139,1032.1013)`，误差 **2.652pt、4.277pt**，均小于 12pt 门槛。没有交换 sample ID 或放宽容差。

用户开启十字预览并亲自点击“标记对齐一致”。原生 AX 状态与应用导出均记录 `coordinate_validation=manual_pointer_samples_single_screen_within_12pt`、`preview_opened=true`、`preview_acceptance=user_reported_aligned`、`preview_active=false`。预览已按验收按钮流程关闭，控制面板继续可操作并成功导出；未将该状态扩大为原生资源泄漏、自动超时路径或实际鼠标穿透已经验收。

本次成功原始诊断 新建保存，先前失败记录完整保留。只证明本机当前单屏、所选两项、当前快照的有容差人工位置对齐，不是像素精确、全部桌面项目或多屏通用契约。图像矩形/标签矩形仍为 null；pbnd 不作为图像或文本范围。Spaces、全屏、Dock、输入穿透、无双影和真实持久恢复仍独立未验证或阻塞，真实桌面物理模式仍禁用。本次未改源码、未重建、未重复跑单测或 600 秒性能测量；没有移动/重命名真实文件或更改 Finder 设置，导出在 Documents 新建一份脱敏 JSON。

## 同日追加复测：最终安装包真实读取

用户再次要求测试。未修改源码或重新构建；最终二进制 SHA256 仍为 `620282862c5bca2cef26a70b4d646b37da416d8e81c222c3a032b21df905ee74`，build ID `1488cfde57e99d03386b56ea3b70d7f355b925f75288c8b20f8f90ad470b3938`。通过原生 `.app` 的检查面板操作，PID 8031 无提示权限预检为 0。

上次两个完整文件名不在当前选择器列表中，未按近似名称猜测身份；由用户重新选定两项。此次同进程查询 **25 成功 / 0 失败**，Finder 列表 17 项，URL 检查 17 项，所选两项匹配且身份复查一致，系统图标 2/2。原始 `dpos` 为 `(1662,892)`、`(102,89)`，不再是此前样本的 `(-1,-1)`。`pbnd` 仍为 `[143,126,207,190]`、`[253,126,317,190]`；不能将这些矩形当成桌面图像/标签范围。

首次两点人工采样失败：当前面板按 Finder 返回顺序先 #2、后 #1，而采样位置恰好交叉对应。按当前单屏候选规则，分别距另一项预期中心约 2.379pt、3.245pt；仍保留原始 sample ID 与 failed 状态，不通过事后交换样本来宣称对齐通过。该现象提示 UI 采样顺序容易混淆，但不单凭数值推断人的实际操作意图。

原始脱敏报告 app-layout-automation-retest-1.json 来自应用导出，默认保存位置为 Documents；比较计算另存 retest-1-analysis.json。没有桌面截图、Finder 设置修改、真实图标移动或文件重命名；没有再次请求系统授权。坐标变化原因尚未查证，不能归因于本次代码修复（本次没有改代码）。

## 第三轮：应用内授权与有限只读预览（2026-11-11）

本节记录当前增量；下方第二轮内容是历史证据，不代表第三轮仍无授权入口。

- 原生入口：`sandbox.mm` 的 DPP 菜单 → 桌面接入检查，`desktop_access.mm` 实现 AppKit 面板。启动和首次开面板不发 Apple Events、不枚举 Desktop。普通检查 `askUserIfNeeded=false`，仅用户接受用途 sheet 后才为 true。
- 本机 SDK `AppleEvents.h` 明确指出授权检查线程安全、可能无限等待且不得在主线程调用。当前使用一个在途后台操作；关闭面板递增 generation，结果迟到只释放在途槽位并记录丢弃，不重新打开窗口。关闭重开不能绕过在途槽位，退出不等待系统授权，也不声称可以取消系统操作。
- 授权目标 `com.apple.finder`，检查和真实事件范围一致为 `core/getd`。读取保留 `kAENeverInteract | kAEDoNotPromptForUserConsent`、每事件 120 ticks。-1744 为需要同意；-1743 为不允许发送（不能只凭该值推断用户拒绝）；超时、目标不可用、传输、Finder reply 错误与 descriptor 解析分别报告。无自动重试、tccutil/TCC 数据库访问或其他权限替代。
- `finder_read.h` 的 object/property/geometry/readFinder 继续由 CLI 和应用复用。原 `finderReport` 移入 `finder_report.h`；应用使用同一读取/解析基础构造最小 version 查询，再枚举 specifier 并仅对所选 2–3 项取 dpos/pbnd。最多 128 个 URL 身份检查、15 秒循环预算（在途事件可超过预算）；没有目录递归或文件内容读取。
- URL 只留内存，用标准化 URL 匹配选择项并在读属性后再次核对，身份变化使位置未知；图像来自该快照同一 URL 的 NSWorkspace 系统图标，不称 Finder 原样缩略图。目录总访问、Finder 可见数量、图标图像与布局独立；选择器成功只证明选定子项，不标记全部 Desktop 已可访问。
- 坐标规则先是候选：单屏主显示器左上角 logical points/Y 向下。全部选定项目经用户 5 秒鼠标中心采样，至少两点相距 80pt 且误差 ≤12pt，才对本快照启用转换。数学转换全部经过 `core/scene/coordinates.*`；无平台对象/路径进入 core，不应用 Retina 倍率。该人工采样是有容差的有限证据，不是所有图标/像素精确坐标的证明。
- 预览仅十字、临时编号和来源；没有已验证矩形所以不画矩形。窗口不 key/main、ignoresMouseEvents，当前 Space、图标层级+1；不复制图像、不移动项目、不改变 Finder 偏好。60 秒自动关闭，控制面板可随时关闭；关闭面板、Space/屏幕变化、快照 5 分钟失效也清理。API 配置仍不能代替实际穿透/Spaces/全屏/Dock 验收。
- 诊断使用原有 `O_EXCL`/0600 新建写入器，默认脱敏，含来源构建 hash、CDHash、签名模式、实际 entitlements、原始状态/阶段、查询统计、原始几何/类型码、坐标采样、矩形 null、预览结果；不导出私人名称、路径或截图。面板本地显示所选名称帮助采样，导出不包含它们。

### 已查证的系统依据

本机 Finder.sdef 中 item 的 dpos 是 point、项目在桌面的位置；item 的 pbnd 是 rectangle、项目边界，window 类另有同码 pbnd 表示窗口边界。字典没有定义 dpos 的锚点/原点/单位，也没有独立图像或文本矩形。原文摘要与 SDK threading/返回码见 `evidence/finder-sdk-automation.txt`。不能把 item pbnd 或 desktop window bounds 当成图像/标签范围。

官方 [AEDeterminePermissionToAutomateTarget](https://developer.apple.com/documentation/coreservices/3025784-aedeterminepermissiontoautomatet)、[NSAppleEventsUsageDescription](https://developer.apple.com/documentation/bundleresources/information-property-list/nsappleeventsusagedescription) 与 [Apple Events entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.automation.apple-events) 已核对。本地开发构建保留非 Hardened Runtime / 非 App Sandbox，以 ad-hoc 显式标识 `dev.dpp.sandbox` 签名，无 entitlements。没有关闭已有系统保护，也没有伪造 Developer ID；签名/entitlement 不是用户授权。若未来改变签名模式须重新审计。

### 实机授权与暴露的旧查询错误

第一次 `.app` 检查返回 -1744，用户在本轮对话明确确认亲自看到系统 Finder 自动化提示并允许；应用 PID 1921 记录 `ask_user_if_needed=true / OSStatus=0`。同进程 version 只读查询成功，项目列表失败为 Finder reply -1700。原始用户导出报告保留为 `evidence/user-export-dpp-access-6D0EC935-E441-4A70-B645-F17543D634C0.json`；授权前/后记录另见 `user-export-dpp-access-C06D2C91-1828-406F-B308-3F21A7B92B39.json`。用户在系统选择器曾选择超范围/非直接子项，被拒绝后重新选择；没有执行任意目录递归。

原因核查：原构造以 `typeEnumerated` 包装 `kAEAll`，而官方 [formAbsolutePosition](https://developer.apple.com/documentation/coreservices/formabsoluteposition) / [typeAbsoluteOrdinal](https://developer.apple.com/documentation/coreservices/1645753-anonymous/typeabsoluteordinal) 要求特殊序数使用 `typeAbsoluteOrdinal ('abso')`。新增共享 `allItems()`，修正 CLI 和应用两条调用，并加入精确 descriptor 类型和 native OSType 字节测试。不能把此前预检通过当作此查询已运行成功。

修正版通过原生应用重启后无提示检查仍为 noErr；仅说明本机这次重建/重启的观测，不保证未来 ad-hoc CDHash 改变后权限持久性。

### 修正版真实字段读取：成功与对齐失败分开

`evidence/app-layout-automation-ordinal.json` 来自 **PID 3617 / build ec80ec3a262eab8586797828af6a2d8babb228ec160cf91c2c4bc4f9313bde3a / CDHash 6af042bcfb680da9a7525cce2a02c7256c26b750**。用户选择 2 个 Desktop 直接子项；同进程无提示预检为 0，16 次查询成功、0 失败，Finder 返回总列表 16 项，检查 8 个 URL 后匹配两项并停止；两项 URL 复查一致，系统图标解码 2/2。不能将 Finder 总列表数量视为实际可见图标数，也未重新枚举整个 Desktop 目录。

| 字段 | 当前实际返回 | 可证明的范围 |
|---|---|---|
| dpos | 两项均 `(-1,-1)`；descriptor `0x51447074` = `QDpt` | 真实点 descriptor 已读到并按 QuickDraw v/h 解析；数值不在当前单屏候选域，不能当成可用桌面坐标，也不猜测 -1 的系统语义 |
| item pbnd | `[143,126,207,190]`、`[253,126,317,190]`；descriptor `0x71647274` = `qdrt` | 是对项目发出的 pbnd 查询并成功返回矩形；原点/单位/容器显示方式/是否含标签仍无充分证据，不将其替代 dpos 或命名为图像范围 |
| 图像矩形 | null | 未验证，虽 pbnd 恰为 64×64 也不据此推断图像矩形 |
| 标签矩形 | null | 未取得/未验证 |
| 人工指针采样 | 一点记录为不匹配；报告保留坐标，不含文件名称 | 未完成人工对齐，预览从未开启，不声称资源关闭/输入穿透实机通过 |

**结论是“读取成功，坐标契约未验证”**。没有用窗口 bounds、网格或 Retina 倍率填补。最终增加负值/重合样本候选门槛，避免让用户对这类值做必然失败的采样：保留原始字段，采样和预览禁用。此 gate 是单屏预览约束，不会让通用 geometry 丢弃合法负屏幕原点数据。

最终产物 source build ID 为 **1488cfde57e99d03386b56ea3b70d7f355b925f75288c8b20f8f90ad470b3938**，CDHash **49c8d6eb5173a588df38241273877736c9359412**，见 `evidence/app-identity-automation-final.txt`。相对上述真实读取版本仅补充候选门槛/明确状态文案、默认导出到 Documents 和相关测试；真实读取证据仍使用原始构建 ID，不冒充最终二进制重新读取的结果。最终构建的实际性能/启动记录另见 performance/testing。

用户以初版默认保存位置在 Desktop 新建了三份诊断 JSON，原件均保留；程序未隐藏/移动原图标、修改 Finder 偏好、重启 Finder、替换壁纸或捕获桌面屏幕。最终默认导出位置改为 Documents，仍允许用户显式选择位置，不覆盖已有证据。

### 后续人工验证操作

打开最终 `dist/macos-dev/physics_sandbox.app` → DPP → 桌面接入检查 → 检查当前权限 → 选择 2–3 个桌面直接子项并读取。若仍为 `(-1,-1)`，导出诊断并保持预览禁用，不改变 Finder 设置来强行通过。只有原始点满足单屏候选域且多点人工采样符合后，才开启只读十字预览、核对并报告一致/不一致；随后检查关闭和普通应用操作。无双影、实际图像/标签矩形、全屏/Spaces/Dock、真实持久恢复仍是独立任务。

最终安装包 1488cfde 在 600 秒测量结束后，再经 Launch Services 正常启动并做原生 UI 回归：Gravity 50 → Billiards 51 → Gravity 50、R 重置、Space 暂停；首次面板仍 not_checked / not_requested，点击无提示检查得到 OSStatus 0。未在最终进程重新选择文件，因此不将先前 ec80ec3a 的字段报告改作最终读取证据。最终面板已留给用户。见 `evidence/app-ui-automation-final.txt`。

## 三项 macOS 能力分开判断

| 能力 / 候选 | 一手依据、公开性与机制 | 权限 / 最小代码 | 本轮结果 / 限制 |
|---|---|---|---|
| Desktop 目录项目 + 系统图标 | Foundation `URLsForDirectory:NSDesktopDirectory`、目录直接枚举；公开 [NSWorkspace icon(forFile:)](https://developer.apple.com/documentation/appkit/nsworkspace/icon(forfile:)) | 系统 Desktop 文件夹访问规则；`probe.mm:directoryReport`，显式 `--desktop-files` | **已完成指定实机测试**：11 项、11 张可解码 CGImage、0 图像失败。本次已有访问权限，没有提示。不递归、不主动下载云文件、不输出路径。不是 Finder 显示列表，也未证明缩略图一致 |
| Finder 实际布局 | 本机 `/System/Library/CoreServices/Finder.app/Contents/Resources/Finder.sdef`，公开脚本字典；`core/getd` 读取 desktop 的 item、`dpos`、`pbnd`、`pURL`、`dktw`、`icop/lvis/iarr` | Finder Automation；`finder_read.h` + `probe.mm:finderReport`，`--finder-layout` | **已编码、已编译，实际预检阻塞**：`-1744` needs_user_consent，layout=null。所有 get-data 路径尚未运行，不能宣称坐标可用 |
| Accessibility 布局候选 | 公开 `AXIsProcessTrusted`；SDK `AXUIElement.h` 的属性读取机制（`AXAttributeConstants.h` 定义位置为全局屏幕左上角，尺寸单位 points） | Accessibility；本轮只调用 `AXIsProcessTrusted` | 本次信任为 true。没有实施 Finder AX 树遍历，没有把权限存在当作图像/项目布局读取成功；不自动操作 Finder UI |
| 透明窗口 / 层级 | 公开 [NSWindow](https://developer.apple.com/documentation/appkit/nswindow)、[CGWindowLevelForKey](https://developer.apple.com/documentation/coregraphics/cgwindowlevelforkey(_:))、[CGWindowListCopyWindowInfo](https://developer.apple.com/documentation/coregraphics/cgwindowlistcopywindowinfo(_:_:))；桌面图标级+1、普通级自有 witness | 自有窗口无需 Finder Automation；`window_probe.h:windowReport`，显式 `--window-test` | 只检查元数据、焦点及关闭；具体本轮结果见 `testing.md` / evidence。无像素捕获；不能证明透明合成、Dock、全屏及实际鼠标路由 |
| Spaces / 输入策略 | `canJoinAllSpaces / stationary / fullScreenNone`、[ignoresMouseEvents](https://developer.apple.com/documentation/appkit/nswindow/ignoresmouseevents) 为公开配置；SDK NSWindow.h 的注释 | 测试窗口不接收输入，不激活应用 | 代码设置标志不等于实际 Spaces/Mission Control/全屏/点击穿透已验收；这些交互本轮未执行 |
| 通过透明镜像实现无双影 | 自有窗口只控制自身合成；不拥有 Finder 原图标 | 尚无可核验隐藏接口；无生产代码 | **阻塞**：即使镜像透明且对齐，原图标仍会出现，不能称无双影 |
| Finder 字典隐藏桌面窗口 | Finder.sdef `window.visible (pvis)` 为只读；应用 `visible` 是整个应用层可见性，不是选择性、可恢复的桌面图标显示权 | 没有发送 set/close/hide 事件 | 在核查字典范围内未找到所需安全接口；没有借机关闭/隐藏 Finder 窗口 |
| 系统 Desktop 显示设置 | Apple 的 [Desktop & Dock 使用说明](https://support.apple.com/lt-lt/guide/mac-help/mchlp1119/mac) 提供 Show Items 用户设置（在线现行说明，未将其当作 15.7.5 自动化契约）；不是已验证的按会话图标隐藏 API | 用户全局偏好；本轮不改动 | 仅候选决策，未操作。会影响用户桌面定义、恢复及 Spaces 行为，不能默认采用 |
| 私有 Finder 偏好、重启/注入、壁纸遮罩 | 不作为受支持方案 | 无代码、未执行 | 不属于当前约束允许路径；未关闭 SIP、未截图/铺固定壁纸 |

**结论：在已核查范围内尚未找到符合约束的方案。** 这是对以上公开字典、SDK 和候选的有边界结论，不是对所有 macOS 技术路径的不可能性证明。真实模式继续禁用。

## 权限与只读边界

`AEDeterminePermissionToAutomateTarget(core,getd,false)` 在后台线程预检；依据本机 SDK `CoreServices/AE.framework/Headers/AppleEvents.h`（Xcode 26.3 SDK 26.2），`-1744` 表示需要用户同意，`-1743` 表示不允许。后续读取还带 `kAENeverInteract | kAEDoNotPromptForUserConsent`；不会因为预检通过就允许隐式提示。每个事件超时 120 ticks，最多 128 个项目，项目循环约 15 秒预算（正在执行的事件可能使总时间超出预算）。Finder 未运行则返回失败，不主动启动或重启。枚举截断或任一已枚举项目的位置读取失败时，顶层状态为 partial_or_failed_layout、退出 3；不能只有枚举成功就宣称布局读取成功。

本轮没有申请新权限、没有操作系统授权对话框。Automation 与 Accessibility 是不同权限；AX=true 不会绕过 Automation。命令行工具的责任进程/启动上下文会影响系统权限判定，换终端不保证继承授权。当前无提示探针不会主动生成授权弹窗；若要补齐布局运行证据，下一步须在用户同意后实现/使用正常的显式授权入口，在相同上下文重新预检，再运行读取。

报告默认只有内部编号、计数、类型码与几何值；不得保存完整私人路径。Finder 返回的 URL 只在内存中用于该桌面项目的系统图标读取。`--output` 以 0600、O_EXCL 新建报告；已有文件不覆盖。它是诊断输出，不是会话 journal，不提供原子恢复语义。

## 坐标与特殊项目

- Desktop 目录没有屏幕位置；不能通过排序、自建网格或合成场景坐标补齐。
- `dpos` 字典描述为项目在桌面的位置，`pbnd` 为项目边界。当前字典没有足够契约证明图像中心、标签范围、原点与缩放。QuickDraw descriptor 按 v/h 与 top/left/bottom/right 解码为原始 x/y、LTRB；非法/非数值/非有限值保持 unknown，不回填 (0,0)。这些解析有单元测试，但真实 Finder 返回尚未验证。
- `NSScreen.frame/visibleFrame` 是 AppKit points、左下坐标；backingScale 仅代表 backing pixels 与 points 比例。Finder 原始值在独立确认前**不进入** `core/scene/coordinates.*`。未来要验证主屏顶部原点、其他屏负原点、工作区偏移，再用共享转换进入模拟空间。
- 本轮 Desktop 样本的 ubiquitous 项计数为 0；没有云端未下载项目证据，不能声称 iCloud Desktop 已验收。没有主动触发内容下载。
- 堆栈会让文件列表与可见图标列表不同；本机 Finder.sdef 未发现堆栈显示对应接口。不能以每个目录项目绘制一个图标宣称还原堆栈。
- Finder 字典的 desktop-object 包含 disks，目录枚举不代表桌面卷；布局分支准备只读卷类别和四项公开卷显示偏好，但因权限阻塞未执行。磁盘/网络卷/特殊项目仍待测试。

## ADR-002：若考虑放宽约束

备选产品约束变化是由用户主动调整系统“显示桌面项目”设置，再运行副本；收益是可能避免原图标重复，代价是全局桌面行为改变、退出/异常后的设置责任、布局读取与权限变更，以及 Spaces/Stage Manager 兼容性。未证明它可提供按会话隔离和可逆恢复，也未验证关闭显示后布局读取是否仍可用。**当前决定不采用，不执行任何设置修改。** 只有用户另行批准产品约束，并实现真实持久记录、恢复与隔离测试后才能重新评估；现有 mock SessionCoordinator 不满足这些条件。

## Windows 状态

A 已编码未编译/运行；B 已编码未编译/运行；C 未实现且前置条件不成立。没有把 macOS 的 `--window-test` 当作 Windows C，通过与否均不构成 Windows 运行证据。第一轮 MSVC/CI 配置保留，本轮未触发远程操作。
