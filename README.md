# AudioPlaybackConnector (APC)

**Bluetooth A2DP Sink connector for Windows 10 2004+**

**English** | [简体中文](https://github.com/Dearkoma/AudioPlaybackConnector/blob/master/README.zh_CN.md)

---

### Motivation

The author is sensitive to ambient noise — they prefer wearing headphones at all times: it keeps their audio private without disturbing others, and blocks outside noise from disturbing them. Windows 10 2004 added native Bluetooth A2DP Sink support but no built-in way to manage connections. Existing third-party apps either lacked system-tray minimization or weren't open-source. This project was born to fill that gap with a simple, modern, and transparent alternative.

### Overview

AudioPlaybackConnector is a single-threaded C++/WinRT desktop application that enables Bluetooth A2DP Sink on Windows 10 2004+. It lives in the system tray and provides a XAML Islands flyout for device selection. The PC acts as a Bluetooth speaker, receiving audio from phones/tablets.

- **Language:** C++20 (latest standard), C++/WinRT 2.0, WIL
- **UI:** Win32 window + XAML Islands (`DesktopWindowXamlSource`)
- **Threading:** Single-threaded apartment (`winrt::init_apartment()`)
- **Toolset:** Visual Studio 2022, v143 platform toolset
- **OS Target:** Windows 10 2004+ (10.0.19041.0)
- **Author:** Dearkoma
- **License:** MIT

### Preview

![Preview](https://raw.githubusercontent.com/Dearkoma/AudioPlaybackConnector/master/AudioPlaybackConnector.gif)

### Build System

- Solution: `AudioPlaybackConnector.sln`
- Project: `AudioPlaybackConnector.vcxproj` (v143, CppWinRT 2.0.240111.5, WIL 1.0.260126.7)
- CI/CD: `.github/workflows/build.yaml` — builds x86/x64, triggers on tags/PRs
- NuGet packages: `Microsoft.Windows.CppWinRT`, `Microsoft.Windows.ImplementationLibrary`

### Source File Map

| File | Role |
|------|------|
| `AudioPlaybackConnector.cpp` | Main entry: `wWinMain`, `WndProc`, all UI logic, Bluetooth operations |
| `AudioPlaybackConnector.h` | Global state: HWND handles, XAML refs, connection map, mutex, custom window messages |
| `pch.h` / `pch.cpp` | Precompiled header — all Windows/C++/WinRT includes |
| `Util.hpp` | UTF-8 ↔ UTF-16 conversion, module filesystem path helper |
| `I18n.hpp` + `FnvHash.hpp` | Translation via FNV-1a hash lookup, `_()` / `C_()` macros |
| `SettingsUtil.hpp` | JSON config read/write (`AudioPlaybackConnector.json`) |
| `Direct2DSvg.hpp` | Renders SVG to HICON via Direct2D for tray icon (light/dark theme) |
| `resource.h` / `AudioPlaybackConnector.rc` | Win32 resources: icon, SVG, version info |
| `AudioPlaybackConnector.manifest` | DPI awareness (PerMonitorV2), common controls v6, long path support |
| `targetver.h` | Windows SDK version gate |

### Architecture & Data Flow

```
wWinMain()
  ├─ winrt::init_apartment()          // STA
  ├─ CreateWindowExW (1×1 layered)    // hidden parent for XAML
  ├─ DesktopWindowXamlSource           // XAML Islands bridge
  ├─ LoadSettings()                    // restore g_reconnect, g_lastDevices
  ├─ SetupFlyout() / SetupMenu()      // XAML flyout & context menu
  ├─ SetupDevicePicker()              // DevicePicker with A2DP filter
  ├─ SetupSvgIcon()                   // Direct2D → HICON light/dark
  ├─ PostMessage(WM_CONNECTDEVICE)    // auto-reconnect on start
  └─ GetMessage loop                  // blocks until messages arrive (NO POLLING)

User clicks device → ConnectDevice() [fire_and_forget coroutine]
  ├─ AudioPlaybackConnection::TryCreateFromId()
  ├─ Register StateChanged callback   // → PostMessage(WM_CONNECTION_CLOSED)
  ├─ connection.StartAsync()          // suspend & wait
  ├─ connection.OpenAsync()           // suspend & wait
  └─ On success: set "Connected" status
     On failure: close + erase from map, set error status

StateChanged (CLOSED)                // fires on Bluetooth/audio thread
  └─ PostMessage(WM_CONNECTION_CLOSED) // marshals to UI thread

WndProc / WM_CONNECTION_CLOSED       // UI thread only
  └─ lock → erase from map → update DevicePicker UI
```

### Global State (`AudioPlaybackConnector.h`)

| Variable | Type | Purpose |
|----------|------|---------|
| `g_audioPlaybackConnections` | `unordered_map<wstring, pair<DeviceInformation, AudioPlaybackConnection>>` | Active connections, keyed by device ID |
| `g_connectionsMutex` | `std::mutex` | Protects the connection map |
| `g_devicePicker` | `DevicePicker` (XAML) | Flyout UI for device list |
| `g_reconnect` | `bool` | Reconnect on next launch |
| `g_shuttingDown` | `bool` | Prevent coroutines from touching freed resources on exit |
| `g_lastDevices` | `vector<wstring>` | Device IDs for auto-reconnect |

### Custom Window Messages

| Message | Purpose |
|---------|---------|
| `WM_NOTIFYICON` (`WM_APP+1`) | System tray icon clicks, context menu |
| `WM_CONNECTDEVICE` (`WM_APP+2`) | Auto-reconnect trigger on startup |
| `WM_CONNECTION_CLOSED` (`WM_APP+3`) | StateChanged → UI thread marshal (thread safety) |

### Thread Safety (critical invariants)

1. **All XAML object access MUST be on the UI thread.** XAML Islands objects have thread affinity.
2. **All `g_audioPlaybackConnections` mutations guarded by `g_connectionsMutex`.** Read or write the map without the lock = data race.
3. **`StateChanged` callback fires on a Bluetooth/audio background thread.** It must NEVER touch XAML or the map directly. It posts `WM_CONNECTION_CLOSED` and the WndProc handler does the work.
4. **`ConnectDevice` is `fire_and_forget`.** Co-routine resumes happen on the UI thread (STA), but the mutex is still held for consistency.
5. **`g_shuttingDown` checked at coroutine entry points** — prevents use-after-free during exit.

### Known Physical Limitations

- **Bluetooth + 2.4 GHz Wi-Fi coexistence:** A2DP streaming uses the 2.4 GHz ISM band, shared with Wi-Fi. Interference is expected and inherent to the physical layer.
- **Multi-device Bluetooth congestion:** Simultaneous A2DP connections compete for radio time. `RestoreAudioService` staggers reconnects by 500 ms per device.

### Menu Items

| Item | Action |
|------|--------|
| Bluetooth Settings | Opens `ms-settings:bluetooth` |
| Disconnect All | Closes all connections, updates UI immediately |
| Restart Bluetooth Audio | Close all → wait 1s → stagger-reconnect each device |
| Exit | Flyout with "Reconnect on next start" checkbox |
