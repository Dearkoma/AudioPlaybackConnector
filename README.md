# AudioPlaybackConnector (APC)

**Bluetooth A2DP Sink connector for Windows 10 2004+**

**English** | [简体中文](https://github.com/Dearkoma/AudioPlaybackConnector/blob/master/README.zh_CN.md)

---

### Motivation

The author is sensitive to ambient noise — they prefer wearing headphones at all times: it keeps their audio private without disturbing others, and blocks outside noise from disturbing them. Windows 10 2004 added native Bluetooth A2DP Sink support but no built-in way to manage connections. Existing third-party apps either lacked system-tray minimization or weren't open-source. This project was born to fill that gap with a simple, modern, and transparent alternative.

### Overview

AudioPlaybackConnector is a single-threaded C++/WinRT desktop application that enables Bluetooth A2DP Sink on Windows 10 2004+. It lives in the system tray and opens a self-drawn XAML Islands flyout listing the available devices. The PC acts as a Bluetooth speaker, receiving audio from phones/tablets.

- **Language:** C++20 (latest standard), C++/WinRT 2.0, WIL
- **UI:** Win32 window + XAML Islands (`DesktopWindowXamlSource`)
- **Device list:** self-drawn flyout with a live refresh indicator and a refresh button — the system `DevicePicker` can host neither
- **Honest link status, and no guesswork:** a row says *Connected* only when the sink reports itself open, and *Connected, no audio* otherwise — but nothing ever tears a link down on a guess. The intermittent "Windows says connected, no audio arrives" failure (seen in every Windows A2DP sink implementation) is left to the one documented remedy, offered as a one-click *Reconnect*, plus a read-only endpoint diagnostic in the tray menu
- **Threading:** Single-threaded apartment — `winrt::init_apartment(winrt::apartment_type::single_threaded)`. XAML islands require an STA, and note that `init_apartment` **defaults to MTA**: in an MTA a coroutine resumes on a thread pool thread after every `co_await`, so any XAML call from there fails with `RPC_E_WRONG_THREAD`, and the island never initialises at all.
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
  ├─ CreateWindowExW (1×1 layered)    // tray-only window, alpha 0
  ├─ LoadSettings()                   // restore g_reconnect, g_lastDevices
  ├─ SetupSvgIcon()                   // Direct2D → HICON light/dark
  ├─ UpdateNotifyIcon()               // Shell_NotifyIcon
  ├─ PostMessage(WM_CONNECTDEVICE)    // auto-reconnect on start
  └─ GetMessage loop                  // blocks until messages arrive (NO POLLING)
                                      // PreTranslateMessage → island (keyboard/IME)

No XAML island exists at startup: the tray menu is a plain Win32 popup menu, and
the device flyout (with its own island) is built on the first left click.

Tray icon left click → ShowDeviceFlyout()
  ├─ CreateFlyout()                   // once: popup window + island + XAML tree
  ├─ Height from fixed metrics, anchor above the tray icon
  ├─ SetWindowPos(SWP_SHOWWINDOW)     // g_flyoutVisible = true
  └─ RefreshDeviceList()              // asynchronous

RefreshDeviceList() [fire_and_forget]
  ├─ SetFlyoutRefreshing(true)        // indicator on, refresh button disabled
  ├─ co_await DeviceInformation::FindAllAsync(GetDeviceSelector(),
  │            { System.Devices.Aep.IsConnected })   // suspend & wait
  └─ RebuildDeviceRows()              // connected devices first
     SetFlyoutRefreshing(false)

Row button click → ConnectDevice() / DisconnectDevice() / ReconnectDeviceNow()
ConnectDevice(DeviceInformation)      [fire_and_forget]
  ├─ UpdateDeviceStatus("Connecting") // row updated in place, button disabled
  ├─ Wait out the reconnect cooldown  // see "Connection Ownership" below
  ├─ AudioPlaybackConnection::TryCreateFromId()
  ├─ insert_or_assign into the map    // replaces (and closes) any previous entry
  ├─ Register StateChanged callback   // → WM_CONNECTION_CLOSED / _OPENED
  ├─ connection.StartAsync()          // suspend & wait — makes the PC a sink
  ├─ connection.OpenAsync()           // suspend & wait — requests the link
  └─ On success: State()==Opened → "Connected", otherwise "Connected, no audio"
                 (StateChanged(Opened) upgrades the row if the sink comes up later)
                 Nothing else: this path makes no Core Audio calls at all, so a
                 silent link can never be caused by the app watching it — see
                 "The Silent-Link Failure" below
     On failure: close ONLY a connection this coroutine owns, then
                 UpdateDeviceStatus(error, Retry)

ReconnectDeviceNow(deviceId)          // the *Reconnect* button, and nothing else
  ├─ DisconnectDevice()               // also arms the cooldown
  └─ ConnectDevice(deviceId)          // one click, one reconnect — never chains

Tray menu → IDM_LOG_AUDIO_INVENTORY → LogAudioInventory()   // read-only, on demand
  └─ Logs every active render endpoint with its level, plus the default
     endpoint's friendly name, volume and mute state. Disconnects nothing.

StateChanged (OPENED)                 // fires on Bluetooth/audio thread
  └─ PostMessage(WM_CONNECTION_OPENED) // marshals to UI thread
WndProc / WM_CONNECTION_OPENED        // UI thread only
  └─ identity check → UpdateDeviceStatus("Connected", Disconnect)

StateChanged (CLOSED)                 // fires on Bluetooth/audio thread
  └─ PostMessage(WM_CONNECTION_CLOSED) // marshals to UI thread
WndProc / WM_CONNECTION_CLOSED        // UI thread only
  └─ lock → erase from map (only if the identity matches) → unlock
     → MarkDeviceClosed → UpdateDeviceStatus("Not connected", Connect)
```

### Connection Ownership & the Sink Lifecycle

Everything about letting a phone play through the PC hinges on who keeps the
`AudioPlaybackConnection` object alive:

- `StartAsync()` is what configures the PC as a **listening A2DP sink**, and that
  configuration belongs to the object. If the object that opened a link is
  dropped, the link goes with it.
- `OpenAsync()` only **requests** the connection. `Success` means the request was
  accepted, not that audio flows — the connection's own `State()` is the
  authority, and `State() == Opened` is the only state that means "sound".
- Because the object *is* the resource, exactly one connection per device may be
  tracked, and cleanup must never close a connection the caller does not own.

Every rule below exists because breaking it produces the same symptom: the phone
shows connected on both sides, and nothing comes out of the speakers.

| Rule | Why it matters |
|------|----------------|
| The map entry is **replaced** (`insert_or_assign`), never `emplace`d | `emplace` silently keeps the older object, so the new connection dies at the end of the coroutine while the UI keeps claiming the device is connected |
| Cleanup closes a connection only when its identity matches the map entry | A failure from an older or duplicate attempt must not drop a newer, working connection |
| A closed device arms a **1.5 s reconnect cooldown** (`g_lastCloseTime`) | Windows needs a moment to release the sink endpoint; reconnecting into an endpoint that is still being torn down yields a link that looks connected but stays silent |
| A row reads `Connected` only when `State() == Opened`; otherwise `Connected, no audio` | "Linked" and "streaming" are different states, and only the second one produces sound |
| `WM_CONNECTION_CLOSED` / `WM_CONNECTION_OPENED` carry the connection identity | `StateChanged` fires on a Bluetooth thread; a stale message must never touch a newer connection |

### The silent-link failure: diagnosed, never auto-repaired

A second failure mode looks identical to the user and cannot be fixed by the
ownership rules above, because there is nothing wrong on this side:

- Windows accepts the connection and reports `State() == Opened`.
- The phone is convinced it is streaming to the PC, so it mutes its own speaker.
- Not one audio byte ever reaches the output endpoint — both ends are silent.
- It is intermittent, it affects every A2DP sink implementation on Windows
  (Microsoft's own Bluetooth Audio Receiver included), and the only known
  remedy is to tear the connection down and renegotiate it.

`AudioPlaybackConnection` cannot see this state — it already says `Opened` — and
**the app does not try to detect it in order to act on it.** An earlier version
did: it metered the default render endpoint (`IAudioMeterInformation`, via Core
Audio) and disconnected any link that stayed silent for 2.5 s, running the
documented workaround automatically. That was wrong twice over:

- A silent output cannot be told apart from a phone that has not started playing
  yet (or is between two tracks), so it tore down healthy connections.
- Tearing a link down makes the phone pause, so the next reading was silent too:
  it proved itself right by breaking something that worked.

The automatic repair was therefore removed, and the setting that gated it was
deleted rather than defaulted to `false` — a key that is still read would keep
the old behaviour alive on any machine whose config file was written by the
version that defaulted it **on**. What remains:

| Piece | Behaviour |
|-------|-----------|
| The connect path | Makes **no Core Audio calls at all**, so a silent link can never be caused by the app watching it. That is what makes "is this build the one that goes quiet?" a question a test can answer. |
| `ReconnectDeviceNow()` | The manual workaround in one click (disconnect + connect), i.e. what the *Reconnect* button calls. One click, one reconnect — it never chains. |
| `LogAudioInventory()` | Read-only, and only on request (tray menu: *Log Audio Endpoint Inventory*). Logs every active render endpoint with its level, plus the default endpoint's friendly name, volume and mute state, so a report can be attributed to the audio going elsewhere, to a muted default endpoint, or to nothing entering the audio stack at all. |
| Row status | *Connected* vs *Connected, no audio* still distinguishes "linked" from "streaming", but nothing acts on it. |

The lesson this cost two releases: **measuring and acting are different
decisions.** Measuring is cheap and safe. Acting on a signal that cannot tell
"broken" apart from "not playing yet" is neither.

### Device Flyout

The device list is drawn by the app itself rather than by the system `DevicePicker`.

The picker can only be customised with a title, a few colours and a per-row status
string. It offers no way to add a refresh indicator or a refresh button, so the user
cannot tell whether the list is being re-checked, and cannot ask for a re-check. The
flyout replaces it so the whole surface is under the app's control.

| Aspect | Implementation |
|--------|----------------|
| Host window | Dedicated `WS_POPUP` window (`WS_EX_TOOLWINDOW \| WS_EX_TOPMOST`). **Not** `WS_EX_LAYERED` — XAML Islands content does not composite inside a layered window, which is also why the alpha-0 tray window cannot host the flyout. |
| Island setup | Three things are mandatory and easy to miss: (1) an STA (`init_apartment(apartment_type::single_threaded)`), (2) `WindowsXamlManager::InitializeForCurrentThread()` before the first `DesktopWindowXamlSource`, and (3) sizing the island's own child window. Skip any of them and the flyout window opens but stays completely unpainted. |
| Island sizing | The island's child window (`IDesktopWindowXamlSourceNative2::get_WindowHandle`) starts at 0x0 and **never** follows the host window, so `ResizeFlyoutXamlHost()` re-sizes it on every host resize (`WM_SIZE` in `FlyoutWndProc`, and after each `SetWindowPos` of the flyout). |
| Lifetime | Window and XAML tree are created on first use and then kept alive while hidden, so connection state survives between opens. |
| Layout | Header (title + "N connected" subtitle + refresh button) → 3px refresh indicator → scrolling device list. The indicator's border stays visible when idle, so toggling it never shifts the list. |
| Refresh indicator | An indeterminate `ProgressBar` inside a 3px `Border`. Visible only while a refresh is in flight; the refresh button is disabled at the same time, and the subtitle switches to "Checking connection status". |
| Refresh button | Re-runs `RefreshDeviceList()`, re-querying `System.Devices.Aep.IsConnected`. |
| Device rows | Name, status text and up to two buttons. The action button is *Connect* / *Disconnect* / *Retry* (`None` disables it while a connect is in flight); next to it, a *Reconnect* button appears **only while this app owns the connection** — nobody else can renegotiate a link. Connected devices are sorted first. The status distinguishes *Connected* (audio flowing through this app's connection) from *Connected, no audio* (the device is linked but nothing is streaming), and the action button only offers *Disconnect* for a connection this app actually manages — a merely-linked device offers *Connect* instead of leaving the user with no way to get sound. |
| In-place update | `UpdateDeviceStatus` mutates the existing row's status text, button label, button colour and target action; the list is only rebuilt on a refresh. The click handler captures a `shared_ptr<DeviceRowState>`, so a row's action can be re-targeted (connect → disconnect) without rebuilding it. |
| Positioning | Anchored above and right-aligned to the tray icon via `Shell_NotifyIconGetRect`, clamped to the nearest monitor's work area. The height is computed from fixed header/row metrics (`FlyoutHeightForRows`) instead of from a `Measure` pass — the island owns the layout of its tree — and is re-applied after every refresh with the bottom edge kept in place. |
| Dismissal | Light dismiss: the flyout hides on `WM_ACTIVATE`/`WA_INACTIVE`. `g_flyoutHiddenTick` remembers when it closed so the same click that closed it does not immediately reopen it (300 ms guard). |
| Theme | Colours are hardcoded per light/dark palette — an island with no `Xaml.Application` cannot resolve `ThemeResource` lookups. The palette is chosen from the same `SystemUsesLightTheme` registry value that selects the tray icon. |

### Global State (`AudioPlaybackConnector.h`)

| Variable | Type | Purpose |
|----------|------|---------|
| `g_audioPlaybackConnections` | `unordered_map<wstring, pair<DeviceInformation, AudioPlaybackConnection>>` | Active connections, keyed by device ID |
| `g_connectionsMutex` | `std::mutex` | Protects the connection map |
| `g_hWndFlyout` | `HWND` | Popup window hosting the flyout island (created on first open) |
| `g_hWndFlyoutXaml` | `HWND` | The island's own child window — the surface that actually paints the XAML. Sized by the app, never by the framework. |
| `g_flyoutSource` / `g_flyoutRoot` | `DesktopWindowXamlSource` / `Grid` | Island and its XAML tree, kept alive while hidden |
| `g_xamlManager` | `WindowsXamlManager` | XAML framework core window for the UI thread (initialised before the first island) |
| `g_uiThreadId` | `DWORD` | Thread that owns `g_hWnd`, the message loop and every XAML object — `RunOnUiThread` marshals to it |
| `g_deviceRows` | `vector<DeviceRow>` | Rendered rows; each holds its status `TextBlock`, action `Button` and a `shared_ptr` state so the row can be re-targeted without a rebuild |
| `g_flyoutRefreshing` | `bool` | A refresh is in flight — drives the indicator and disables the refresh button |
| `g_flyoutVisible` / `g_flyoutHiddenTick` | `bool` / `ULONGLONG` | Visibility, plus the tick guard that stops the closing click from reopening the flyout |
| `g_reconnect` | `bool` | Reconnect on next launch |
| `g_shuttingDown` | `bool` | Prevent coroutines from touching freed resources on exit |
| `g_lastDevices` | `vector<wstring>` | Device IDs for auto-reconnect |
| `g_lastCloseTime` | `unordered_map<wstring, Clock::time_point>` | When each device was last closed — drives the reconnect cooldown |

### Custom Window Messages

| Message | Purpose |
|---------|---------|
| `WM_NOTIFYICON` (`WM_APP+1`) | System tray icon clicks, context menu |
| `WM_CONNECTDEVICE` (`WM_APP+2`) | Auto-reconnect trigger on startup |
| `WM_CONNECTION_CLOSED` (`WM_APP+3`) | StateChanged(Closed) → UI thread marshal (thread safety) |
| `WM_RUNONUITHREAD` (`WM_APP+4`) | Carries a callable posted by `RunOnUiThread`; the WndProc invokes it on the UI thread |
| `WM_CONNECTION_OPENED` (`WM_APP+5`) | StateChanged(Opened) → UI thread marshal; this is the moment audio actually starts flowing |

### Thread Safety (critical invariants)

1. **All XAML object access MUST be on the UI thread.** XAML Islands objects have thread affinity and are not agile. The app runs an STA, so coroutine continuations resume on the UI thread; every XAML-mutating helper additionally re-checks `g_uiThreadId` and re-posts itself through `RunOnUiThread` if it was called from anywhere else. (This only holds because `init_apartment` is given `apartment_type::single_threaded` — its default is MTA, in which case *every* `co_await` continuation lands on a thread pool thread and all XAML updates fail with `RPC_E_WRONG_THREAD`.)
2. **All `g_audioPlaybackConnections` mutations guarded by `g_connectionsMutex`.** Read or write the map without the lock = data race. `g_lastCloseTime` is guarded by the same mutex, and `IsCurrentConnection()` locks internally — never call it with the lock already held.
3. **`StateChanged` callback fires on a Bluetooth/audio background thread.** It must NEVER touch XAML or the map directly. It posts `WM_CONNECTION_CLOSED` and the WndProc handler does the work.
4. **`ConnectDevice` is `fire_and_forget`.** Co-routine resumes happen on the UI thread (STA), so its `UpdateDeviceStatus` calls are safe; the mutex is still held for consistency.
5. **`g_shuttingDown` checked at coroutine entry points** — prevents use-after-free during exit.
6. **Never update XAML while holding `g_connectionsMutex`.** Handlers that walk the connection map (disconnect-all, restart audio, `WM_CONNECTION_CLOSED`) collect the device IDs under the lock, release it, and only then call `UpdateDeviceStatus`. Touching the UI under the lock risks a deadlock and serialises the UI on Bluetooth work.

### Known Physical Limitations

- **Bluetooth + 2.4 GHz Wi-Fi coexistence:** A2DP streaming uses the 2.4 GHz ISM band, shared with Wi-Fi. Interference is expected and inherent to the physical layer.
- **Multi-device Bluetooth congestion:** Simultaneous A2DP connections compete for radio time. `RestoreAudioService` staggers reconnects by 500 ms per device.

### Menu Items

| Item | Action |
|------|--------|
| Bluetooth Settings | Opens `ms-settings:bluetooth` |
| Language ▸ English / 中文 | Switches the UI language (`g_language`), stored in the config |
| View Logs | Opens `AudioPlaybackConnector.log` in the default handler |
| Disconnect All | Closes all connections, updates UI immediately |
| Restart Bluetooth Audio | Close all → wait 1s → stagger-reconnect each device |
| Log Audio Endpoint Inventory | Read-only diagnostic: writes every render endpoint and its level, plus the default endpoint's name/volume/mute state, to the log. Changes nothing |
| Exit | Flyout with "Reconnect on next start" checkbox |
