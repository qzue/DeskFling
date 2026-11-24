# 上游源码审计（2026-11-11）

仓库：https://github.com/liuxiaogang-com/desktop-icon-billiards 。审计版本：`187e5e89f6ab76ede0d1fe22246477b0cfaf0541`。已实际 clone 并阅读源文件，没有运行上游程序，也没有改动用户桌面。当前工作区是独立新 C++ 项目，并非上游 Python 仓库；原有 Claude 文件保留并增量修正。

实际 `LICENSE` 为 MIT，Copyright (c) 2025-2026 LXG。许可证已保存到 `third_party/notices/desktop-icon-billiards-LICENSE`。以下判断来自该 commit 的代码，未把 README 的支持声明当作本项目验收。

| 实际路径 / 符号 | 观察与复用判断 | 原因 / 风险 |
|---|---|---|
| `app.pyw::main` | 仅参考。入口按 `--watchdog` 分派恢复进程，否则加载 Tk GUI | 本项目原生 C++/Objective-C++，不引入 Python 运行时 |
| `requirements.txt`, `pyproject.toml` | 不采用。Pillow 13.0.0、pywin32 312、comtypes 1.4.17；Tk 来自 Python | 图形/COM 改为平台 SDK；不复制依赖栈 |
| `dib/shell.py::ShellDesktop.__init__`, `items` | 仅参考 API 路线。Shell Windows `FindWindowSW(SWC_DESKTOP)` → IServiceProvider → IShellBrowser → IShellView → IFolderView；枚举视图 PIDL、`GetItemPosition`、解析名称身份 | 已独立编写 C++ COM 只读探针。视图枚举比只扫 Desktop 文件夹覆盖完整；位置仍需图像矩形/DPI 实测 |
| `dib/shell.py::ShellDesktop._invoke/get_flags/set_flags` | 不采用 Python 手工 vtable 调用 | C++ SDK 接口可直接类型检查；本期不写排列设置 |
| `dib/shell.py::ShellDesktop.move`, `LayoutSession.apply` | 不采用。`SelectAndPositionItems` 批量移动真实图标，旧路径限制偏移范围 | 本产品禁止物理循环移动真实图标；只模拟副本 |
| `dib/overlay.py::artwork`, `ImageFactory` | 仅参考。`SHCreateItemFromParsingName` + IShellItemImageFactory 获取位图，转换 RGBA、释放 HBITMAP | 本项目 Windows 探针通过父 Shell folder/PIDL 构造 image factory，无名称导出；缓存/alpha 正确性仍待原生渲染验证 |
| `dib/overlay.py::desktop_listview`, `item_rects`, `map_rect` | 不采用跨进程内存路径。查找 `SHELLDLL_DefView/SysListView32`，向 Explorer 分配内存，发送 ListView 消息读取图标/标签矩形，再映射坐标 | 系统实现细节和外部进程内存操作，不是稳定公共契约。不能将视图位置直接冒充图标图像中心 |
| `dib/overlay.py::BilliardsOverlay.show/close` | 仅参考安全顺序。先记录 `hidden_listview`，再 `ShowWindow(SW_HIDE)`，关闭时显示 | 需要隔离兼容模块、验证原可见性、宿主身份、HWND 复用和用户后续更改；本期没有移植隐藏功能 |
| `dib/overlay.py::BilliardsOverlay.__init__/show/table_window/stack_table_windows` | 仅参考层级。以 DefView 为父窗口创建 layered child；`WS_EX_NOACTIVATE`、点击穿透，分别设置球/球杆/桌面层顺序 | 不证明 Windows 全版本、Win+D、全屏、宿主重启可靠；本期 Win32 普通调试窗口不视作等价实现 |
| `dib/overlay.py::LayerSurface.present`, `sprite_frame`, `build_templates` | 不采用最终渲染路径。Pillow 图像、复用 DIB/DC、CPU 转换/拷贝，再 `UpdateLayeredWindow`；缓存部分模板 | 有资源缓存思路，但不是已验证 GPU 合成路径。本期 Mac 使用缓存 CALayer；Windows 普通窗口 D2D，未来透明 DComp 独立验证 |
| `dib/overlay.py::TablePointer.update`, `BilliardsOverlay.update_pointer` | 仅参考输入状态。轮询光标/键状态，用前台窗口和类名判断是否处于桌面 | 存在焦点/宿主判断兼容性；本期使用普通原生窗口鼠标事件及失焦取消 |
| `dib/table.py::BilliardsMotion.step/_walls/_collisions/_collide` | 玩法仅参考；求解器不采用。手写圆碰撞、边界、速度阻尼和按速度细分的积分 | 本项目 Box2D 3.1.1 负责接触和鼠标约束，不重写碰撞求解器 |
| `dib/table.py::shoot/_in_pocket/_auto_place/place_white/reset` | 仅参考简化规则：蓄力、六袋、目标计数、白球入袋再放置 | 本项目独立 Box2D 规则实现；没有复制竞技规则/辅助轨迹预测。白球找不到空位时保持禁用，不能重叠重生 |
| `dib/shell.py::atomic_json`, `LayoutSession.begin/restore` | 仅参考。临时文件+flush/fsync+replace，version/session_id/owner_pid/flags/geometry/items；overlay-only 不重排图标 | 写日志顺序有价值；恢复隐藏 HWND 只验证类名，未充分证明进程/窗口身份、用户可见性冲突；非 overlay 路径会恢复所有坐标，不适合本产品 |
| `dib/watchdog.py::main` | 仅参考。OpenProcess + WaitForSingleObject(400ms)，session nonce 校验、ready/error/recovered 文件；正常结束由 journal active 控制 | 已持有进程句柄后不受 PID 重用影响，但 OpenProcess 前需启动身份验证；损坏日志会继续等待。主进程与助手同杀无法即时恢复。本期只做模拟状态机，未移植助手 |
| `dib/ui.py::start/spawn_watchdog/poll/restore/close` | 仅参考。准备 overlay、等待 matching session ready、超时回滚，Esc/全局热键退出 | 可见失败状态需要保留；没有把线程 ready 当作跨进程恢复已验证。本期无系统修改，不启动助手或全局热键 |
| `dib/instance.py::acquire_instance/claim_instance_mutex` | 仅参考 Windows 命名互斥锁和激活旧实例的思路 | 真实桌面会话单实例需与残留恢复一起验证，当前只有模拟锁冲突测试 |
| `dib/display.py::dpi_context/monitors`, `BilliardsOverlay.draw` | 仅参考。物理显示器/DPI/工作区比较，0.5s 检查变化后停止；每帧检查 geometry | 不足以证明 Explorer 宿主重建、锁屏/休眠恢复；本期两个探针检测显示器数，真实模式整体禁用 |
| `dib/wallpaper.py::primary_wallpaper`, `dib/art.py::table_art` | 不采用。读取壁纸文件并生成桌布背景 | 禁止用固定壁纸遮盖桌面来替代真实桌面接管；本期沙盒清晰标注合成资源 |
| `build.ps1`, `DesktopIconBilliards.spec`, `tools/gen_version_info.py` | 仅参考交付检查。Python 虚拟环境、PyInstaller 单文件 EXE、版本资源、SHA256 | 改用 CMake/原生 .app/便携目录，暂无签名公证 |
| `tests/test_table.py`, `test_art.py`, `test_update.py`, `tools/verify_desktop.py` | 测试场景仅参考。涵盖台球规则、渲染资产、更新数据；另有真实桌面验证工具 | 没有执行危险桌面验证，不复制联网更新。新增 GoogleTest 和模拟会话测试；CI 编译不替代桌面实测 |
| `dib/update.py` | 不采用 | 自动更新/联网不在范围 |

结论：本轮没有直接复用/逐行移植上游代码或资产。可用的是公共 Shell 枚举路线、规则范围和恢复风险清单。上游审计已完成，但 Windows 桌面路线仍需本项目独立原型及实机证据。

## Box2D 版本核验

已有 vendored 代码与 `v3.1.1` commit `8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3` 的 `include/`、`src/`、顶层 CMake 逐文件比较无差异。读取了该 tag 的 [官方 simulation.md](https://github.com/erincatto/box2d/blob/v3.1.1/docs/simulation.md) 固定步长/子步/Mouse Joint 章节，并以本地 `box2d.h/types.h` 核查签名。使用 `b2World_Step(world, 1/60, 4)` 与 `b2MouseJointDef`；在线最新版文档已是不同版本，不作为本项目签名来源。

复核命令：`git clone --depth 1 <repo> /tmp/<audit-dir>`、`git rev-parse HEAD`；Box2D 加 `--branch v3.1.1`，对 vendored 路径运行 `diff -qr`。Windows API 参考：[IFolderView 官方契约](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifolderview)。
