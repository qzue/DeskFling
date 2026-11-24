# 测试与真实验收记录

## 最新 Mac 人工确认：图标大小一致 · 2026-11-13 16:02

用户确认“现在图标大小一致了”。最终构建 `c8202f0082d8…` 的实际会话记录三个 64×64pt 副本；尺寸由 Finder 桌面容器配置自动读取，当前本机配置获人工大小确认。位置仍需现有人工核对，不升级逐像素自动对齐、其他显示配置或性能。反馈与会话。没有因此新增构建或重复测试；下方待确认状态为交付时记录。

## 当前 Mac 图像与尺寸回归 · 2026-11-13

最终构建 `c8202f0082d8…`：Debug、Release、ASan/UBSan 各实际 **55/55**，构建/安装/签名校验 exit 0。新增测试只使用临时合成图片和模拟 Finder 返回值。安装应用已实际读到桌面尺寸配置 64，绘制框采用 64pt；最终视觉大小尚未确认。前一缩略图版实机读取 1 张图片缩略图、2 个软件系统图标。

首次编译失败、受限类型服务导致的 2 项测试失败、首次尺寸候选 -1728 与用户“还是小”均保留。原生 fixture 29/29 在前一尺寸候选运行，最后仅修正属性引用后未重跑；不重复计为最终新结果。完整命令、身份和边界。下方为历史记录。

## 当前 Windows W3：本次配置的基础显示与交互获人工确认 · 2026-11-13 01:26

输入 `2a9f7050c388…` 的实际运行已保存完整结果：用户将静态副本、重力/碰撞、暂停/重置、透明/普通应用行为、拖拽/抛掷及输入释放五项均选为正常，退出问题选正常，聊天补充“正常”。实机记录与 A–F 范围。

当前单屏 Windows 10、120 DPI、60×60 图像、统一人工下移 5 像素；3 个快捷方式副本不加箭头。实际进入显式交互，记录 13 次按下/松开、2874 次捕获移动；保存停止 exit 0，停止后无自有捕获、剩余预览进程 0。未测资源泄漏数和 FPS 保持未知。

保留这版为当前已获基础人工反馈的开发基线。不升级自动精确对齐、其他配置、无双影、GPU 性能或持久恢复；正式模式继续禁用。本次只归档报告，没有新构建或测试。当前交接已完成，不自动展开新阶段。下方为历史。

## 当前 Windows W3：按用户选择去掉副本快捷方式箭头 · 2026-11-13 01:22

用户反馈镜像箭头偏大，随后选择“不带这个”。本轮仅让快捷方式副本不合成 Shell 箭头叠加层，主图标大小不变；读取上一报告中的 +5 屏幕像素人工偏移作为新初值。真实图标不改，下面原图标的小箭头可能仍透出；不升级自动对齐或其他人工验收。

新输入 `2a9f7050c388fe10166160a67a7127af1b8ea0eb82a1ef8196e0020e307149eb`（123 文件），本轮 Windows Debug/Release 各 **41/41**，构建/安装/ZIP/哈希/帮助/启动脚本通过，未签名。只修改 Windows 内容，Mac 未重跑，CI 未触发。新 GUI 仍待用户确认。交付记录。

下一项只核对副本不再额外带大箭头，保留原图标、微调和正式模式禁用。下方为历史。

## 当前 Windows W3：大小获人工确认，副本统一下移待核对 · 2026-11-13 01:12

系统图像列表版实机记录为 60×60 像素 / 120 DPI，用户确认“现在大小没问题”，但所有副本稍高，明确要求统一向下移。见实机反馈。这不是新的拖拽、性能或退出人工验收。

本轮保持大小/取图不变，副本默认统一向下 4 个屏幕像素；静态阶段可按 1 像素上下调整，范围 ±16，每次调整清除旧静态确认。原始坐标与人工偏移分别记录，不改真实图标，不称为自动精确定位。

新输入 `ea7173737f0dd71d895922c8de3d07499a209e55c180cc738ae0f26f70a5f275`（123 文件），Windows Debug/Release 本轮各 **41/41**；涉及共享坐标函数，Mac Debug 本轮 **49/49**。构建/安装/ZIP/哈希/帮助/启动脚本通过，未签名；新 GUI 未运行。Mac Release/ASan/人工桌面未重跑。详见交付记录。

下一项：只核对静态副本高度，必要时使用上下微调按钮并保存结果。保留原图标、正式模式禁用；精确自动对齐与未测能力不升级。下方均为历史。

## 当前 Windows W3：偏小回归已定位到实际绘制尺寸，系统图像列表候选待实测 · 2026-11-13

用户已关闭，真实副本报告回读：120 DPI、配置值 48、5 张位图 48×48、最终 38.4 DIP/48 屏幕像素，静态观感因明显偏小未通过。不是位图不足导致禁止放大的问题。原表单未填写，聊天反馈单独保存；退出问卷“有问题”的具体含义仍待澄清，不能计为退出人工通过。

新候选使用同快照完整 PIDL 取得系统图标索引，读取系统图像列表实际像素并复制图像，保留配置原值。若仍得到已失败的 120 DPI/48px 组合则保持标记；不乘猜测倍率。新增启动即写的脱敏尺寸记录，保留原图标、静态对照和正式模式禁用。精确图像边界仍未知。

最终输入 `3792d0b7ca6f83ec960addbbccd93f132bf02977812b30ef15dd6186f963e914`，123 文件；Windows 本轮最终 Debug/Release 各 **41/41**，脚本 exit 0。首次 PIDL 类型警告导致的失败及中间候选结果保留。新包已解压、哈希/帮助/v6 清单/启动脚本验证通过，未签名，GUI 尚未运行。本次 Windows 专属改动，Mac 未重跑。

下一项：确认当前普通桌面点击状态后，从新包读取实际系统图像列表尺寸并仅复查静态大小/图像/边缘；不重复所有玩法，不升级精确对齐或交互。详见本轮交付。下方为历史记录。

## 当前 Windows W3：新副本明显偏小，修订未通过 · 2026-11-13

用户反馈“你这更不对了小的多”。`b5f994f7f650…` 尺寸/画质修订不能认定有效，静态观感保持未通过。已保存的首次运行是 96 DPI、仅标记、没有副本；后一次副本运行报告尚未保存，不能混用两个会话或套用此前 120 DPI。已请求保存当前结果，无需重跑。详见尺寸回归记录。

下一项：先读同一次副本运行的尺寸证据，核对视图配置、图像列表实际像素与绘制单位，再修复。此跟进没有改源码、构建或重跑测试，不发布猜测倍率的新包；此前自动测试不代表本次视觉通过。正式模式继续禁用，下方为历史。

## 当前 Windows W3：尺寸与清晰度修订 · 2026-11-12 23:45

用户新增反馈指出副本位置偏差和边缘毛躁，静态视觉质量不再沿用先前笼统“正常”。当前改为读取 Shell 图标像素尺寸、按实际位图等比例绘制且不放大低分辨率图；移除大号绿色文字与全框，静态阶段提供“仅看原图标 / 显示副本”对照。图像角内小编号对应面板。原图标保留，精确图像边界仍未知，没有逐项偏移或隐藏功能。

新输入 `b5f994f7f6504c035f402f4fefaf3dec03f14bde5ac7f7f781d5a375721f8635`（123 文件）；本轮 Windows Debug/Release 各 **40/40**，构建/安装/打包 exit 0。涉及共享坐标函数，Mac Debug 本轮 **49/49**；首次测试编译警告已修正并保留失败日志，Mac Release/ASan/人工验收未重跑。新包未签名，已解压并校验，尚未实际运行新 GUI。详见本轮记录。

下一项：从新目录运行静态对照，核对尺寸、清晰度与位置。有问题保存停止，前置正常才继续重力/交互。实际 Shell 尺寸值、新画质与精确对齐待实测；不升级 GPU 性能、其他显示配置、正式模式或持久恢复。下方均为历史记录。

## 当前 Windows W3：缩小副本与显式交互候选 · 2026-11-12 23:21

旧播放包 `552f36e836f3…` 已实际进入静态/重力，收到 8 次暂停切换、4 次重置，退出正常；用户反馈“图标比原来的大 其他的正常”。原面板未核对字段保持不变，一般性反馈单独记录，见 真实播放。默认自定义尺寸已从 64 缩小到 48 DIP（25%），仍不声称与原图标同尺寸。

新输入 `86a1e0763b3dcac9493a94f31b1b6681d01d88b0440131173c5c031950975ad9`（123 文件），Windows Debug/Release 本次最终各 **38/38**，构建脚本 exit 0、安装/ZIP/哈希/帮助检查通过。新包未签名，已解压到普通用户目录并提供双击启动器，脚本语法和上传哈希通过。本次没有重跑 Mac。

默认仍不捕获输入；本次静态/播放确认后必须再点击“进入交互”，才允许抓取/抛掷 DPP 副本。失焦/控制面板/切换应用恢复被动播放、取消抓取和自有捕获，并恢复自有窗口层级；独立退出/停止及限时保护保留。新 48 DIP 观感、真实拖拽和输入释放结果仍待本机确认，不能把旧包正常反馈转移为新包通过。详见 本轮交付记录。

下一项：读取本次交互交接，逐项核对尺寸、拖拽/抛掷、窗口层级、切换应用释放与退出。正式模式继续禁用；精确对齐、其他配置、GPU 性能和持久恢复仍未通过。下方各次状态为历史。

## 当前交接说明 · 2026-11-12

用户回复“测完了”后补充“只看到了绿色 S1～S5 标记”。本轮回读未发现新播放包 checks 目录，新 EXE 哈希与交付清单相符、回读时预览进程数为 0；无法据此确定用户看到的是哪个二进制，不能升级静态副本或重力验收。已在新包测试目录增加 `Start-W3.cmd` 双击入口，固定调用该目录的 check-w3.ps1；上传哈希复核通过，未从 SSH 执行。没有修改程序源码、重编译或重跑测试。证据见 交接澄清。下一项仍是从新入口进入副本和重力阶段，保存独立结果。

## 当前 Windows W3：静态副本与非交互重力包 · 2026-11-12 22:01

标记包 `d4e5a9a0764f…` 的本机运行和用户四项正常确认已独立归档，见 实机记录。据此前置，现有透明路径接入同快照图像缓存和共享重力；每次仍先显示新标记，显式确认后显示 DPP 自定义 64 DIP 副本，本次静态确认前不能播放重力。鼠标交互未开放，原图标保留，正式模式禁用。

新包输入 `552f36e836f307f9a27bc7ada6e0c46ae76f26038942091365d90cc5477872af`（123 文件），Windows Debug/Release 本次各 **36/36**，构建脚本 exit 0，安装/ZIP/哈希/帮助检查通过，未签名。首次 MSVC /WX 失败已修复且保留证据；本次未重跑 Mac。新包已解压到普通用户目录，顺序交接脚本语法与哈希通过。

本次 90 秒保护停止桌面绘制，保留面板供填写保存；静态、重力、暂停/重置、窗口行为和退出的新包人工结果均待确认。旧包四项正常不转移为新二进制验收。详见 新包实现与验证。下一项：读取这一次静态/播放交接结果，前提成立后才考虑显式交互。精确对齐、其他显示/系统配置、GPU 性能与持久恢复仍未知。下方旧计划为历史。

## W3 标记实机反馈已到达 · 2026-11-12 21:45

本包 `d4e5a9a0764f…` 在 session 1 执行了 GPU 初始化、坐标映射和 Present/Show，90 秒自动关闭，退出码 0，脚本记录剩余预览进程 0；用户确认退出后桌面正常。面板选择因未点保存仍为未核对，原始报告保持不变；随后用户针对本次运行补充“这四项都正常”，分别确认绿色编号/锚点附近对应、背景透明、桌面/任务栏点击和普通应用层级。

原始报告、交接结果、独立人工补充。当前单屏 Windows 10 配置下基础标记检查通过，精确像素对齐、持久桌面层级、GPU/FPS 和其他配置仍未验证。静态副本与重力尚未运行；下一步复用已验证路径连接自定义图标副本及共享重力，继续保持原图标和正式模式禁用。

## 当前 Windows W3：非隐藏透明标记原型 · 2026-11-12

新增 `desktop_mirror_preview.exe --report <新文件>`，标题“桌面镜像开发预览——保留原图标”。复用既有 Shell 快照和图像缓存，在受限单屏、无滚动 ListView、PMv2 条件下记录锚点 → 屏幕 → 预览客户区的转换；D2D 使用明确像素单位，不用 120 DPI / 100% 差异猜测比例。原图标保留，正式模式禁用。

最终输入标识 `d4e5a9a0764f43c8803c12f529b2bd5eaa4100ca2d6b5f29ba1ec75f7e0bbbd1`（123 文件），新开发 ZIP 已构建、安装、哈希核对并解压到 Windows 普通用户测试目录。Windows Debug/Release 本轮各 **33/33**，Mac Debug **47/47**；新 EXE 帮助 exit 0，未签名。Mac Release/ASan 和人工桌面本轮未重跑。详细包身份、命令与源码变更见 W3 原型记录。

当前仅开放绿色锚点与编号，有独立停止面板、90 秒自动关闭，不捕获输入、不永久置顶、不绑定或修改 Explorer。标记、透明、点击穿透和层级尚待登录桌面人工确认；静态副本、重力与交互保持未开放，不能宣称 W3 完成。交接已发出；脚本加 BOM 后已传输，SSH 短暂超时后恢复，最终语法检查错误 0、远端哈希匹配。仍等待本机运行结果。

下一项具体任务：读取一次标记交接结果，逐项判断坐标、透明、鼠标、层级和退出；前置通过后才连接图像副本及共享重力。W2 第二次样本匹配继续保留为历史真实证据，本轮没有新沙盒拖动验收。下方旧计划不得覆盖此状态。

## 最新 Windows W2 样本复查 · 2026-11-12 16:43

用户再次运行包内只读样本入口并保存结果：本次快照的 5/5 项项目对应、5/5 项图像、5/5 项相对布局均由用户选择“相符”。真实 Shell 视图共 15 项，5 项的项目、图像及原始位置 API 读取成功，包含 2 个非文件系统项目。此范围内的基础样本人工核对通过；不是屏幕逐像素对齐验收。

包输入标识仍为 `869301e44ffd…`，执行后包内探针哈希与交付记录一致；此次直接命令没有记录启动时哈希或退出码。回读时探针进程数为 0，不据此推断所有资源无泄漏。显示条件仍为 Windows 10 x64 19045.6466、session 1、单屏 1920×1080、工作区 1920×1030，窗口 DPI 120 与缩放 API 100% 的差异未解释。

原始报告与边界见 第二次样本复查。上次两项图像不一致和位置未核对的原始记录保留；本次编号只对当前快照有效，不能解释成代码修复了上次问题。原定时器包的拖动“改善或正常”反馈沿用第一次记录，本次没有重测沙盒。输入释放、屏幕坐标转换与精确对齐仍未验证，W2 不笼统标为全部通过。

下一项具体任务：在后续对应验收中补齐输入释放，并查清 DPI/坐标转换前提。本轮停在样本结果归档，不开发透明层。未改源码、未重编译、未重跑自动测试；正式真实桌面模式继续禁用。下方记录为各次历史状态。

## 最新 Windows W2 实测反馈 · 2026-11-12

一次性交接已完成，原定时器包 `199eb44687ae…` 的拖动反馈为“改善或正常”，输入释放选择“未测试/不确定”；沙盒退出码 0。新探针包 `869301e44ffd…` 在 session 1 的可见输入桌面读取成功，探针退出码 0：实际 Shell 视图共有 15 项，5 个样本的项目、位置和图像 API 均成功，含 2 个非文件系统项目。5 项均由用户确认项目对应；图标仅 S3/S4/S5 确认相符，S1/S2 报告不一致，不能据 API 成功覆盖人工结果。

5 项位置均选“未核对”，用户随后说明原始坐标难以理解。数值相对关系为左列从上到下 S1、S2、S4、S3，S5 在 S1 右侧且同 Y；这是待用户对照的解释，不是对齐验证。当前单屏范围 1920×1080、工作区 1920×1030；Shell/诊断窗口 DPI 均为 120，缩放 API 返回 100%，两种读数差异未解释，不据此换算原始坐标。

原始报告、逐样本数据和解释见 实测结果。本次只整理真实结果与说明，未改源码、重编译或重跑测试。W2 仍不整体通过：下一项是让相对位置更容易核对，并定位 S1/S2 图标不一致；输入释放保持未确认，屏幕精确对齐保持未验证。不推进透明层。下方待交接状态是此前历史。

## 当前 Windows W2 样本交接 · 2026-11-12

新探针包输入标识 `869301e44ffd4f30b91183bf65e8c10d092816afabc1b415576a625787d372bc`（119 文件）；默认计数保留，显式 `--samples 3|4|5 --report <新文件>` 打开本机逐项核对窗口。名称/图像不导出，报告按快照 S 编号保留原始坐标、API 结果和人工选择。

本轮 Windows Debug/Release 各 **28/28**，安装、ZIP、哈希及帮助入口已执行；Mac Debug **47/47**，本轮未跑 Mac Release/ASan 或原生 fixture。新包未签名，原包、Mac 基线、恢复确认和历史证据保留。

拖动复查仍绑定原 `199eb44687ae…` 定时器包。已核实登录会话的运行 EXE 哈希匹配，但专项手感反馈待人工确认。当前 SSH session 0、Explorer session 1；真实 Shell 样本尚未读取、图像与项目对应尚未核对，登录显示配置仍未知。仅保留 Shell 视图原始项目锚点，不转换成屏幕坐标，屏幕精确对齐未验证，W2 不整体通过。

**下一项具体任务：** 用户在 Windows 普通 PowerShell 运行已准备的一次性交接脚本，完成短时拖动复查及 S1～S5 样本确认，再读取脱敏结果逐项更新能力。入口、准确包身份及实际命令见 本轮验证记录。本轮不开发透明层、不触发 CI、不改系统设置；正式真实桌面模式继续禁用。下面各轮记录均按其当时状态保留。

## 最新人工复查反馈 · 2026-11-12

用户在 Mac 沙盒修复后回复“可以了你在镜像打开让我测试一下”，随后在普通桌面镜像测试交接后回复“搞定了也没问题”。记录为沙盒问题已由用户报告改善、普通镜像测试由用户报告正常；具体操作未逐项确认，没有独立采集镜像会话计数、资源或 FPS。界面复查只看到普通沙盒窗口，不能作为镜像交互的机器验证。

原始反馈见 人工复查记录。下方“待本机复查／镜像未测”为反馈前历史状态；根因因果仍未最终确认。此次仅补充记录，没有重编译、重新测试或改动功能，正式真实桌面模式和失败候选继续禁用。

## 最新 macOS 沙盒修正 · 2026-11-12：保持按住时的抓取

用户报告普通沙盒长按左键快速移动会脱手，桌面镜像尚未测试。已修正 `mouseExited:` 将边界退出误作取消抓取的路径；松键与失焦仍正常释放。实际用户操作是否由这条路径触发尚未确定，修复后的手感待本机复查。

本次 Debug/Release/ASan+UBSan 自动测试各 **47/47**，显式原生事件处理器合成 fixture 三种配置各 **9/9**，安装包 3 秒冒烟重试 exit 0。不是实际快速鼠标操作验收；历史 29 项镜像 fixture 本次未重跑。开发 app 已更新，构建标识 `b60c9b7e96ad55fa86cd8ac18c0dd84ecba7ce96bac12f9d34fd893c8c62e1cf`，ad-hoc、未公证。详见 问题、修改和真实执行记录。共享核心、Box2D、Windows、桌面镜像、失败候选禁用与恢复状态保持不变。

## 最新修复 · 2026-11-12：拖动时刷新延迟

用户确认卡顿发生在 Windows 本机，主要在拖动鼠标时。原生消息窗口实验复现了反复 SetTimer 延迟刷新；已改为只在周期变化时重设，活跃周期 15 ms、静止 200 ms，失焦停止并忽略残留定时消息。没有更换渲染后端、修改系统定时精度或桌面设置。

新版输入标识 `199eb44687aeac63d44b4f86b76e4e96bc5e1f25b60ccafff79b14dd8f698f9e`（115 个实际输入文件），Windows Debug/Release 各 **20/20**，安装/ZIP/哈希核对通过；macOS Debug **47/47**。这些是本次实际执行结果；本次未跑 macOS Release、ASan/UBSan 或原生 fixture。旧包和历史记录保留。

新包已放入独立 Windows 普通用户测试目录，尚未由用户启动复查；实际拖拽改善、呈现 FPS/GPU、Shell 样本仍未验证，W2 不整体通过。普通 macOS 镜像与已停用候选状态不变，正式真实桌面模式继续禁用。完整命令、原始测量、包身份与启动路径见 定时器修复记录。下方反馈和入口检查记录描述修复前状态。

## 最新人工反馈 · 2026-11-12：操作正常，伴随卡顿

用户对“拖拽、暂停/重置、台球、切换应用和三种退出方式是否正常”回复“正常就是有点卡”。功能组记为用户人工报告正常，流畅度问题保持开放；不把主观卡顿归因于 GPU、物理计算或远控。用户随后确认是 Windows 本机直接操作；并确认主要在拖动鼠标时卡。随后只读进程检查未发现运行中的沙盒，未采集到性能样本；不能据此证明全部资源无泄漏。

原始人工反馈单独保存在 人工反馈记录。尚未收到 Shell 探针输出或真实样本对应，W2 不能整体通过；显示配置、实际 DPI、FPS 和 GPU 数据仍未知。下方“等待人工观察/未启动”是本次反馈前的历史检查，不覆盖此反馈；包、源码和 W1 证据未改。


## 历史 W2 入口检查 · 2026-11-12：收到反馈之前

已复用 d1da3d7b13f9 开发 ZIP，在 Windows 新用户测试目录解压；ZIP、源码清单和两项 x64 EXE 的哈希核验通过。SSH 在 session 0，Explorer 在 session 1。现有远控文字接口只提供远控工具栏，未形成可验证的目标沙盒画面/控件观察；本轮禁止桌面截图，因此未盲目操纵或从 SSH 后台启动程序。普通桌面启动、重力/台球、退出及 Shell 样本核对均未验证，显示配置、实际 DPI 与资源计数保持未知。

已提供普通 PowerShell 从新解压包启动的一组人工操作，见 W2 记录与操作流程，机器数据见 环境记录。当前探针仅输出计数，不具备真实样本对应证据；GetItemPosition 的视图项目左上角也不能当作图像中心或屏幕坐标。本轮未改源码、重跑 W1、改代理/网络、采集桌面数据或新增透明层。以下 W1 通过结论保留，W2 没有升级。


## 最新 Windows x64：真实构建、测试与开发包链路通过

2026-11-11，在用户已授权的 Windows 10 家庭中文版 x64（10.0.19045.6466、Intel i5-10210U）通过既有 SSH 实际执行。VS2022 Build Tools 17.14.37906.3、MSVC 19.44.35230.0、Windows SDK 10.0.26100.0、MSBuild 17.14.60、CMake 4.4.5、PowerShell 7.7.0。VS 安装 exit 0；按授权临时启用既有代理后，已恢复 ProxyEnable=0，代理地址未变，独立检查通过。

共享核心、原生沙盒、只读探针及既有 benchmark 已在 Windows x64 编译；Debug 与 Release 的共享自动测试各 **18/18**，配置、构建、测试、staging 安装和 ZIP 流程 exit 0。没有删除共享测试；macOS 专属测试保持 APPLE 隔离。开发包未签名，两项 EXE 均为 PE x64，dumpbin 仅列出系统 DLL。产物及准确输入标识见 Windows 实测记录 和 [构建入口](windows-build.md)。113 个实际输入文件逐项哈希核验，包含未提交内容，不以 Git HEAD 冒充快照。

实际修复仅限 Windows 探针的 Windows/COM 头文件顺序和 PIDL 指针类型；构建脚本导出日志脱敏、失败状态同步。早先失败安装与两次探针编译失败均保留。最终说明同步后重新生成开发包；原始 macOS 证据、已确认恢复、最近 32 段会话和失败候选禁用状态未改。本轮未改共享核心或根 CMake，未重复 macOS 人工验收或将旧 47/47、29 项 fixture 写成 Windows 结果。

**边界**：原生沙盒界面未启动；登录桌面交互、Shell 数据实机核对（W2）未验证。CI 未触发，无 run ID。Windows 11、透明合成、GPU/呈现性能及真实持久恢复均没有新增验收；正式桌面模式仍禁用，不推进透明桌面层。下面环境不足、工具未安装及 macOS 各轮记录均为历史，不覆盖当前结果。


## 历史：首次 Windows 工具安装与配置尝试（后续已完成安装）

用户已明确授权安装工具并继续测试。已通过现有 SSH 在 Windows 10 x64 运行 PowerShell 7.7.0 和 CMake 4.4.5，两者官方发布 SHA256 校验通过，放在 `%USERPROFILE%/DPPValidation/20261111-toolchain`，未改全局 PATH。已传输排除真实证据的 113 文件构建快照。

构建入口实际执行两次：首次缺 CMake，exit 1；CMake 安装后第二次进入配置，但 Visual Studio 17 2022 生成器找不到 VS 实例，exit 1。VS 官方 bootstrapper 签名有效，但安装器组件下载持续失败/停滞；winget 也曾返回 0x8a15000f。已停止本轮下载进程，残留安装进程数为 0。Windows 系统代理仍保持原来的关闭状态，临时启用既有代理的请求待用户回复。

尚未 Windows 编译、单测、生成 ZIP 或运行桌面程序；不把这些安装/配置检查当作通过。详见 安装与实际配置记录。原始安装日志和输入清单位于本地忽略目录 `out/windows-transfer/diagnostics-20261111.zip`，脱敏失败摘要在 `docs/evidence/windows-install-attempt/`。先前未找到 PowerShell/CMake 的记录为安装前历史。


## 历史：找回并连接既有 SSH 主机

通过聊天《我如何打开win10 的ssh让你连接上》找回既有专用密钥，并以持久 known_hosts 严格校验重新登录成功（exit 0）。Windows 10 build 19045.6466、Intel Core i5-10210U、64 位。先前“只有 macOS、没有 Windows 目标”的判断遗漏了其他聊天中的 SSH 连接，不再作为当前结论。

当前阻塞改为构建工具链：只读检查显示 PowerShell 5.1；cmake、ctest、pwsh、cl、git 不在 PATH，标准位置未发现 VS Installer/vswhere 或 Windows 10 SDK Include。该结果不证明任何自定义位置都没有工具，但目前尚无可用的 VS2022 x64 构建环境。没有上传源码、安装软件或修改远端设置。另一个项目历史上使用 CI 产包后在此机运行；该项目的远程仓库授权不自动用于本项目。Windows 编译、测试和开发 ZIP 仍未执行；SSH 登录不等于登录桌面交互验收。


## 较早检查：执行环境阻塞（已由上方更正）

已核对现有源码、预设与工作流，准备隔离输入快照的 `scripts/build-windows.ps1` 并接入现有 CI 的 Windows 分支。当前仍只有 macOS arm64；无项目 remote、无有效 HEAD，无已授权 Windows 远程执行目标。本轮未上传、推送或触发 CI。Windows 编译、单测、ZIP、原生运行、登录桌面交互及 Shell 实机核对全部未执行。脚本未经过 PowerShell 运行验证；不能写为 Windows 链路通过。

实际本地检查及停点见 [Windows 构建说明](windows-build.md) 和 环境检查记录。等待 Windows 执行环境或远程授权，不推进透明桌面层。macOS 原交付、已停用候选、32 段会话和恢复确认保持不变；下文平台测试数字都是历史结果。


## 当前工程收尾：失败候选已停用

用户辅助隐藏候选在本机本次人工观察中未通过：调整系统设置后原图标一直存在，无证据指向某个点击动作；不是机器检测，也不推广为所有 macOS 均不支持。关联会话 `A2791BEC-3FEF-4874-A425-134FA4FF619A` 与原始证据保持不变。用户已确认恢复本次改动，提醒已清除；该确认仍为用户报告。

普通桌面镜像预览继续用于开发，原图标保留，可能重复显示。检查面板的实验入口仅显示失败原因，旧准备/返回处理器及原生启动层均拒绝辅助实验，不准备快照、不创建窗口、不捕获输入、不新增恢复提醒。更换系统或设备不会自动开放。历史会话、脱敏导出、手动恢复说明保留；最近 32 段保存格式不迁移，未知原值不提供恢复成功确认，原本隐藏时不要求改成显示。

正式真实桌面模式仍未完成且不可启动：无成立的无双影与自动接管方案，无真实平台持久恢复能力。Windows 与 GPU/呈现性能本轮没有新增验证。下面所有早期“待验证”、操作步骤、46/46 和 25 项 fixture 均是历史记录，不是当前启动说明或本轮结果。本轮命令、构建标识及检查结果见 收尾验证记录。


## 镜像重力预览增量（2026-11-11）

- Debug、Release、ASan/UBSan：各 **41/41**。原始 ctest 结果在 `evidence/tests-{debug,release,asan}-mirror.txt`。单测不读取真实 Desktop、不申请权限、不创建窗口。
- 另行显式运行 `build/macos/platforms/macos/Release/mirror_native_smoke <new-json-path>`（未注册为 ctest）：22 检查通过，含缺缓存、场景/缓存/窗口/计时器 4 阶段失败、原生静态与播放、重置、显式交互焦点、控制面板焦点释放、10 次重复会话、关闭后无可见预览窗口。`evidence/mirror-native-fixture-final.json`。故障注入宏仅用于该 fixture，正式开发 .app 没有此入口。
- fixture 的坐标/图像全部合成，JSON 明确 `synthetic_fixture`；不是普通沙盒测试，也不是实际 Finder 项目的桌面验收。早期 `mirror-native-fixture-1.json` 顶层虽已标为 fixture，其项目来源字段仍误用 Finder/manual 文本；保留历史，使用最终报告（字段已纠正）。
- 最终 .app 正常退出/重新启动并预检：OSStatus 0、无旧快照、原生入口与禁用门槛可用。`evidence/app-mirror-restart-preflight.json`；构建及签名见 `evidence/mirror-build-manifest.json`。
- 追加实机证据：`evidence/app-mirror-three-items-run-1.json`，最终构建 49ba0570…、PID 16020。用户选择 3 项，28 次 Finder 查询成功、0 失败，系统图标 3/3；三个采样误差为 0.976 / 2.251 / 3.771pt，均在 12pt 内。新增独立项为选择编号 #2、dpos=(1542,194)，其余为 #3=(1671,928)、#1=(102,89)。镜像会话报告 3 bodies / 3 images / 3 layers、144 次 physics 计时样本，累计 starts=2 / closes=2；最后一次外部应用切换触发关闭，关闭后预览 windows/bodies/images=0、输入已释放。该历史报告 pointer_events=0、resets=0，仅凭它不能宣称实际抓取抛掷、暂停或重置通过；后续人工验收与重启证据另列。
- 用户随后明确确认三项副本位置正确且看到下落，另存 `evidence/app-mirror-three-items-run-1-human-confirmation.json`。后续交互与重启验收见下一条。关闭后顶层 coordinate_validation/preview_acceptance 被清除，不抹去原始证据；启动门槛要求采样与对齐按钮验收均通过，这只是代码门槛推断，不代替副本目视反馈。
- 同一最终构建 49ba0570… 与 CDHash 保持一致，进程由 PID 16020 变为 20324；重启后再次读取三项，Finder 查询 28 成功/0 失败，三项采样误差为 6.239 / 1.004 / 5.500pt，均符合 12pt 门槛。新进程记录 starts=2 / closes=2，最后一次外部切换关闭后预览 windows/bodies/images=0，输入已释放。
- 用户对完整测试组明确回复“全部完成且正常”，据此将显式交互、抓取抛掷、面板切换释放交互、暂停、重置、手动关闭及退出后桌面正常点击记为本机三项场景的人工实测通过。
- 当前导出只保留最后一段会话，该段 pointer_events=0、resets=0；这些计数不能独立证实人工确认的交互和重置，也不能推断未保留会话发生了什么。人工确认与机器记录分开保存，未改写原始诊断。原始诊断 `evidence/app-mirror-restart-interaction-run-2.json`，对照与人工验收 `evidence/app-mirror-restart-interaction-acceptance.json`。
- 未运行桌面预览性能长测，不重解释旧无效 600 秒沙盒测量。


2026-11-11，本机 Apple M1 Pro / macOS 15.7.5 / Apple Clang 17 (Xcode 26.3)。没有运行真实桌面状态破坏测试，没有移动/删除用户文件，没有上传桌面数据。

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

## 第三轮实际执行：应用内 Finder Automation

读取 AGENTS.md、README、status/testing/platform-capabilities/desktop-integration/performance 及现有 macOS 入口、探针、descriptor、窗口、plist、构建和测试。保留整个未提交工作树、vendored Box2D、图像缓存、历史证据与 Windows 未验证状态。

所有 CMake 命令使用工作区 `.tools/bin`，项目代码 Werror。初版 33/33；真实授权后暴露旧列表 ordinal 类型错误，修复后为 34/34；最终加入 (-1,-1) 和重合样本门槛后为 **35/35**。新增测试全部 mock / 内存 descriptor；不访问真实 Desktop、不发 Apple Events、不显示窗口、不申请权限。真实权限流程单独由用户操作。

| 实际命令 | 结果与证据 |
|---|---|
| `cmake --preset macos -DDPP_WARNINGS_AS_ERRORS=ON` | exit 0，SDK 26.2 / minos 13.0 |
| `cmake --build --preset macos-release -j8`；`ctest --preset macos-release --output-log docs/evidence/tests-release-automation-final.txt` | exit 0 / 0，35/35 |
| `cmake --build --preset macos-debug -j8`；`ctest --preset macos-debug --output-log docs/evidence/tests-debug-automation-final.txt` | exit 0 / 0，35/35 |
| `cmake --preset macos-asan -DDPP_WARNINGS_AS_ERRORS=ON`；`cmake --build --preset macos-asan-debug -j8`；`ctest --preset macos-asan-debug --output-log docs/evidence/tests-asan-automation-final.txt` | exit 0 / 0 / 0，35/35，ASan+UBSan |
| `desktop_probe --output "$PWD/docs/evidence/probe-default-automation.json"` | exit 0；directory/finder/window_test 均 not_requested；无提示默认路径保留 |
| `xcrun clang-format --dry-run --Werror`（本轮 C++/ObjC++ 改动文件） | exit 0；排除 third_party |
| `codesign -dvvv --entitlements - APP`；`codesign --verify --strict --verbose=2 APP`；`plutil -p APP/Contents/Info.plist`；`xcrun vtool -show-build APP/Contents/MacOS/physics_sandbox` | 实际产物核验；初版原始输出 `evidence/app-identity-automation-initial.txt`，未用源码配置替代 |

新增自动化涵盖：-1744/-1743/noErr/超时/目标不可用映射，默认不执行检查/申请，无明确 consent 不进入申请，重复操作只有一在途，关闭重开仍保留槽位、迟到结果丢弃、退出不能重启，权限通过不等于读取成功，malformed/minimal-query 失败、部分项目失败、URL 身份变化、未知矩形不得标记已验证，脱敏输出无名称/路径，逻辑 points/Retina/原点负例，预览状态重复清理，以及 `kAEAll` 必须使用 `typeAbsoluteOrdinal`。预览状态测试不是原生 overlay 资源/输入穿透的实机验证。

### 已发生的 GUI 与用户授权交互

原生应用由 CUA 通过确切 Release `.app` 路径启动（Launch Services 上下文，不用 Terminal/osascript 的权限结果替代）。启动沙盒正常；Space 暂停；应用菜单与 DPP 状态菜单复用“桌面接入检查”。首次开面板全部 not_requested / not_checked，读布局、采样、预览禁用。点击检查无提示返回 -1744。用途说明 sheet 包含准确文字与权限范围，用户随后亲自继续、看到系统提示并允许（本轮用户回复确认）；agent 没有点击系统授权按钮。

初版 PID 1921 的导出包含 permission_request ask=true/status=0，其后同进程 version 查询成功、列表失败 -1700。用户曾选择非直接子项或超过三项，面板拒绝后重新选定；实际失败单独记录，未伪造布局成功。修复全选 ordinal 后重启 `.app`，无提示预检仍为 0；初版与修正版构建证据分开。

用户使用默认保存位置在 Desktop 新建了三份脱敏 JSON，agent 仅按诊断文件模式或已经观察到的确切文件名读取，并复制到 `docs/evidence/`，未移动/删除原件，不导出其他桌面内容。原始提示/允许事实来自用户确认，API 不声称能观察系统提示可见性或实际按钮选择。

修正版 PID 3617 实际读取两项：16 查询成功、0 失败、URL 复查一致、系统图标 2/2，真实 dpos 为 QDpt (-1,-1)，pbnd 为 qdrt。原始报告 `evidence/app-layout-automation-ordinal.json` 记录一项指针采样不匹配、coordinate_validation=unverified、preview_opened=false。没有人工位置对齐成功，原生预览资源清理仍未实测。

最终构建增加 `macTopLeftSampleSetUsable` 拒绝当前单屏域外/重合样本、清晰失败状态和 Documents 默认导出位置；新增回归保证 (-1,-1) 仍能作为原始数据保留，但不能开启校准。以上真实查询记录来自 ec80ec3a 构建，不被改名为最终 1488cfde 构建结果。最终三配置 35/35、格式检查通过，已 `cmake --install build/macos --config Release --prefix "$PWD/dist/macos-dev"`；实际安装包签名验证 exit 0，身份见 `evidence/app-identity-automation-final.txt`。

默认 CLI 在受限 shell 的 screens=[]，只用它验证默认不访问受保护功能，不用于 GUI/屏幕验收。本轮原生应用检查没有桌面截图、系统提示代点、Finder 设置修改或真实图标移动。

最终安装包另在登录图形会话从 `/private/tmp` 运行 `--measure-seconds 600 --report .../docs/evidence/native-render-600s-automation-final.json`，exit 0、600.003894208 秒、completed=true，但 workload_valid=false / focus_lost=true / user_interactions=0；全部 gravity。**完整时长已执行，有效标准负载长跑未通过**。未仅凭退出码判断，详见 [性能记录](performance.md)。

最终安装包 1488cfde 在 600 秒测量结束后，再经 Launch Services 正常启动并做原生 UI 回归：Gravity 50 → Billiards 51 → Gravity 50、R 重置、Space 暂停；首次面板仍 not_checked / not_requested，点击无提示检查得到 OSStatus 0。未在最终进程重新选择文件，因此不将先前 ec80ec3a 的字段报告改作最终读取证据。最终面板已留给用户。见 `evidence/app-ui-automation-final.txt`。

## 第二轮实际执行

先读取适用 AGENTS.md、README 与已有五份状态/测试/能力/性能/上游审计文档，均存在。工作树全部为未提交文件，本轮未 reset、删除既有成果、提交、推送或触发 CI。macOS GUI 可执行；未发现 Windows 实机/VM或已授权远程环境。

所有以下 CMake 命令在仓库根目录执行，`PATH="$PWD/.tools/bin:$PATH"`。项目警告以错误处理（第三方除外）。本轮新增 6 个 macOS 离线解析/报告测试及 1 个回调统计测试，旧 17 个保留，当前共 **24 个**。

| 实际命令 | 配置 / 退出码 / 结果 |
|---|---|
| `cmake --build --preset macos-release -j8`；`ctest --preset macos-release`（改动前） | Release，0 / 0，旧基线 17/17 |
| `cmake --preset macos -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DDPP_WARNINGS_AS_ERRORS=ON` | 0；修复默认版本设置过晚后显式更新旧 cache |
| `cmake --build --preset macos-release -j8`；`ctest --preset macos-release --output-log docs/evidence/tests-release-integration.txt` | 最终 Release，0 / 0，**24/24** |
| `cmake --build --preset macos-debug -j8`；`ctest --preset macos-debug --output-log docs/evidence/tests-debug-integration.txt` | Debug，0 / 0，**24/24** |
| `cmake --preset macos-asan -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DDPP_WARNINGS_AS_ERRORS=ON`；`cmake --build --preset macos-asan-debug -j8`；`ctest --preset macos-asan-debug --output-log docs/evidence/tests-asan-integration.txt` | 0 / 0 / 0，**24/24**，AddressSanitizer + UBSan；不声称 LeakSanitizer/GUI 长跑 |
| `desktop_probe --desktop-files --finder-layout --output "$PWD/docs/evidence/macos-capabilities-final-integration.json"` | Release，**exit 3**；Desktop 与图像 11/11；Finder 需要用户授权 -1744，详见 JSON。首次记录 `macos-capabilities-integration.json` 也保留 |
| `desktop_probe --window-test --output "$PWD/docs/evidence/macos-window-integration.json"` | Release，0；两个自有窗口约 1 秒。普通窗口在测试层上、前台应用不变、非 key、关闭后均不在列表 |
| `physics_sandbox --measure-seconds 20 --report "$PWD/docs/evidence/native-render-smoke-integration.json"` | 缓存优化前 Release，0，completed=true |
| `physics_sandbox --measure-seconds 20 --report "$PWD/docs/evidence/native-render-cache-integration.json"` | 缓存优化后 Release，0，completed=true |
| `physics_sandbox --measure-seconds 600 --report "$PWD/docs/evidence/native-render-600s-integration.json"` | 缓存优化后、回调统计修正前 Release，0，600.002 s；发现闲置唤醒混入间隔，原始数据保留并限定解释 |
| `physics_sandbox --measure-seconds 600 --report "$PWD/docs/evidence/native-render-600s-final-integration.json"` | 最终统计口径，exit 0，但 73.093 s 时正常退出、completed=false，期间变为台球；**未通过完整长跑** |
| `physics_sandbox --measure-seconds 20 --report "$PWD/docs/evidence/native-render-20s-final-integration.json"` | 最终 Release，exit 0、20.003 s；completed/workload_valid=true，无人工交互/失焦；功能短测，非最终长跑 |
| `xcrun vtool -show-build build/macos/platforms/macos/Release/physics_sandbox.app/Contents/MacOS/physics_sandbox` | 0，实际 LC_BUILD_VERSION minos=13.0 / SDK=26.2；不是 macOS 13 运行验收 |
| `xcrun clang-format --dry-run --Werror`（本轮改动 C++/ObjC++ 文件） | 0，格式检查通过 |
| `cmake -S . -B build/macos-default-check -G Ninja -DDPP_BUILD_TESTS=OFF -DDPP_BUILD_APPS=OFF -DDPP_BUILD_BENCHMARKS=OFF` | 0；全新 cache 自动默认部署目标 13.0，不是仅靠显式参数掩盖默认错误 |
| `cmake --install build/macos --config Release --prefix "$PWD/dist/macos-dev"`；从 `/private/tmp` 执行安装包 `physics_sandbox --smoke-test` | 0 / 0；输出 `sandbox_smoke=completed`，资源与退出正常，见 `smoke-installed-integration.txt` |
| `cpack --config build/macos/CPackConfig.cmake -C Release -B dist -D CPACK_PACKAGE_FILE_NAME=DesktopPhysicsSandbox-0.1.0-Darwin-integration` | 0；独立新 ZIP，保留第一轮归档；安装包可运行不等于 Developer ID 签名/公证 |
| Windows / Linux / x86_64 / GitHub Actions / 第一轮 headless benchmark | **本轮未执行**；保留原记录，不升级验证状态 |

表中 `desktop_probe` 完整路径是 `build/macos/platforms/macos/Release/desktop_probe`；`physics_sandbox` 是 `build/macos/platforms/macos/Release/physics_sandbox.app/Contents/MacOS/physics_sandbox`。GUI 和读取真实 Desktop 的显式探针在正常登录图形会话执行，受限 shell 看不到 WindowServer 的失败不能当作平台能力结论。

开发中暴露的失败也保留在结论中：Finder 预检失败是未解除的能力阻塞；最初新代码触发 Werror（GNU 条件表达式、多字符常量 NSNumber 推导），已显式修正；新增测试误用不存在的坐标 getter，已改用共享 SceneToPhysics 并通过测试。默认部署目标初始实为 15.7，已在最终二进制核验 13.0。

## 第二轮 GUI 与无权限单元测试

本轮通过原生 UI 控制只操作合成沙盒，观察：50 图标堆叠、Space 标题 PAUSED/恢复、顶部图标拖拽抛出后离开堆栈且 awake=6、R/B 后进入 51 刚体台球、900 ms 蓄力松键 Shots 从 0 变 1 且白球和球阵运动、20 次 B 切换保持响应、P 面板有 6 个缓存图像和 114 个复用图层（跨模式池），Esc 后应用 isRunning=false。缓存改动后图像与圆形裁剪正常。没有操作真实桌面图标。

新增自动化覆盖：拒绝缺失/错误长度/文本/无限值坐标；保留负坐标和小数；QuickDraw 点和矩形顺序；Finder property/container descriptor 结构；新建 JSON 不覆盖已有证据；空样本用 null、非空中位数/P95/P99；不把重置、暂停/恢复、休眠抓取唤醒的 200 ms 等待计入活动回调间隔。所有单测不访问 Desktop、不发 Apple Events、不申请权限、不显示窗口。

`--window-test` 是独立显式测试，不属于 ctest。它没有验证逐像素合成、实际鼠标穿透、Spaces/Mission Control、全屏、Dock、锁屏或拓扑变化。没有原图标隐藏或真实恢复能力。

## 第一轮实际命令与结果（未重跑项不得视为本轮结果）

使用本项目 `.tools/bin` 中 CMake 3.31.6 / Ninja 1.11.1.3。配置以 `DPP_WARNINGS_AS_ERRORS=ON` 检查项目代码，第三方警告不升级。

| 命令 | 真实结果 |
|---|---|
| `cmake --preset macos` | 成功；最初发现离线 GoogleTest 本地路径与 URL 列表混用，已改成优先使用单一本地 archive，SHA256 仍校验 |
| `cmake --build --preset macos-debug`、`ctest --preset macos-debug` | 构建与测试成功，见 `evidence/tests-debug.txt` |
| `cmake --build --preset macos-release`、`ctest --preset macos-release` | 构建与测试成功，见 `evidence/tests-release.txt` |
| `cmake --preset macos-asan`、`cmake --build --preset macos-asan-debug`、`ctest --preset macos-asan-debug` | AddressSanitizer + UBSan 测试成功，见 `evidence/tests-asan.txt`；项目核心与 Box2D 均插桩；未宣称 macOS LeakSanitizer 或 GUI 长跑通过 |
| `cmake --preset macos-x86_64 -DDPP_BUILD_TESTS=OFF`、`cmake --build --preset macos-x86_64-release` | x86_64 交叉编译成功，`file` 确认为 Mach-O x86_64；没有运行 |
| `desktop_probe`（默认参数） | 在登录 GUI 环境中得到 1 屏、1728×1006 points、2×；不申请权限、不读桌面文件 |
| `physics_sandbox --smoke-test` | GUI 启动后 3 秒正常结束，输出 `sandbox_smoke=completed` |
| `./scripts/benchmark.sh` | 18 组合：50/200/500 × fall/stack/throw/sleep/pause/switch；原始计时在 `evidence/benchmark-macos-arm64-release.txt` |
| Windows MSVC、Linux、GitHub Actions | 当前未执行；仅有源码/构建/CI 配置 |

受限 shell 最初看不到 WindowServer，探针返回 screens=0，首次 UI 启动失败；随后在正常登录图形会话成功执行。后续入口增加无屏幕诊断返回，不能把 screens=0 当作用户实际没有显示器。

## 核心自动化覆盖

GoogleTest 当前涵盖坐标往返/负屏幕原点/backing pixels，异常 dt/配置，重力边界和休眠，50 物体堆叠有限数值与低速稳定，mouse joint 抓取/速度上限/取消，鼠标历史过期，暂停取消拖拽及不补算暂停时长，20 次模式切换/对象数，进球只计一次，白球安全重生与无空位保持禁用，蓄力取消/击球/不动鼠标仍更新视觉。

会话测试使用 mock backend：8 个启动阶段分别失败、重复启动、幂等停止、恢复失败后显式 RecoveryFailed/锁不释放、重试成功、异常不越界。权限拒绝和助手未 ready 仅由相应阶段返回失败模拟。**尚无 journal 文件格式、损坏/过期/冲突文件、PID 复用或独立助手实现，因此未声称这些测试通过。**

## 第一轮 macOS 实际 GUI 检查

使用原生 UI 自动化只操作沙盒窗口：

- 50 个合成图标自由落体/堆叠显示，最终 0 awake。
- Space 暂停状态出现在标题；恢复、R 重置。
- B 切换到 51 刚体台球，按住左键再松开后 Shots=1，白球/目标球发生运动。
- 将堆叠顶部图标向上拖动再释放，图标离开堆栈并飞行；awake 数量随之变化。
- 发送 20 次 B 切换，保持响应；P 统计面板打开。
- Esc 退出后应用列表确认进程不再运行。

验证发现并修复：Mac 每 tick 读取全局 pressedMouseButtons 会在鼠标松键消息处理前取消正常击球，改为原生 mouseUp、离开内容区及失焦取消。核心单测不覆盖系统事件排队，因此保留此 GUI 回归步骤。

这些验证不是：真实图标枚举、Finder 布局、原图标隐藏、桌面层级、Spaces/Mission Control、Dock/菜单栏交互、锁屏/休眠、屏幕拓扑变化或崩溃恢复验收。

## 后续平台验收清单（未执行）

1. Windows 登录桌面 W2（构建/共享单测链路已通过）：沙盒交互与退出、Shell 项目/图像/位置与 Explorer 逐项比对，包括公共桌面、回收站、重定向目录；不写原坐标。
2. 最小透明 GPU 合成原型：逐像素透明、桌面层级、Win+D、任务栏、应用切换、输入捕获，失败不隐藏任何图标。
3. 持久恢复实现后，在明确授权隔离环境测试主进程结束、双进程结束后重启、旧 session/PID 冲突、日志损坏、助手失败；普通单测永远用 mock。
4. 两端真实模式能力成立后：进入/退出与切换各 20 次，空桌面/大量图标/拒绝权限/多显示器、DPI/分辨率、锁屏/休眠；Windows Explorer 重启、macOS Spaces/Mission Control/全屏。
5. 本轮补充原生 1100×700 points / 2× 的十分钟 CPU 与内存记录；1080p 等效工作区、实际呈现帧/GPU 和空闲功耗仍需独立工具验收。
