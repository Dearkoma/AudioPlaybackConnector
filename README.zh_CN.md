# AudioPlaybackConnector (APC)

**Windows 10 2004+ 蓝牙音频接收 (A2DP Sink) 连接工具**

[English](https://github.com/Dearkoma/AudioPlaybackConnector/blob/master/README.md) | **简体中文**

---

### 为什么写这个项目

作者对声音比较敏感，喜欢戴耳机 — 既不想打扰别人，也不想被别人打扰。Windows 10 2004 虽已加入蓝牙 A2DP Sink 支持，但系统没有内置的连接管理工具。已有的第三方软件要么不能最小化到托盘，要么不开源。于是就有了这个项目：一个简洁、现代、透明的小工具。

### 概述

AudioPlaybackConnector 是一个单线程 C++/WinRT 桌面应用，为 Windows 10 2004+ 提供蓝牙 A2DP Sink 功能。它驻留在系统托盘，通过 XAML Islands 弹出窗口让用户选择蓝牙设备。PC 充当蓝牙音箱，接收来自手机/平板的音频流。

- **语言：** C++20 (latest standard)，C++/WinRT 2.0，WIL
- **UI：** Win32 窗口 + XAML Islands (`DesktopWindowXamlSource`)
- **线程模型：** 单线程套间 (`winrt::init_apartment()`)
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
  ├─ CreateWindowExW (1×1 layered)    // 隐藏父窗口，承载 XAML
  ├─ DesktopWindowXamlSource           // XAML Islands 桥接
  ├─ LoadSettings()                    // 恢复 g_reconnect、g_lastDevices
  ├─ SetupFlyout() / SetupMenu()      // XAML 弹出窗口 & 右键菜单
  ├─ SetupDevicePicker()              // DevicePicker + A2DP 设备筛选
  ├─ SetupSvgIcon()                   // Direct2D → HICON 图标（亮/暗色）
  ├─ PostMessage(WM_CONNECTDEVICE)    // 启动时自动重连
  └─ GetMessage 消息循环              // 阻塞等待消息（无轮询）

用户点击设备 → ConnectDevice() [fire_and_forget 协程]
  ├─ AudioPlaybackConnection::TryCreateFromId()
  ├─ 注册 StateChanged 回调         // → PostMessage(WM_CONNECTION_CLOSED)
  ├─ connection.StartAsync()          // 挂起等待
  ├─ connection.OpenAsync()           // 挂起等待
  └─ 成功：显示 "Connected" 状态
     失败：关闭连接、从 map 移除、显示错误状态

StateChanged (CLOSED 状态)           // 在蓝牙/音频线程触发
  └─ PostMessage(WM_CONNECTION_CLOSED) // 切换到 UI 线程处理

WndProc / WM_CONNECTION_CLOSED       // 仅在 UI 线程执行
  └─ 加锁 → 从 map 删除 → 更新 DevicePicker UI
```

### 全局状态（`AudioPlaybackConnector.h`）

| 变量 | 类型 | 用途 |
|------|------|------|
| `g_audioPlaybackConnections` | `unordered_map<wstring, pair<DeviceInformation, AudioPlaybackConnection>>` | 活跃连接表，以设备 ID 为键 |
| `g_connectionsMutex` | `std::mutex` | 保护连接表的互斥锁 |
| `g_devicePicker` | `DevicePicker` (XAML) | 设备列表弹出 UI |
| `g_reconnect` | `bool` | 下次启动时自动重连 |
| `g_shuttingDown` | `bool` | 退出时阻止协程访问已释放资源 |
| `g_lastDevices` | `vector<wstring>` | 用于自动重连的设备 ID 列表 |

### 自定义窗口消息

| 消息 | 用途 |
|------|------|
| `WM_NOTIFYICON` (`WM_APP+1`) | 托盘图标点击、右键菜单 |
| `WM_CONNECTDEVICE` (`WM_APP+2`) | 启动时自动重连触发器 |
| `WM_CONNECTION_CLOSED` (`WM_APP+3`) | StateChanged → UI 线程切换（线程安全） |

### 线程安全（关键约束）

1. **所有 XAML 对象访问必须在 UI 线程。** XAML Islands 对象有线程亲和性。
2. **所有对 `g_audioPlaybackConnections` 的修改由 `g_connectionsMutex` 保护。** 无锁读写 = 数据竞争。
3. **`StateChanged` 回调在蓝牙/音频后台线程触发。** 绝对不能直接操作 XAML 或 map，只能通过 PostMessage 将工作转到 UI 线程。
4. **`ConnectDevice` 是 `fire_and_forget` 协程。** 协程恢复点在 UI 线程（STA），为了一致性仍然持锁。
5. **所有协程入口检查 `g_shuttingDown`** — 防止退出时的 use-after-free。

### 已知物理限制

- **蓝牙与 2.4GHz Wi-Fi 共存：** A2DP 音频流使用 2.4GHz ISM 频段，与 Wi-Fi 共享频谱。干扰是物理层固有现象，无法纯代码消除。
- **多设备蓝牙拥堵：** 多个 A2DP 连接同时建立会竞争无线电资源。`RestoreAudioService` 以每设备 500ms 间隔错开重连。

### 右键菜单功能

| 菜单项 | 操作 |
|--------|------|
| 蓝牙设置 | 打开 `ms-settings:bluetooth` |
| 断开全部 | 关闭所有连接，立即更新 UI |
| 重启蓝牙音频 | 全部关闭 → 等待 1 秒 → 逐个错开重连 |
| 退出 | 弹出确认窗口，可选"下次启动自动重连" |
