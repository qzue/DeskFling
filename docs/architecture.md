# 架构和坐标契约

## 本轮可运行路径

`apps/physics_sandbox/CMakeLists.txt` 选择 `platforms/macos/sandbox.mm` 或 `platforms/windows/sandbox.cpp`。两个 host 直接调用 `core/scene/Playground`，同一线程推进输入、物理、规则、绘制数据；系统窗口/资源不进入共享核心。

- `PhysicsWorld`：RAII 持有 Box2D world，内含边界、刚体、查询、mouse joint；销毁 world 一次性释放引擎对象。BodyIndex 只在所属 mode/world 内有效，不跨模式保存。
- `GameMode`：模式持有自己的 world；GravityMode / BilliardsMode 只消费 SceneSetup、InputEvent 并生成 DrawList。
- `Playground`：固定步进、暂停、重置、模式替换、退出请求、计时。先成功构造新 mode 再替换旧 mode；两套 world 的短暂并存只属于内存场景，不是两个桌面会话。
- `DrawList`：variant 结构化指令，缓冲区容量复用；TextureId 引用 host 资源，不含文件名、系统句柄或 Objective-C 对象。
- macOS：AppKit NSView/NSWindow；ARC；缓存 6 个合成图像和 CALayer/CAShapeLayer/CATextLayer 池，关闭隐式动画；仅更新已有图层。文本层在图形之上。无每帧文件读取。
- Windows：Win32 消息循环和 Direct2D HWND render target、DirectWrite；普通不透明开发窗口，资源失效时重建 target/brush。合成形状，尚未加入真实图标纹理缓存；没有把 HWND target 当作透明合成完成。
- `PerformanceMonitor`：输入、physics、rules、draw、submit、活动帧回调间隔各自固定容量 ring。内存只在构造时申请；统计百分位仅请求时排序。数组不放 Windows 默认 1 MB 栈。

## 坐标

统一逻辑 scene：工作区左上角 `(0,0)`，X 右、Y 下，单位 points/DIP。`core/scene/coordinates.*` 集中转换：

| 来源 | 到 scene |
|---|---|
| Windows 物理屏幕 pixels | `(pixel - workArea.topLeft) / (dpi/96)`；负屏幕坐标合法 |
| macOS AppKit 全局 points，Y 向上 | `x-visibleFrame.x`、`visibleFrame.maxY-y` |
| macOS flipped view 本地 points | 与 scene 同向；窗口沙盒原点是内容视图左上角 |
| macOS backing pixels | scene points × backingScaleFactor；不再重复参与物理缩放 |
| Box2D meters，Y 向上 | `x/100`、`(sceneHeight-y)/100` |

速度只缩放/翻转 Y，不加原点。角度 scene = -physicsAngle。图标种子使用图像中心与尺寸，标签不参与碰撞。Shell `GetItemPosition` 是 view 坐标/项目位置，未验证图像矩形前不得直接当作图像中心。

## 步进和输入

固定主步 1/60s，Box2D 子步 4；累计器每帧最多 4 个主步，dt 最大计入 .25s，多余积压丢弃。NaN/Inf/负 dt 不推进；配置范围和场景/图标尺寸验证避免非法值进入引擎。暂停时取消拖拽并清空累计器；恢复不补算暂停时间。

抓取使用 mouse joint，最大力随质量变化。释放速度按最近 .1s 样本线性拟合，替换而不重复相加，引擎速度和抛掷速度分别有上限。失焦/捕获丢失/鼠标离开 Mac 内容区取消约束。Mac 不轮询 pressedMouseButtons 提前取消正常排队的松键事件；该问题已在 GUI 验证中发现并修复。

Mac 活动时 NSTimer 目标 60Hz，按提交耗时调整下一次触发；暂停/全部睡眠降至 5Hz且不步进、不重复渲染；非活动停止 timer。Windows WM_TIMER 空闲降频、失焦停止。这里的计时器回调不是显示器实际呈现帧数。锁屏/休眠/DPI 桌面会话验收仍待做。

## 桌面与恢复边界

`desktop_probe` 是独立只读程序，无 `DesktopAdapter` 生产实现。两端没有可启用的真实桌面入口。macOS 默认不读 Desktop；可选目录读取不等于 Finder 布局；AXIsProcessTrusted 不触发申请。

`SessionCoordinator` 已实现 Idle → Probing → Preparing → Active → Restoring → Idle / RecoveryFailed 和故障回滚。SessionBackend 是为模拟测试和将来平台差异保留的唯一恢复接口；尚无磁盘日志、单实例 OS 锁、进程身份令牌或看门狗实现。不得把 mock 测试称为跨进程恢复通过。

真实 adapter 未来必须先完成：单实例/残留验证 → 探针 → 快照 → 原子持久化变更意图 → 场景 → 必要助手 ready → 最后系统变更。仅恢复自身更改，比较后恢复；失败留诊断状态。两个进程同时结束无法保证即时恢复；必须下次启动/独立恢复入口。模式切换不重新激活桌面。
