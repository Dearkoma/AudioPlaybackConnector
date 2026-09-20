# AudioPlaybackConnector (APC)

**Windows 10 2004+ 蓝牙音频接收 (A2DP Sink) 连接工具**

[English](https://github.com/Dearkoma/AudioPlaybackConnector/blob/master/README.md) | **简体中文**

---

### 为什么写这个项目

作者对声音比较敏感，喜欢戴耳机 — 既不想打扰别人，也不想被别人打扰。Windows 10 2004 虽已加入蓝牙 A2DP Sink 支持，但系统没有内置的连接管理工具。已有的第三方软件要么不能最小化到托盘，要么不开源。于是就有了这个项目：一个简洁、现代、透明的小工具。

### 概述

AudioPlaybackConnector 是一个单线程 C++/WinRT 桌面应用，为 Windows 10 2004+ 提供蓝牙 A2DP Sink 功能。它驻留在系统托盘，左键点击弹出**自绘**的设备列表。PC 充当蓝牙音箱，接收来自手机/平板的音频流。

- **语言：** C++20 (latest standard)，C++/WinRT 2.0，WIL
- **UI：** Win32 窗口 + XAML Islands (`DesktopWindowXamlSource`)
- **设备列表：** 自绘弹窗，带刷新动效线条与刷新按钮 —— 系统 `DevicePicker` 无法承载这两者
- **静音链路识别：** Windows 报告链路正常、却一个音频字节都没送过来的情形（Windows 各实现上都存在的间歇性 A2DP sink 故障），通过测量输出端点电平识别并如实显示；弹窗里提供一键**「重连」**，把文档里那套解法交给用户手动执行
- **线程模型：** 单线程套间 —— `winrt::init_apartment(winrt::apartment_type::single_threaded)`。XAML Islands 必须运行在 STA 上；注意 `init_apartment` **默认是 MTA**：在 MTA 下协程每次 `co_await` 之后都会在线程池线程上恢复，于是任何 XAML 调用都会以 `RPC_E_WRONG_THREAD` 失败，而且岛根本无法完成初始化。
- **工具集：** Visual Studio 2022，v143 平台工具集
- **系统要求：** Windows 10 2004+ (10.0.19041.0)
- **作者：** Dearkoma
- **许可证：** MIT

### 预览

![预览](https://raw.githubusercontent.com/Dearkoma/AudioPlaybackConnector/master/AudioPlaybackConnector.gif)

### 构建系统

- 解决方案：`AudioPlaybackConnector.sln`
- 项目：`AudioPlaybackConnector.vcxproj` (v143, CppWinRT 2.0.240111.5, WIL 1.0.260126.7)
- CI/CD：`.github/workflows/build.yaml` — 构建 x86/x64，由 tags/PR 触发
- NuGet 包：`Microsoft.Windows.CppWinRT`，`Microsoft.Windows.ImplementationLibrary`

### 源码文件结构

| 文件 | 功能 |
|------|------|
| `AudioPlaybackConnector.cpp` | 主入口：`wWinMain`、`WndProc`、所有 UI 逻辑、蓝牙操作 |
| `AudioPlaybackConnector.h` | 全局状态：窗口句柄、XAML 引用、连接表、互斥锁、自定义窗口消息 |
| `pch.h` / `pch.cpp` | 预编译头 — 所有 Windows/C++/WinRT 头文件 |
| `Util.hpp` | UTF-8 ↔ UTF-16 转换、模块路径辅助函数 |
| `I18n.hpp` + `FnvHash.hpp` | 通过 FNV-1a 哈希查找实现多语言翻译，`_()` / `C_()` 宏 |
| `SettingsUtil.hpp` | JSON 配置文件读写（`AudioPlaybackConnector.json`） |
| `Direct2DSvg.hpp` | 通过 Direct2D 将 SVG 渲染为 HICON 托盘图标（明/暗主题） |
| `resource.h` / `AudioPlaybackConnector.rc` | Win32 资源：图标、SVG、版本信息 |
| `AudioPlaybackConnector.manifest` | DPI 感知 (PerMonitorV2)、Common Controls v6、长路径支持 |
| `targetver.h` | Windows SDK 版本定义 |

### 架构与数据流

```
wWinMain()
  ├─ winrt::init_apartment()          // STA 单线程套间
  ├─ CreateWindowExW (1×1 layered)    // 仅托盘的窗口，alpha = 0
  ├─ LoadSettings()                   // 恢复 g_reconnect、g_fixSilentConnection、
  │                                   // g_lastDevices
  ├─ SetupSvgIcon()                   // Direct2D → HICON 图标（亮/暗色）
  ├─ UpdateNotifyIcon()               // Shell_NotifyIcon
  ├─ PostMessage(WM_CONNECTDEVICE)    // 启动时自动重连
  └─ GetMessage 消息循环              // 阻塞等待消息（无轮询）
                                      // PreTranslateMessage → 弹窗岛（键盘/输入法）

启动时不创建任何 XAML 岛：右键菜单是纯 Win32 弹出菜单，设备弹窗（含自己的岛）
在第一次左键点击时才构建。

托盘图标左键点击 → ShowDeviceFlyout()
  ├─ CreateFlyout()                   // 仅一次：弹出窗口 + XAML 岛 + XAML 树
  ├─ 高度由固定指标算出、对齐到托盘图标上方
  ├─ SetWindowPos(SWP_SHOWWINDOW)     // g_flyoutVisible = true
  └─ RefreshDeviceList()              // 异步执行

RefreshDeviceList() [fire_and_forget]
  ├─ SetFlyoutRefreshing(true)        // 动效线条开启，刷新按钮禁用
  ├─ co_await DeviceInformation::FindAllAsync(GetDeviceSelector(),
  │            { System.Devices.Aep.IsConnected })   // 挂起等待
  └─ RebuildDeviceRows()              // 已连接设备排在最前
     SetFlyoutRefreshing(false)

行内按钮点击 → ConnectDevice() / DisconnectDevice() / ReconnectDeviceNow()
ConnectDevice(DeviceInformation, autoReconnectAttempt, allowAutoReconnect)
                                       [fire_and_forget]
  ├─ UpdateDeviceStatus("Connecting") // 原地更新该行，按钮禁用
  ├─ 先等够重连冷却               // 见下文"连接归属与 sink 生命周期"
  ├─ AudioPlaybackConnection::TryCreateFromId()
  ├─ insert_or_assign 写入 map        // 若已有旧表项则替换（并关闭旧的）
  ├─ 注册 StateChanged 回调         // → WM_CONNECTION_CLOSED / _OPENED
  ├─ connection.StartAsync()          // 挂起等待 —— 让 PC 成为可被连的 sink
  ├─ connection.OpenAsync()           // 挂起等待 —— 请求建立链路
  └─ 成功：State()==Opened → "Connected"，否则 "Connected, no audio"
           （之后若 sink 起来，StateChanged(Opened) 会把该行升级为已连接）
           → VerifyAudioAfterConnect()  // 仅在"连接后无音频时自动重连"勾选时
     失败：只关闭本次协程自己拥有的连接，再 UpdateDeviceStatus(错误信息, Retry)

VerifyAudioAfterConnect(device, attempt, allowAutoReconnect) [fire_and_forget]
  ├─ 先等链路真正到达 Opened（最多 4 秒）—— 刻意**不用** State() 快照判断：
  │    OpenAsync 返回时 State() 仍是 Closed，Opened 要等 StateChanged 约一秒后才到
  ├─ 然后每 250ms 采一次默认输出端点的电平，共观察 3 秒
  ├─ 测到音频（或端点根本读不到）→ 记一行 "Audio confirmed" 后结束
  └─ 测不到音频 → 记录测量结果 + 一份端点清单，并让该行不再假装有声音：
       "已连接，无音频"
       只有右键菜单「连接后无音频时自动重连」**已勾选**、且本次连接本身不是
       手动「重连」时才动手：断开 → 等 1.5 秒冷却 → ConnectDevice(..., false)。
       只尝试一次，且后续那次连接不会再次触发自动重连

StateChanged (OPENED 状态)            // 在蓝牙/音频线程触发
  └─ PostMessage(WM_CONNECTION_OPENED) // 切换到 UI 线程处理
WndProc / WM_CONNECTION_OPENED        // 仅在 UI 线程执行
  └─ 校验身份 → UpdateDeviceStatus("Connected", Disconnect)

StateChanged (CLOSED 状态)            // 在蓝牙/音频线程触发
  └─ PostMessage(WM_CONNECTION_CLOSED) // 切换到 UI 线程处理
WndProc / WM_CONNECTION_CLOSED        // 仅在 UI 线程执行
  └─ 加锁 → 身份匹配才从 map 删除 → 解锁
     → MarkDeviceClosed → UpdateDeviceStatus("Not connected", Connect)
```

### 连接归属与 sink 生命周期

手机能不能出声，全看**谁把 `AudioPlaybackConnection` 对象持有住**：

- `StartAsync()` 才是"把 PC 配置成一个可被连接的 A2DP sink"的那一步，而这份配置**属于对象**。打开链路的那个对象一旦被释放，链路跟着断。
- `OpenAsync()` 只是**请求**建立连接。返回 `Success` 只代表请求被受理，不代表有声音——真正的权威是连接自身的 `State()`，只有 `Opened` 才意味着音频在流。
- 正因为"对象即资源"，同一个设备同时只能被一个连接跟踪，清理时**绝不能关掉不属于自己的那条连接**。

下面每条规则背后都有对应的踩坑，破坏任意一条都会得到同一个症状：手机和 PC 两边都显示已连接，但音箱一点声音都没有。

| 规则 | 为什么 |
|------|--------|
| map 表项用 `insert_or_assign` **替换**，绝不用 `emplace` | `emplace` 在已有表项时会静默保留旧对象，于是新连接在协程结束时被析构，而 UI 仍然声称设备已连接 |
| 清理时只在**身份匹配**当前 map 表项时才关闭 | 旧的一次失败/重复请求绝不能把后来那条正常工作的连接关掉 |
| 断开后启动 **1.5 秒重连冷却**（`g_lastCloseTime`） | Windows 需要一点时间释放 sink 端点；在端点还没释放完时重连，会得到一条"看着已连接、其实完全静音"的链路 |
| 行内状态只有 `State() == Opened` 才显示 `Connected`，否则显示 `Connected, no audio` | "已连上"和"音频在流"是两个不同状态，只有后者才有声音 |
| `WM_CONNECTION_CLOSED` / `WM_CONNECTION_OPENED` 都携带连接身份 | `StateChanged` 在蓝牙线程触发；过期的消息绝不能碰到更新的连接 |

### "连上了却没声音"：这类问题**不在我们这边**

还有另一种故障，用户看到的症状完全一样，但上面那些归属规则救不了它，因为我们这边根本没错：

- Windows 接受了连接，并且报告 `State() == Opened`。
- 手机确信自己正在往 PC 推流，于是把自己的扬声器静音了。
- 音频一个字节都没到达输出端点 —— 两端都是静音。
- 它是**间歇性**的，Windows 上所有 A2DP sink 实现都中招（微软自家的 Bluetooth Audio Receiver 也一样），而**唯一已知的解法就是把链路彻底断掉、重新协商一次**。

`AudioPlaybackConnection` 看不到这个状态（它早就说 `Opened` 了），所以本程序改为**测量默认输出端点的峰值电平**（Core Audio 的 `IAudioMeterInformation`）。

**这个测量是"报告"，不是"扳机"。** 早期版本一旦发现输出端点在 2.5 秒内始终静音，就自动断开重连，替用户执行文档里那套解法。这个设计错得很彻底：**静音的输出端点，和"手机还没开始放音（或正好在两首歌之间）"根本无法区分**，所以它会拆掉本来健康的连接；而拆掉链路又会让手机暂停播放，于是下一次照样测得静音 —— 它用"把好用的东西弄坏"来证明自己是对的。能自动化的是**测量**；要不要重连是用户的决定，这正是**「重连」**按钮存在的意义。

| 组成 | 行为 |
|------|------|
| `GetDefaultRenderPeak()` | 默认输出端点的峰值电平；**读不到时返回负数**。读不到一律按"正常"处理，绝不当作静音 —— 否则在本程序查询不了音频栈的机器上，每一条连接都会被误判成坏的。 |
| `VerifyAudioAfterConnect()` | 仅在右键菜单「连接后无音频时自动重连」勾选时才会运行。先等链路到达 `Opened`（最多 4 秒，反复重读状态，**绝不用快照**），再每 250ms 采一次、观察 3 秒。测不到音频时记录测量结果、把该行改成 *已连接，无音频*，然后断开重开**一次** —— 每次用户主动连接只尝试一次，且后续那次连接不会再次触发。 |
| `LogAudioInventory()` | 纯诊断，只在测得静音后写入：列出所有活动渲染端点及其电平，外加默认端点的友好名、音量与静音状态。这样收到日志就能一句话分清是"音频跑去了别的设备"、"默认端点被静音了"还是"音频压根没进音频栈"。 |
| `ReconnectDeviceNow()` | 把手工解法做成一键（断开 + 连接），也就是「重连」按钮调用的东西。它**不会**再连锁触发自动重连 —— 点一次就是一次。 |
| 右键菜单「连接后无音频时自动重连」 | 开关整套行为（JSON 配置里的 `fixSilentConnection`）。**默认关闭**，而"关闭"意味着连接路径里**一次 Core Audio 调用都没有** —— 这正是它默认关的原因：静音链路和故障链路不再被本程序的代码搅在一起，"是不是这个版本把声音弄没的"就变成一个测试能回答的问题。 |

### 设备弹窗（自绘）

设备列表由应用自己绘制，不再使用系统 `DevicePicker`。

`DevicePicker` 只能自定义标题、几个颜色和每行的状态文本，**没有任何接口**可以加入刷新动效或刷新按钮。结果是用户既看不出列表正在重新检测，也无法主动触发重新检测。因此改为自绘弹窗，把整个界面收回到自己手里。

| 方面 | 实现方式 |
|------|----------|
| 宿主窗口 | 独立的 `WS_POPUP` 窗口 (`WS_EX_TOOLWINDOW \| WS_EX_TOPMOST`)。**刻意不加** `WS_EX_LAYERED` —— XAML Islands 内容在分层窗口中不会渲染，这也是 alpha = 0 的托盘窗口无法承载弹窗的原因。 |
| 岛的初始化 | 三件事是硬性要求，且都很容易漏：(1) STA（`init_apartment(apartment_type::single_threaded)`）；(2) 创建第一个 `DesktopWindowXamlSource` 之前调用 `WindowsXamlManager::InitializeForCurrentThread()`；(3) 由应用自己给岛的子窗口定尺寸。少任何一项，弹窗窗口都会正常打开但**一个像素都不渲染**。 |
| 岛的尺寸 | 岛自己的子窗口（`IDesktopWindowXamlSourceNative2::get_WindowHandle`）初始为 0x0，且**绝不会**跟随宿主窗口，因此 `ResizeFlyoutXamlHost()` 在每次宿主尺寸变化时（`FlyoutWndProc` 的 `WM_SIZE`，以及每次弹窗 `SetWindowPos` 之后）重新设置它的尺寸。 |
| 生命周期 | 窗口与 XAML 树在首次打开时创建，之后隐藏时保持存活，因此连接状态在多次开关之间得以保留。 |
| 布局 | 头部（标题 + "N 台已连接" 副标题 + 刷新按钮）→ 3px 刷新动效线 → 可滚动的设备列表。动效线的边框在空闲时依然可见，因此显示/隐藏它不会引起列表跳动。 |
| 刷新动效 | 3px `Border` 内嵌一个不确定进度的 `ProgressBar`。仅在刷新进行中显示；同一时间刷新按钮被禁用，副标题切换为"正在检测连接状态"。 |
| 刷新按钮 | 重新执行 `RefreshDeviceList()`，重新查询 `System.Devices.Aep.IsConnected`。 |
| 设备行 | 设备名、状态文本，以及最多两个按钮。操作按钮是 *连接* / *断开* / *重试*（`None` 表示连接正在进行中，按钮置灰）；它旁边还有一个「重连」按钮，**仅在本程序持有该连接时出现** —— 别人持有的连接谁也没法重新协商。已连接的设备排在最前。状态区分 *已连接*（音频正在通过本程序的连接流动）与 *已连接，无音频*（设备已连上但没有音频在流）；操作按钮只对**本程序自己管理的连接**提供 *断开* —— 只是链路连上（手机与系统相连、但没有本程序的连接对象）的设备提供 *连接*，否则用户看着"已连接"却没有任何办法把声音弄出来。 |
| 原地更新 | `UpdateDeviceStatus` 直接修改既有行的状态文本、按钮文字、按钮颜色与目标操作；只有在刷新时才重建整个列表。按钮点击处理器捕获 `shared_ptr<DeviceRowState>`，因此可以重新指定某一行的操作（连接 → 断开）而无需重建。 |
| 定位 | 通过 `Shell_NotifyIconGetRect` 锚定在托盘图标上方并右对齐，再钳制到最近显示器的工作区内。高度由固定的头部/行高指标（`FlyoutHeightForRows`）算出，而不是靠 `Measure` —— 岛的 XAML 树布局由岛自己负责 —— 并在每次刷新后重设，且保持底边不动。 |
| 关闭方式 | 轻量关闭：焦点离开时（`WM_ACTIVATE`/`WA_INACTIVE`）自动隐藏。`g_flyoutHiddenTick` 记录关闭时刻，使"刚刚关闭它的那一次点击"不会立刻重新打开（300ms 保护）。 |
| 主题 | 颜色按明/暗两套调色板硬编码 —— 没有 `Xaml.Application` 的岛无法解析 `ThemeResource` 查找。调色板取自与托盘图标相同的 `SystemUsesLightTheme` 注册表值。 |

### 全局状态（`AudioPlaybackConnector.h`）

| 变量 | 类型 | 用途 |
|------|------|------|
| `g_audioPlaybackConnections` | `unordered_map<wstring, pair<DeviceInformation, AudioPlaybackConnection>>` | 活跃连接表，以设备 ID 为键 |
| `g_connectionsMutex` | `std::mutex` | 保护连接表的互斥锁 |
| `g_hWndFlyout` | `HWND` | 承载弹窗岛的弹出窗口（首次打开时创建） |
| `g_hWndFlyoutXaml` | `HWND` | 岛自己的子窗口 —— 真正绘制 XAML 的那块表面。由本程序设置尺寸，框架从不代劳。 |
| `g_flyoutSource` / `g_flyoutRoot` | `DesktopWindowXamlSource` / `Grid` | XAML 岛与其 XAML 树，隐藏时保持存活 |
| `g_xamlManager` | `WindowsXamlManager` | UI 线程的 XAML 框架核心窗口（在第一个岛创建之前初始化） |
| `g_uiThreadId` | `DWORD` | 拥有 `g_hWnd`、消息循环与所有 XAML 对象的线程 —— `RunOnUiThread` 向它投递 |
| `g_deviceRows` | `vector<DeviceRow>` | 已渲染的行；每行持有状态 `TextBlock`、「重连」`Button`、操作 `Button` 和一个 `shared_ptr` 状态，使该行可被重新指定操作而无需重建 |
| `g_flyoutRefreshing` | `bool` | 是否有刷新在进行 —— 驱动动效线并禁用刷新按钮 |
| `g_flyoutVisible` / `g_flyoutHiddenTick` | `bool` / `ULONGLONG` | 可见性，以及防止"关闭它的那次点击"重新打开的时钟保护 |
| `g_reconnect` | `bool` | 下次启动时自动重连 |
| `g_fixSilentConnection` | `bool` | 默认关闭。它管住整套"测量输出端点"的动作：勾选后，测不到音频会写日志（含端点清单），并把该链路断开重开一次（每次用户主动连接最多一次）。右键菜单中的"连接后无音频时自动重连" |
| `g_shuttingDown` | `bool` | 退出时阻止协程访问已释放资源 |
| `g_lastDevices` | `vector<wstring>` | 用于自动重连的设备 ID 列表 |
| `g_lastCloseTime` | `unordered_map<wstring, Clock::time_point>` | 每台设备最近一次关闭的时刻 —— 驱动重连冷却 |

### 自定义窗口消息

| 消息 | 用途 |
|------|------|
| `WM_NOTIFYICON` (`WM_APP+1`) | 托盘图标点击、右键菜单 |
| `WM_CONNECTDEVICE` (`WM_APP+2`) | 启动时自动重连触发器 |
| `WM_CONNECTION_CLOSED` (`WM_APP+3`) | StateChanged(Closed) → UI 线程切换（线程安全） |
| `WM_RUNONUITHREAD` (`WM_APP+4`) | 携带 `RunOnUiThread` 投递的可调用对象，由 WndProc 在 UI 线程上执行 |
| `WM_CONNECTION_OPENED` (`WM_APP+5`) | StateChanged(Opened) → UI 线程切换；这一刻音频才真正开始流 |

### 线程安全（关键约束）

1. **所有 XAML 对象访问必须在 UI 线程。** XAML Islands 对象有线程亲和性，且不具备 agile 特性。本程序运行在 STA 上，协程恢复点因此落回 UI 线程；此外每个改 XAML 的辅助函数都会再校验一次 `g_uiThreadId`，一旦发现不在 UI 线程就通过 `RunOnUiThread` 把自己重新投递回去。（这一点成立的前提是 `init_apartment` 收到了 `apartment_type::single_threaded` —— 它的默认值是 MTA，那样每一次 `co_await` 恢复都会落到线程池线程上，所有 XAML 更新都会以 `RPC_E_WRONG_THREAD` 失败。）
2. **所有对 `g_audioPlaybackConnections` 的修改由 `g_connectionsMutex` 保护。** 无锁读写 = 数据竞争。`g_lastCloseTime` 由同一把锁保护,且 `IsCurrentConnection()` 内部会自行加锁 —— 绝不能在已持锁时调用它。
3. **`StateChanged` 回调在蓝牙/音频后台线程触发。** 绝对不能直接操作 XAML 或 map，只能通过 PostMessage 将工作转到 UI 线程。
4. **`ConnectDevice` 是 `fire_and_forget` 协程。** 协程恢复点在 UI 线程（STA），为了一致性仍然持锁。
5. **所有协程入口检查 `g_shuttingDown`** — 防止退出时的 use-after-free。
6. **绝不在持有 `g_connectionsMutex` 时更新 XAML。** 需要遍历连接表的地方（断开全部、重启蓝牙音频、`WM_CONNECTION_CLOSED`）先加锁收集设备 ID，解锁后再调用 `UpdateDeviceStatus`。持锁操作 UI 有死锁风险，也会让 UI 被蓝牙操作串行阻塞。

### 已知物理限制

- **蓝牙与 2.4GHz Wi-Fi 共存：** A2DP 音频流使用 2.4GHz ISM 频段，与 Wi-Fi 共享频谱。干扰是物理层固有现象，无法纯代码消除。
- **多设备蓝牙拥堵：** 多个 A2DP 连接同时建立会竞争无线电资源。`RestoreAudioService` 以每设备 500ms 间隔错开重连。

### 右键菜单功能

| 菜单项 | 操作 |
|--------|------|
| 蓝牙设置 | 打开 `ms-settings:bluetooth` |
| 语言 ▸ English / 中文 | 切换界面语言（`g_language`），写入配置文件 |
| 查看日志 | 用默认程序打开 `AudioPlaybackConnector.log` |
| 断开全部 | 关闭所有连接，立即更新 UI |
| 重启蓝牙音频 | 全部关闭 → 等待 1 秒 → 逐个错开重连 |
| 连接后无音频时自动重连 | `g_fixSilentConnection` 的勾选项 —— 打开"测量输出端点 + 测不到音频时断开重开一次"整套行为。**默认关闭**（关闭时连接路径里不含任何 Core Audio 调用） |
| 退出 | 弹出确认窗口，可选"下次启动自动重连" |
