#include "pch.h"
#include "AudioPlaybackConnector.h"

#include <stdarg.h>
#include <strsafe.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

// Posted with WM_CONNECTION_CLOSED / WM_CONNECTION_OPENED: the device id of the
// connection the state change belongs to, plus the AudioPlaybackConnection
// instance that reported it. Both handlers compare this against the current map
// entry, so a stale message from a previous connection to the same device can
// never touch a newer, still-open one.
struct ConnectionEventInfo
{
	std::wstring deviceId;
	winrt::Windows::Media::Audio::AudioPlaybackConnection connection;
};

// ── Logging ──────────────────────────────────────────────────────────
// Log file: AudioPlaybackConnector.log next to the exe, in the same directory
// as the AudioPlaybackConnector.json config file (UTF-8). Business events
// (connect/disconnect/etc.) go through LogEvent(); WIL's LOG_*/THROW_*
// failures are routed here via wil::SetResultLoggingCallback.

std::wstring GetLogFilePath()
{
	return (GetModuleFsPath(g_hInst).remove_filename() / L"AudioPlaybackConnector.log").wstring();
}

// Append one line to the log file. Thread-safe; never throws.
void WriteLogLine(const wchar_t* text)
{
	try
	{
		static std::mutex s_logMutex;
		std::lock_guard<std::mutex> lock(s_logMutex);

		std::wstring wtext(text);
		std::string utf8 = Utf16ToUtf8(wtext);
		utf8.push_back('\n');

		// FILE_APPEND_DATA makes WriteFile always write at end of file.
		wil::unique_hfile hFile(CreateFileW(GetLogFilePath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		DWORD written = 0;
		THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
	}
	catch (...)
	{
		// Logging must never take the app down.
	}
}

// WIL failure callback: every LOG_*/THROW_* event lands here (possibly on a
// background thread). FailureInfo is copied onto the stack before the call.
// __stdcall matches WIL's callback typedef (needed for the x86 build).
void __stdcall WriteWilDiagnosticsToFile(wil::FailureInfo const& failure) noexcept
{
	wchar_t message[2048];
	if (FAILED(wil::GetFailureLogString(message, ARRAYSIZE(message), failure)))
	{
		StringCchPrintfW(message, ARRAYSIZE(message), L"HRESULT 0x%08X", failure.hr);
	}
	WriteLogLine(message);
}

// Timestamped business-event log entry (wide printf-style format).
void LogEvent(PCWSTR fmt, ...)
{
	wchar_t message[2048];
	va_list args;
	va_start(args, fmt);
	StringCchVPrintfW(message, ARRAYSIZE(message), fmt, args);
	va_end(args);

	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t line[2304];
	StringCchPrintfW(line, ARRAYSIZE(line), L"[%04u-%02u-%02u %02u:%02u:%02u] %s",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, message);
	WriteLogLine(line);
}

// Menu command ids for the Win32 popup menu.
enum : UINT
{
	IDM_BLUETOOTH_SETTINGS = 1,
	IDM_LANG_EN,
	IDM_LANG_ZH,
	IDM_VIEW_LOGS,
	IDM_DISCONNECT_ALL,
	IDM_RESTART_AUDIO,
	IDM_FIX_SILENT,
	IDM_EXIT,
};

// ── UI thread marshalling ────────────────────────────────────────────
// XAML objects are not agile: they belong to the thread that created them, and
// a call from any other thread fails with RPC_E_WRONG_THREAD. Coroutine
// continuations are the danger here — in a multi-threaded apartment they resume
// on whatever thread pool thread completed the async operation, so every XAML
// update that follows a co_await has to be marshalled back through this helper.
// The app runs single-threaded (see wWinMain), which already resumes coroutines
// on the UI thread; this is the belt-and-braces path for anything that still
// arrives from elsewhere.
template <typename Fn>
void RunOnUiThread(Fn&& fn)
{
	if (GetCurrentThreadId() == g_uiThreadId)
	{
		fn();
		return;
	}

	auto task = new std::function<void()>(std::forward<Fn>(fn));
	if (!PostMessageW(g_hWnd, WM_RUNONUITHREAD, 0, reinterpret_cast<LPARAM>(task)))
	{
		LOG_LAST_ERROR();
		delete task;
	}
}

// ── Connection ownership ─────────────────────────────────────────────
// One device can only be tracked by a single AudioPlaybackConnection at a time,
// and the tracked object is what owns the A2DP sink configuration. Everything
// below exists so that (a) the object that actually opened the link is the one
// kept in the map, and (b) cleanup only ever touches a connection the caller
// really owns. Getting either wrong is what produces the "the phone is
// connected but nothing comes out of the speakers" failure mode.

// How long to wait after closing a connection before the same device may be
// connected again. Windows needs a moment to release the old sink endpoint;
// reconnecting into an endpoint that is still being torn down yields a link
// that looks connected but stays silent.
constexpr auto kReconnectCooldown = std::chrono::milliseconds(1500);

// ── "Connected but silent" detection ─────────────────────────────────
// A second, unrelated failure mode looks identical to the user: Windows
// accepts the connection and reports State() == Opened, the phone believes it
// is streaming to the PC, both ends stay silent, and not one audio byte ever
// reaches the speakers. It is intermittent and it affects every A2DP sink
// implementation on Windows (including Microsoft's own Bluetooth Audio
// Receiver); the only known remedy is to tear the connection down and
// renegotiate it. Because AudioPlaybackConnection already reports Opened, the
// connection API cannot tell that state apart from a healthy one — watching the
// output endpoint's signal level is the only way to see it.
//
// Window to watch the default output endpoint for audio right after a
// connection is established. Long enough to cover a phone that starts pushing
// samples late, short enough not to delay the verdict noticeably.
constexpr auto kSilenceProbeWindow = std::chrono::milliseconds(2500);
constexpr auto kSilenceProbeInterval = std::chrono::milliseconds(250);
// Digital silence is exactly 0.0; anything above this is real audio.
constexpr float kAudioPeakThreshold = 0.0001f;
// Automatic reconnect attempts per user-initiated connect. The documented
// workaround occasionally needs a second one, so allow two — no more, so a
// connection that is silent because the phone simply is not playing yet cannot
// turn into an endless reconnect loop.
constexpr int kMaxAutoReconnects = 2;

// True when the map entry for this device is the very connection object passed
// in. Must not be called with g_connectionsMutex held.
bool IsCurrentConnection(std::wstring const& deviceId, AudioPlaybackConnection const& connection)
{
	std::lock_guard<std::mutex> lock(g_connectionsMutex);
	auto it = g_audioPlaybackConnections.find(deviceId);
	return it != g_audioPlaybackConnections.end() && it->second.second == connection;
}

// Remembers the moment a connection was closed so the next connect for the same
// device can wait out kReconnectCooldown.
void MarkDeviceClosed(std::wstring const& deviceId)
{
	std::lock_guard<std::mutex> lock(g_connectionsMutex);
	g_lastCloseTime.insert_or_assign(deviceId, Clock::now());
}

// Waits out whatever is left of kReconnectCooldown for this device, if any.
// Non-zero means the caller should wait that long before connecting again.
std::chrono::milliseconds ReconnectCooldownRemaining(std::wstring const& deviceId)
{
	std::lock_guard<std::mutex> lock(g_connectionsMutex);
	auto last = g_lastCloseTime.find(deviceId);
	if (last == g_lastCloseTime.end()) return std::chrono::milliseconds::zero();

	const auto elapsed = Clock::now() - last->second;
	if (elapsed >= kReconnectCooldown) return std::chrono::milliseconds::zero();

	return std::chrono::duration_cast<std::chrono::milliseconds>(kReconnectCooldown - elapsed);
}

// Peak sample value currently playing on the default render endpoint (0.0 =
// silence, 1.0 = full scale), or a negative number when it cannot be measured
// at all. The two are deliberately different: "could not read the endpoint"
// must not be mistaken for "the endpoint is silent", or every connection would
// look broken on a machine whose audio stack the app cannot query.
//
// COM is already initialised for this thread: the app runs in a single-threaded
// apartment (see wWinMain) and every coroutine continuation resumes on that
// same thread, so no CoInitializeEx call is needed here.
float GetDefaultRenderPeak()
{
	try
	{
		winrt::com_ptr<IMMDeviceEnumerator> enumerator;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			__uuidof(IMMDeviceEnumerator), enumerator.put_void())))
		{
			return -1.0f;
		}

		winrt::com_ptr<IMMDevice> endpoint;
		if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.put())))
		{
			return -1.0f;
		}

		winrt::com_ptr<IAudioMeterInformation> meter;
		if (FAILED(endpoint->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, meter.put_void())))
		{
			return -1.0f;
		}

		float peak = 0.0f;
		if (FAILED(meter->GetPeakValue(&peak)))
		{
			return -1.0f;
		}

		return peak;
	}
	catch (...)
	{
		// Not worth failing over, but worth one line in the log: the caller
		// treats "unmeasurable" as "healthy" so it never reconnects on a guess,
		// and this makes that decision visible when a log is sent in.
		LogEvent(L"Could not read the default output endpoint level");
		return -1.0f;
	}
}

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK FlyoutWndProc(HWND, UINT, WPARAM, LPARAM);
winrt::fire_and_forget ConnectDevice(std::wstring deviceId);
winrt::fire_and_forget ConnectDevice(DeviceInformation device, int autoReconnectAttempt = 0);
void SetupSvgIcon();
void UpdateNotifyIcon();
void DisconnectAllDevices();
void DisconnectDevice(std::wstring const& deviceId);
void ReconnectDeviceNow(std::wstring const& deviceId);
winrt::fire_and_forget RestoreAudioService();
winrt::fire_and_forget ReconnectDevices(std::vector<std::wstring> deviceIds);
void RebuildUi();
void CreateFlyout();
void DestroyFlyout();
void HideDeviceFlyout();
void ShowDeviceFlyout();
winrt::fire_and_forget RefreshDeviceList();
void ApplyFlyoutTexts();
void UpdateFlyoutSubtitle();
void UpdateDeviceStatus(std::wstring_view deviceId, winrt::hstring const& status, DeviceAction action);
bool IsSystemLightTheme();
HMENU BuildPopupMenu();
void HandleMenuCommand(int cmd);

// ── "Connected but silent" recovery ──────────────────────────────────
// Watches the default output endpoint for a moment and reports whether any
// audio showed up. Resolves true as soon as a sample is seen, and also when the
// endpoint cannot be measured at all (see GetDefaultRenderPeak), so that an
// unreadable endpoint never triggers a reconnect.
winrt::IAsyncOperation<bool> WaitForAudioOnOutputEndpoint()
{
	const auto deadline = Clock::now() + kSilenceProbeWindow;

	while (Clock::now() < deadline)
	{
		if (g_shuttingDown) co_return true;

		const float peak = GetDefaultRenderPeak();
		if (peak < 0.0f) co_return true; // unmeasurable: assume healthy
		if (peak > kAudioPeakThreshold) co_return true;

		co_await winrt::resume_after(kSilenceProbeInterval);
	}

	co_return false;
}

// Called after a connect succeeded. When Windows reports the link as open but
// nothing is playing on the output, that is the known intermittent A2DP sink
// failure that only a disconnect/reconnect clears — so do exactly that, up to
// kMaxAutoReconnects times, and give up quietly afterwards (the row then offers
// a manual "Reconnect" button).
winrt::fire_and_forget VerifyAudioAfterConnect(DeviceInformation device, int attempt)
{
	if (g_shuttingDown) co_return;

	// Kept in locals so they outlive the suspension points below.
	const std::wstring deviceId(device.Id());
	const std::wstring deviceName(device.Name().c_str());

	bool opened = false;
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it != g_audioPlaybackConnections.end())
		{
			try
			{
				opened = it->second.second.State() == AudioPlaybackConnectionState::Opened;
			}
			catch (winrt::hresult_error const&)
			{
				LOG_CAUGHT_EXCEPTION();
			}
		}
	}

	// An open that never reached Opened is the same symptom seen from the other
	// side: there is nothing to measure, so go straight to the reconnect.
	bool audio = false;
	if (opened)
	{
		audio = co_await WaitForAudioOnOutputEndpoint();
	}
	if (g_shuttingDown) co_return;

	if (audio)
	{
		LogEvent(L"Audio confirmed on the output endpoint: %s", deviceName.c_str());
		co_return;
	}

	LogEvent(L"No audio on the output endpoint after connecting: %s  (opened=%d, auto reconnect %d/%d)",
		deviceName.c_str(), opened ? 1 : 0, attempt, kMaxAutoReconnects);

	if (attempt >= kMaxAutoReconnects)
	{
		// Out of automatic attempts. The connection is left alone (it is still
		// a valid link the user may want to keep), but the row must stop
		// claiming audio is flowing: the output endpoint measured none, and the
		// "Reconnect" button next to it is the manual way out.
		LogEvent(L"Auto reconnect exhausted for: %s", deviceName.c_str());
		UpdateDeviceStatus(deviceId, _(L"Connected, no audio"), DeviceAction::Disconnect);
		co_return;
	}

	// DisconnectDevice closes the connection, forgets it and arms the reconnect
	// cooldown; ConnectDevice then waits that cooldown out before opening again.
	DisconnectDevice(deviceId);
	co_await winrt::resume_after(kReconnectCooldown);
	if (g_shuttingDown) co_return;

	ConnectDevice(device, attempt);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	wil::SetResultLoggingCallback(WriteWilDiagnosticsToFile);
	LogEvent(L"Application started");

	// XAML islands require a single-threaded apartment. Note that
	// winrt::init_apartment() defaults to apartment_type::multi_threaded: in an
	// MTA the thread has no apartment context for continuations to be sent
	// back to, so every coroutine after a co_await resumes on a thread pool
	// thread and any XAML call from there fails with RPC_E_WRONG_THREAD. An MTA
	// also stops the XAML framework from initialising the island at all.
	winrt::init_apartment(winrt::apartment_type::single_threaded);
	g_uiThreadId = GetCurrentThreadId();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	// No XAML island is created at startup: the tray menu is a plain Win32
	// popup menu, and the device flyout (with its own island) is created on
	// first use. Unlike the system picker it replaced, the flyout window and
	// tree are then kept alive while hidden, so connection state survives
	// between opens instead of being rebuilt from scratch every time.
	LoadSettings();
	ReloadTranslations();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		if (g_flyoutSourceNative2)
		{
			winrt::check_hresult(g_flyoutSourceNative2->PreTranslateMessage(&msg, &processed));
		}
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
	{
		// Signal all async operations to stop — prevents pending
		// coroutines (ConnectDevice, RestoreAudioService) from
		// touching resources we are about to release.
		g_shuttingDown = true;
		LogEvent(L"Application exiting");
		DestroyFlyout();

		// Tear the XAML framework down for this thread, after the island that
		// lives on it is gone.
		if (g_xamlManager)
		{
			try
			{
				g_xamlManager.Close();
			}
			catch (...)
			{
				// Shutting down anyway; never fail the exit path over this.
			}
			g_xamlManager = nullptr;
		}

		// Save settings while we still have the connection list intact
		SaveSettings();

		// Close all audio connections and release endpoints.
		// Each Close() triggers async cleanup via StateChanged →
		// WM_CONNECTION_CLOSED, but since we clear the map after
		// closing, those messages will find nothing and be harmless.
		{
			std::lock_guard<std::mutex> lock(g_connectionsMutex);
			for (auto& pair : g_audioPlaybackConnections)
			{
				pair.second.second.Close();
			}
			g_audioPlaybackConnections.clear();
		}

		// Remove tray icon
		Shell_NotifyIconW(NIM_DELETE, &g_nid);

		// Allow time for async audio endpoint cleanup to propagate
		// before the process exits. Without this delay, the OS may
		// not release endpoints in time, leaving zombie entries that
		// require a reboot to recover.
		Sleep(500);

		PostQuitMessage(0);
		break;
	}
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			// A click on the tray icon toggles the flyout. Showing it steals
			// focus, so the next click deactivates it (light dismiss) before
			// this handler runs — hence the "just closed" check, which turns
			// that click into a close instead of an immediate reopen.
			const bool justClosed = g_flyoutHiddenTick != 0 && GetTickCount64() - g_flyoutHiddenTick < 300;
			if (g_flyoutVisible || justClosed)
			{
				HideDeviceFlyout();
			}
			else
			{
				ShowDeviceFlyout();
			}
		}
		break;
		case WM_CONTEXTMENU:
		{
			// The tray callback does not carry coordinates in wParam/lParam
			// (lParam is the WM_CONTEXTMENU value itself), so use the real
			// cursor position to place the popup menu.
			POINT pt;
			GetCursorPos(&pt);

			HMENU menu = BuildPopupMenu();
			SetForegroundWindow(hWnd);
			const int cmd = static_cast<int>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr));
			DestroyMenu(menu);

			if (cmd)
			{
				HandleMenuCommand(cmd);
			}
		}
		break;
		}
		break;
	case WM_RUNONUITHREAD:
	{
		// A callable posted by RunOnUiThread. This thread owns every XAML
		// object, so it is the only one allowed to run it.
		std::unique_ptr<std::function<void()>> task(reinterpret_cast<std::function<void()>*>(lParam));
		if (task && *task)
		{
			(*task)();
		}
		break;
	}
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			// Stagger reconnects so simultaneous requests don't flood the
			// Bluetooth stack (same reasoning as RestoreAudioService).
			ReconnectDevices(std::move(g_lastDevices));
		}
		break;
	case WM_CONNECTION_CLOSED:
	{
		// StateChanged callback runs on an audio/Bluetooth background thread.
		// It posts this message so all XAML UI updates and map mutations happen
		// on the main UI thread, avoiding cross-thread races.
		std::unique_ptr<ConnectionEventInfo> info(reinterpret_cast<ConnectionEventInfo*>(wParam));
		std::wstring closedDeviceId;
		{
			std::lock_guard<std::mutex> lock(g_connectionsMutex);
			auto it = g_audioPlaybackConnections.find(info->deviceId);
			// Only remove the entry if the connection that closed is still the
			// one in the map. A stale message from an earlier connection to the
			// same device must not drop a newer, still-open connection.
			if (it != g_audioPlaybackConnections.end() && it->second.second == info->connection)
			{
				LogEvent(L"Disconnected: %s", it->second.first.Name().c_str());
				closedDeviceId = it->second.first.Id().c_str();
				g_audioPlaybackConnections.erase(it);
			}
		}

		if (!closedDeviceId.empty())
		{
			// Start the reconnect cooldown from the moment the link went away:
			// reconnecting into an endpoint that is still being released is how
			// a phone ends up "connected" with no audio.
			MarkDeviceClosed(closedDeviceId);

			// Update the flyout outside the lock: calling into XAML while
			// holding g_connectionsMutex risks a deadlock if a layout pass
			// re-enters.
			UpdateDeviceStatus(closedDeviceId, _(L"Not connected"), DeviceAction::Connect);
		}
		break;
	}
	case WM_CONNECTION_OPENED:
	{
		// StateChanged(Opened): the sink is really up and the phone's audio is
		// now routed here. Until this arrives, a successful OpenAsync only means
		// the request was accepted — the phone can be connected at the
		// Bluetooth level with nothing actually streaming.
		std::unique_ptr<ConnectionEventInfo> info(reinterpret_cast<ConnectionEventInfo*>(wParam));
		std::wstring openedDeviceId;
		std::wstring openedDeviceName;
		{
			std::lock_guard<std::mutex> lock(g_connectionsMutex);
			auto it = g_audioPlaybackConnections.find(info->deviceId);
			if (it != g_audioPlaybackConnections.end() && it->second.second == info->connection)
			{
				openedDeviceName = it->second.first.Name().c_str();
				openedDeviceId = info->deviceId;
			}
		}

		if (!openedDeviceId.empty())
		{
			LogEvent(L"Audio flowing: %s", openedDeviceName.c_str());
			UpdateDeviceStatus(openedDeviceId, _(L"Connected"), DeviceAction::Disconnect);
		}
		break;
	}
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

// ── Device flyout (self-drawn) ───────────────────────────────────────
// The system device picker (Windows.Devices.Enumeration.DevicePicker) can only
// set a title, a few colors and a per-row status text — it exposes no way to
// host custom controls, so a refresh indicator and a refresh button cannot live
// there. The list is therefore drawn by this app: a dedicated popup window
// hosts its own XAML island.
//
// The window and the XAML tree are created on first use and then kept alive
// (hidden). Rebuilding the tree on every open used to lose all row state; with
// a persistent tree, connection status survives between opens.

using winrt::Windows::UI::Xaml::Media::SolidColorBrush;

namespace
{
	constexpr wchar_t kFlyoutClassName[] = L"AudioPlaybackConnectorFlyout";
	constexpr float kFlyoutWidthDip = 340.0f;

	// The flyout height is computed from its contents rather than measured: the
	// island lays the XAML tree out itself, and interleaving the app's own
	// Measure pass with that layout is fragile. Header, indicator and row
	// heights are therefore fixed, which makes the sum exact.
	constexpr int kFlyoutHeaderHeightDip = 60; // title + subtitle + refresh button
	constexpr int kFlyoutIndicatorHeightDip = 3; // separator line / refresh indicator
	constexpr int kFlyoutRowHeightDip = 56;      // one device row, divider included
	constexpr int kFlyoutMaxListDip = 392;       // list is capped at this and scrolls beyond
	constexpr int kFlyoutEmptyListDip = 46;      // "No devices found" line

	// Colors are baked into the XAML instead of using ThemeResource: the island
	// has no Application object, so theme resources do not reliably resolve.
	// They follow the system app theme — the same registry value the tray icon
	// already uses.
	struct FlyoutPalette
	{
		const wchar_t* surface;
		const wchar_t* border;
		const wchar_t* textPrimary;
		const wchar_t* textSecondary;
		const wchar_t* accent;
		const wchar_t* track;
		const wchar_t* connected;
		const wchar_t* danger;
	};

	const FlyoutPalette kFlyoutLight = { L"#F9F9F9", L"#E5E5E5", L"#1A1A1A", L"#616161", L"#0078D4", L"#E5E5E5", L"#107C41", L"#C42B1C" };
	const FlyoutPalette kFlyoutDark = { L"#202020", L"#3A3A3A", L"#FFFFFF", L"#A0A0A0", L"#60CDFF", L"#3A3A3A", L"#6CCB9F", L"#FF99A4" };
}

bool IsSystemLightTheme()
{
	DWORD value = 0, cbValue = sizeof(value);
	if (FAILED(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue)))
	{
		// Unreadable: assume light, matching the system default.
		return true;
	}
	return value != 0;
}

FlyoutPalette const& CurrentFlyoutPalette()
{
	return IsSystemLightTheme() ? kFlyoutLight : kFlyoutDark;
}

winrt::Windows::UI::Color ColorFromHex(std::wstring_view hex)
{
	auto nibble = [](wchar_t c) -> uint8_t
	{
		if (c >= L'0' && c <= L'9') return static_cast<uint8_t>(c - L'0');
		if (c >= L'a' && c <= L'f') return static_cast<uint8_t>(c - L'a' + 10);
		if (c >= L'A' && c <= L'F') return static_cast<uint8_t>(c - L'A' + 10);
		return 0;
	};

	winrt::Windows::UI::Color color{};
	color.A = 255;
	if (hex.size() >= 7 && hex[0] == L'#')
	{
		color.R = static_cast<uint8_t>((nibble(hex[1]) << 4) | nibble(hex[2]));
		color.G = static_cast<uint8_t>((nibble(hex[3]) << 4) | nibble(hex[4]));
		color.B = static_cast<uint8_t>((nibble(hex[5]) << 4) | nibble(hex[6]));
	}
	return color;
}

SolidColorBrush SolidBrush(std::wstring_view hex)
{
	auto brush = SolidColorBrush();
	brush.Color(ColorFromHex(hex));
	return brush;
}

// Sizes the island's own child window to the flyout's client area.
//
// This is not optional: the island starts out at 0x0 and the framework never
// makes it follow the host window (verified on Windows 11 — resizing the host
// leaves the island at 1x1), so without this call the flyout window opens and
// stays completely unpainted, which looks exactly like "the tray click did
// nothing". UI thread only.
void ResizeFlyoutXamlHost()
{
	if (!g_hWndFlyout || !g_hWndFlyoutXaml) return;

	RECT client{};
	if (!GetClientRect(g_hWndFlyout, &client)) return;

	const int width = client.right - client.left;
	const int height = client.bottom - client.top;
	if (width <= 0 || height <= 0) return;

	SetWindowPos(g_hWndFlyoutXaml, nullptr, 0, 0, width, height,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Height the flyout needs for the given number of device rows, in pixels.
int FlyoutHeightForRows(int rowCount, UINT dpi)
{
	int listDip = kFlyoutEmptyListDip;
	if (rowCount > 0)
	{
		listDip = rowCount * kFlyoutRowHeightDip;
		if (listDip > kFlyoutMaxListDip) listDip = kFlyoutMaxListDip;
	}

	int height = MulDiv(kFlyoutHeaderHeightDip + kFlyoutIndicatorHeightDip + listDip, dpi, USER_DEFAULT_SCREEN_DPI);
	const int minHeight = MulDiv(96, dpi, USER_DEFAULT_SCREEN_DPI);
	const int maxHeight = MulDiv(560, dpi, USER_DEFAULT_SCREEN_DPI);
	if (height < minHeight) height = minHeight;
	if (height > maxHeight) height = maxHeight;
	return height;
}

// Keeps the window height in sync with the row count after a refresh. The
// bottom edge stays put, so the flyout grows upwards from the tray icon
// instead of sliding under the taskbar. UI thread only.
void UpdateFlyoutHeight()
{
	if (!g_flyoutVisible || !g_hWndFlyout) return;

	UINT dpi = GetDpiForWindow(g_hWndFlyout);
	if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

	RECT rect{};
	if (!GetWindowRect(g_hWndFlyout, &rect)) return;

	const int width = rect.right - rect.left;
	const int target = FlyoutHeightForRows(static_cast<int>(g_deviceRows.size()), dpi);
	if (target == rect.bottom - rect.top) return;

	SetWindowPos(g_hWndFlyout, nullptr, rect.left, rect.bottom - target, width, target,
		SWP_NOZORDER | SWP_NOACTIVATE);
	ResizeFlyoutXamlHost();
}

// Layout: header (title + subtitle + refresh button) → refresh indicator →
// scrolling device list. The indicator sits in a 3px border that stays visible
// even when idle, so showing/hiding it never shifts the list.
std::wstring BuildFlyoutXaml(FlyoutPalette const& p)
{
	std::wstring xaml = LR"XAML(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Background="{{SURFACE}}">
  <Grid.RowDefinitions>
    <RowDefinition Height="Auto"/>
    <RowDefinition Height="Auto"/>
    <RowDefinition Height="*"/>
  </Grid.RowDefinitions>
  <Grid Grid.Row="0" Height="60" Padding="14,10,10,10">
    <Grid.ColumnDefinitions>
      <ColumnDefinition Width="*"/>
      <ColumnDefinition Width="Auto"/>
    </Grid.ColumnDefinitions>
    <StackPanel Grid.Column="0" VerticalAlignment="Center">
      <TextBlock x:Name="TitleText" FontSize="14" FontWeight="SemiBold" Foreground="{{TEXT_PRIMARY}}" TextTrimming="CharacterEllipsis"/>
      <TextBlock x:Name="SubtitleText" FontSize="12" Margin="0,3,0,0" Foreground="{{TEXT_SECONDARY}}" TextTrimming="CharacterEllipsis"/>
    </StackPanel>
    <Button x:Name="RefreshButton" Grid.Column="1" VerticalAlignment="Center" Padding="10,4,10,5">
      <StackPanel Orientation="Horizontal" Spacing="6">
        <FontIcon FontFamily="Segoe MDL2 Assets" Glyph="&#xE72C;" FontSize="12"/>
        <TextBlock x:Name="RefreshLabel" FontSize="12" VerticalAlignment="Center"/>
      </StackPanel>
    </Button>
  </Grid>
  <Border Grid.Row="1" Height="3" Background="{{TRACK}}">
    <ProgressBar x:Name="RefreshProgress" IsIndeterminate="True" Minimum="0" Maximum="100" Height="3"
                 Background="Transparent" Foreground="{{ACCENT}}" BorderThickness="0" Padding="0"
                 HorizontalAlignment="Stretch" VerticalAlignment="Stretch"/>
  </Border>
  <ScrollViewer Grid.Row="2" VerticalScrollBarVisibility="Auto" HorizontalScrollMode="Disabled" MaxHeight="392" Padding="0,0,0,6">
    <StackPanel x:Name="DeviceList"/>
  </ScrollViewer>
</Grid>)XAML";

	auto replace = [&xaml](std::wstring_view token, std::wstring_view value)
	{
		for (size_t pos = xaml.find(token); pos != std::wstring::npos; pos = xaml.find(token, pos + value.size()))
		{
			xaml.replace(pos, token.size(), value);
		}
	};

	replace(L"{{SURFACE}}", p.surface);
	replace(L"{{TEXT_PRIMARY}}", p.textPrimary);
	replace(L"{{TEXT_SECONDARY}}", p.textSecondary);
	replace(L"{{ACCENT}}", p.accent);
	replace(L"{{TRACK}}", p.track);

	return xaml;
}

void ApplyFlyoutTexts()
{
	// XAML only — hop back to the UI thread if a coroutine called us.
	if (GetCurrentThreadId() != g_uiThreadId)
	{
		RunOnUiThread([] { ApplyFlyoutTexts(); });
		return;
	}

	if (!g_flyoutRoot) return;

	try
	{
		if (g_flyoutTitle) g_flyoutTitle.Text(_(L"Bluetooth Audio Devices"));
		if (g_flyoutRefreshLabel) g_flyoutRefreshLabel.Text(_(L"Refresh"));
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}

	UpdateFlyoutSubtitle();
}

void UpdateFlyoutSubtitle()
{
	// XAML only — hop back to the UI thread if a coroutine called us.
	if (GetCurrentThreadId() != g_uiThreadId)
	{
		RunOnUiThread([] { UpdateFlyoutSubtitle(); });
		return;
	}

	if (!g_flyoutRoot || !g_flyoutSubtitle) return;

	try
	{
		if (g_flyoutRefreshing)
		{
			g_flyoutSubtitle.Text(_(L"Checking connection status"));
			return;
		}

		unsigned connected = 0;
		for (auto const& row : g_deviceRows)
		{
			if (row.state && row.state->action == DeviceAction::Disconnect)
			{
				++connected;
			}
		}

		wchar_t text[64];
		swprintf(text, ARRAYSIZE(text), _(L"%d connected"), static_cast<int>(connected));
		g_flyoutSubtitle.Text(text);
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

void SetFlyoutRefreshing(bool refreshing)
{
	// XAML only — hop back to the UI thread if a coroutine called us.
	if (GetCurrentThreadId() != g_uiThreadId)
	{
		RunOnUiThread([refreshing] { SetFlyoutRefreshing(refreshing); });
		return;
	}

	g_flyoutRefreshing = refreshing;

	if (g_flyoutRoot)
	{
		try
		{
			if (g_flyoutProgressBar) g_flyoutProgressBar.Visibility(refreshing ? Visibility::Visible : Visibility::Collapsed);
			if (g_flyoutRefreshButton) g_flyoutRefreshButton.IsEnabled(!refreshing);
		}
		catch (winrt::hresult_error const&)
		{
			LOG_CAUGHT_EXCEPTION();
		}
	}

	UpdateFlyoutSubtitle();
}

winrt::hstring ActionLabel(DeviceAction action)
{
	switch (action)
	{
	case DeviceAction::Disconnect: return _(L"Disconnect");
	case DeviceAction::Retry: return _(L"Retry");
	case DeviceAction::Connect: return _(L"Connect");
	default: return {};
	}
}

SolidColorBrush StatusBrush(DeviceAction action)
{
	auto const& palette = CurrentFlyoutPalette();
	switch (action)
	{
	case DeviceAction::Disconnect: return SolidBrush(palette.connected);
	case DeviceAction::Retry: return SolidBrush(palette.danger);
	default: return SolidBrush(palette.textSecondary);
	}
}

// Reads the authoritative connection state of a device. Some Bluetooth
// adapters do not report the property at all; the callers fall back to the
// app's own connection map in that case.
bool IsDeviceConnected(DeviceInformation const& device)
{
	try
	{
		auto properties = device.Properties();
		if (properties && properties.HasKey(L"System.Devices.Aep.IsConnected"))
		{
			return winrt::unbox_value_or<bool>(properties.Lookup(L"System.Devices.Aep.IsConnected"), false);
		}
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
	return false;
}

// `connected` is "the device is linked" (which the system may report even when
// this app never managed to open the audio stream); `streaming` is "audio is
// actually flowing through this app's connection"; `managed` is "this app holds
// the connection object". They differ often enough that the row has to show all
// three, otherwise a silent link looks healthy.
void AppendDeviceRow(DeviceInformation const& device, bool connected, bool streaming, bool managed)
{
	auto const& palette = CurrentFlyoutPalette();

	auto nameText = TextBlock();
	nameText.FontSize(13);
	nameText.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
	nameText.Foreground(SolidBrush(palette.textPrimary));
	nameText.TextTrimming(TextTrimming::CharacterEllipsis);
	nameText.Text(device.Name());

	auto statusText = TextBlock();
	statusText.FontSize(12);
	statusText.Margin(Thickness{ 0, 3, 0, 0 });
	statusText.TextTrimming(TextTrimming::CharacterEllipsis);
	statusText.Foreground(StatusBrush(!connected ? DeviceAction::Connect : (streaming ? DeviceAction::Disconnect : DeviceAction::Retry)));
	statusText.Text(!connected ? _(L"Not connected") : (streaming ? _(L"Connected") : _(L"Connected, no audio")));

	auto textPanel = StackPanel();
	textPanel.VerticalAlignment(VerticalAlignment::Center);
	textPanel.Children().Append(nameText);
	textPanel.Children().Append(statusText);

	// The click handler captures only this shared state, so updating a row's
	// action later (connect → disconnect, or an error → retry) does not require
	// rebuilding the row.
	//
	// Only a connection this app manages can be closed by this app, so that is
	// the only case offering Disconnect. A device that is merely linked (the
	// phone is paired and connected, but no app has opened the audio stream)
	// offers Connect instead — otherwise the user is stuck looking at
	// "connected" with no way to actually get sound.
	auto state = std::make_shared<DeviceRowState>();
	state->deviceId = std::wstring(device.Id());
	state->action = (connected && managed) ? DeviceAction::Disconnect : DeviceAction::Connect;

	auto actionButton = Button();
	actionButton.FontSize(12);
	actionButton.Padding(Thickness{ 12, 4, 12, 5 });
	actionButton.MinWidth(76);
	actionButton.VerticalAlignment(VerticalAlignment::Center);
	actionButton.Content(winrt::box_value(ActionLabel(state->action)));
	actionButton.Click([state](auto&&, auto&&)
	{
		switch (state->action)
		{
		case DeviceAction::Disconnect:
			DisconnectDevice(state->deviceId);
			break;
		case DeviceAction::Connect:
		case DeviceAction::Retry:
			ConnectDevice(state->deviceId);
			break;
		default:
			break;
		}
	});

	// The manual form of the "connected but silent" workaround: Windows can
	// report a perfectly healthy link that never carries a single audio byte,
	// and tearing the connection down and opening it again is the only known
	// remedy. Offered only while this app owns the connection, because nobody
	// else can renegotiate it.
	const std::wstring rowDeviceId(device.Id());
	auto reconnectButton = Button();
	reconnectButton.FontSize(12);
	reconnectButton.Padding(Thickness{ 10, 4, 10, 5 });
	reconnectButton.MinWidth(64);
	reconnectButton.Margin(Thickness{ 0, 0, 8, 0 });
	reconnectButton.VerticalAlignment(VerticalAlignment::Center);
	reconnectButton.Content(winrt::box_value(winrt::hstring(_(L"Reconnect"))));
	reconnectButton.Visibility(managed ? Visibility::Visible : Visibility::Collapsed);
	reconnectButton.Click([rowDeviceId](auto&&, auto&&) { ReconnectDeviceNow(rowDeviceId); });

	auto row = Grid();
	row.Padding(Thickness{ 14, 8, 14, 8 });

	auto nameColumn = ColumnDefinition();
	nameColumn.Width(GridLength{ 1.0, GridUnitType::Star });
	auto reconnectColumn = ColumnDefinition();
	reconnectColumn.Width(GridLength{ 0.0, GridUnitType::Auto });
	auto actionColumn = ColumnDefinition();
	actionColumn.Width(GridLength{ 0.0, GridUnitType::Auto });
	row.ColumnDefinitions().Append(nameColumn);
	row.ColumnDefinitions().Append(reconnectColumn);
	row.ColumnDefinitions().Append(actionColumn);

	Grid::SetColumn(textPanel, 0);
	Grid::SetColumn(reconnectButton, 1);
	Grid::SetColumn(actionButton, 2);
	row.Children().Append(textPanel);
	row.Children().Append(reconnectButton);
	row.Children().Append(actionButton);

	auto divider = Border();
	// Fixed height so the window height (FlyoutHeightForRows) is exact.
	divider.Height(kFlyoutRowHeightDip);
	divider.BorderThickness(Thickness{ 0, 1, 0, 0 });
	divider.BorderBrush(SolidBrush(palette.border));
	divider.Child(row);

	g_flyoutDeviceList.Children().Append(divider);
	g_deviceRows.push_back(DeviceRow{ state->deviceId, statusText, reconnectButton, actionButton, state });
}

void RebuildDeviceRows(std::vector<DeviceInformation> devices)
{
	// XAML only — hop back to the UI thread if a coroutine called us.
	if (GetCurrentThreadId() != g_uiThreadId)
	{
		RunOnUiThread([devices = std::move(devices)]() mutable { RebuildDeviceRows(std::move(devices)); });
		return;
	}

	// Device id → is the audio stream actually open? An entry exists as soon as
	// this app tracks the device, and the mapped value is the connection's own
	// state — the only thing that tells "connected" apart from "connected, but
	// no audio is flowing".
	std::unordered_map<std::wstring, bool> managed;
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		managed.reserve(g_audioPlaybackConnections.size());
		for (auto const& entry : g_audioPlaybackConnections)
		{
			// State() can throw once the object has been closed underneath us;
			// that only means "not streaming".
			bool opened = false;
			try
			{
				opened = entry.second.second.State() == AudioPlaybackConnectionState::Opened;
			}
			catch (winrt::hresult_error const&)
			{
				LOG_CAUGHT_EXCEPTION();
			}
			managed.emplace(entry.first, opened);
		}
	}

	g_flyoutDeviceList.Children().Clear();
	g_deviceRows.clear();

	auto const& palette = CurrentFlyoutPalette();

	if (devices.empty())
	{
		auto emptyText = TextBlock();
		emptyText.FontSize(12);
		emptyText.Height(kFlyoutEmptyListDip);
		emptyText.VerticalAlignment(VerticalAlignment::Center);
		emptyText.Margin(Thickness{ 14, 0, 14, 0 });
		emptyText.Foreground(SolidBrush(palette.textSecondary));
		emptyText.Text(_(L"No devices found"));
		g_flyoutDeviceList.Children().Append(emptyText);
		UpdateFlyoutSubtitle();
		UpdateFlyoutHeight();
		return;
	}

	// Connected devices first, so the ones in use are always at the top. The
	// enumeration order is preserved within each group.
	struct RowSeed
	{
		bool connected;
		bool streaming;
		bool managed;
		DeviceInformation device;
	};

	std::vector<RowSeed> rows;
	rows.reserve(devices.size());
	for (auto const& device : devices)
	{
		const std::wstring deviceId(device.Id());
		auto tracked = managed.find(deviceId);
		const bool byApp = tracked != managed.end();

		// A device this app does not track is still "linked" when the system
		// reports it as connected — that alone never produces sound, because
		// no object of ours has configured the PC as a sink for it. The row
		// says so and offers Connect instead of pretending it is fine.
		rows.push_back(RowSeed{
			byApp || IsDeviceConnected(device),
			byApp && tracked->second,
			byApp,
			device });
	}
	std::stable_sort(rows.begin(), rows.end(), [](RowSeed const& a, RowSeed const& b) { return a.connected && !b.connected; });

	for (auto const& row : rows)
	{
		AppendDeviceRow(row.device, row.connected, row.streaming, row.managed);
	}

	UpdateFlyoutSubtitle();
	UpdateFlyoutHeight();
}

void CreateFlyout()
{
	if (g_flyoutRoot) return;

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = FlyoutWndProc,
		.hInstance = g_hInst,
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = kFlyoutClassName
	};
	RegisterClassExW(&wcex); // 0 when already registered — nothing to do

	// No WS_EX_LAYERED here: XAML Islands content does not render inside a
	// layered window, which is also why the tray window (alpha 0) cannot host
	// the flyout. WS_EX_TOOLWINDOW keeps it out of Alt+Tab.
	g_hWndFlyout = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kFlyoutClassName, nullptr, WS_POPUP,
		0, 0, 10, 10, nullptr, nullptr, g_hInst, nullptr);
	if (!g_hWndFlyout)
	{
		LOG_LAST_ERROR();
		return;
	}

	try
	{
		// The XAML framework's core window has to exist for this thread before
		// the first island is created on it.
		if (!g_xamlManager)
		{
			g_xamlManager = WindowsXamlManager::InitializeForCurrentThread();
		}

		g_flyoutSource = DesktopWindowXamlSource();
		g_flyoutSourceNative2 = g_flyoutSource.as<IDesktopWindowXamlSourceNative2>();
		winrt::check_hresult(g_flyoutSourceNative2->AttachToWindow(g_hWndFlyout));

		// The child window that actually paints the XAML. Kept so it can be
		// resized with the flyout (see ResizeFlyoutXamlHost).
		winrt::check_hresult(g_flyoutSourceNative2->get_WindowHandle(&g_hWndFlyoutXaml));

		auto root = winrt::Windows::UI::Xaml::Markup::XamlReader::Load(BuildFlyoutXaml(CurrentFlyoutPalette())).as<Grid>();
		g_flyoutRoot = root;
		g_flyoutTitle = root.FindName(L"TitleText").as<TextBlock>();
		g_flyoutSubtitle = root.FindName(L"SubtitleText").as<TextBlock>();
		g_flyoutRefreshButton = root.FindName(L"RefreshButton").as<Button>();
		g_flyoutRefreshLabel = root.FindName(L"RefreshLabel").as<TextBlock>();
		g_flyoutProgressBar = root.FindName(L"RefreshProgress").as<ProgressBar>();
		g_flyoutDeviceList = root.FindName(L"DeviceList").as<StackPanel>();

		g_flyoutRefreshButton.Click([](auto&&, auto&&) { RefreshDeviceList(); });

		g_flyoutSource.Content(root);
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
		DestroyFlyout();
		return;
	}

	// The island starts at 0x0 and never follows the host window by itself:
	// without this the flyout would open and stay completely unpainted.
	ResizeFlyoutXamlHost();

	ApplyFlyoutTexts();
	SetFlyoutRefreshing(false);
}

void DestroyFlyout()
{
	g_flyoutVisible = false;
	g_deviceRows.clear();
	g_flyoutDeviceList = nullptr;
	g_flyoutProgressBar = nullptr;
	g_flyoutRefreshLabel = nullptr;
	g_flyoutRefreshButton = nullptr;
	g_flyoutSubtitle = nullptr;
	g_flyoutTitle = nullptr;
	g_flyoutRoot = nullptr;

	if (g_flyoutSource)
	{
		try
		{
			g_flyoutSource.Content(nullptr);
		}
		catch (...)
		{
			// The island may already be partially torn down; never let
			// teardown crash the app.
		}
		g_flyoutSource = nullptr;
	}
	g_flyoutSourceNative2 = nullptr;
	g_hWndFlyoutXaml = nullptr; // goes away with its host window below

	if (g_hWndFlyout)
	{
		DestroyWindow(g_hWndFlyout);
		g_hWndFlyout = nullptr;
	}
}

void HideDeviceFlyout()
{
	if (!g_flyoutVisible) return;

	g_flyoutVisible = false;
	// Remember when the flyout went away: the click that closes it must not be
	// mistaken for a click that opens it again.
	g_flyoutHiddenTick = GetTickCount64();

	if (g_hWndFlyout)
	{
		ShowWindow(g_hWndFlyout, SW_HIDE);
	}
}

LRESULT CALLBACK FlyoutWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			// Light dismiss: hide as soon as focus moves anywhere else.
			HideDeviceFlyout();
		}
		break;
	case WM_SIZE:
		// The island never resizes itself with its host (see
		// ResizeFlyoutXamlHost), so every size change has to be forwarded.
		ResizeFlyoutXamlHost();
		break;
	case WM_CLOSE:
		HideDeviceFlyout();
		return 0;
	}
	return DefWindowProcW(hWnd, message, wParam, lParam);
}

void ShowDeviceFlyout()
{
	CreateFlyout();
	if (!g_flyoutRoot) return;

	try
	{
		ApplyFlyoutTexts();

		UINT dpi = GetDpiForWindow(g_hWndFlyout);
		if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

		// windows.h defines min/max macros, so std::clamp/std::max would be
		// mangled in this translation unit; clamp explicitly instead.
		auto clampTo = [](int value, int low, int high) { return value < low ? low : (value > high ? high : value); };

		const int width = MulDiv(static_cast<int>(kFlyoutWidthDip), dpi, USER_DEFAULT_SCREEN_DPI);
		// Height comes from the fixed header/row metrics rather than from a
		// Measure pass: the island owns the layout of its tree, and interleaving
		// our own measure with it is fragile.
		const int height = FlyoutHeightForRows(static_cast<int>(g_deviceRows.size()), dpi);

		// Anchor to the tray icon: above it and right-aligned, like a tray flyout.
		RECT icon{};
		if (FAILED(Shell_NotifyIconGetRect(&g_niid, &icon)))
		{
			LOG_LAST_ERROR();
			POINT cursor{};
			GetCursorPos(&cursor);
			icon = { cursor.x, cursor.y, cursor.x + 1, cursor.y + 1 };
		}

		int x = icon.right - width;
		int y = icon.top - height - MulDiv(8, dpi, USER_DEFAULT_SCREEN_DPI);

		MONITORINFO monitor{ sizeof(monitor) };
		if (GetMonitorInfoW(MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST), &monitor))
		{
			const RECT& work = monitor.rcWork;
			const int lowestX = static_cast<int>(work.left);
			const int lowestY = static_cast<int>(work.top);
			const int highestX = static_cast<int>(work.right) - width;
			const int highestY = static_cast<int>(work.bottom) - height;
			x = clampTo(x, lowestX, highestX < lowestX ? lowestX : highestX);
			y = clampTo(y, lowestY, highestY < lowestY ? lowestY : highestY);
		}

		SetWindowPos(g_hWndFlyout, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
		// Give the island the new client size, otherwise the window opens empty.
		ResizeFlyoutXamlHost();
		SetForegroundWindow(g_hWndFlyout);
		g_flyoutVisible = true;

		// Detect the connection state of every device in the list, so an
		// already-connected device is shown as connected.
		RefreshDeviceList();
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

winrt::fire_and_forget RefreshDeviceList()
{
	if (!g_flyoutRoot) co_return;

	SetFlyoutRefreshing(true);

	try
	{
		std::vector<winrt::hstring> requestedProperties{ L"System.Devices.Aep.IsConnected" };
		auto devices = co_await DeviceInformation::FindAllAsync(
			AudioPlaybackConnection::GetDeviceSelector(),
			winrt::single_threaded_vector(std::move(requestedProperties)));

		if (g_shuttingDown) co_return;

		std::vector<DeviceInformation> list;
		list.reserve(devices.Size());
		for (auto const& device : devices)
		{
			list.push_back(device);
		}

		RebuildDeviceRows(std::move(list));
	}
	catch (winrt::hresult_error const&)
	{
		// Enumeration can fail (no radio, session issues). Leave whatever is
		// already on screen and just stop the indicator.
		LOG_CAUGHT_EXCEPTION();
	}
	catch (...)
	{
	}

	if (g_shuttingDown) co_return;
	SetFlyoutRefreshing(false);
}

void UpdateDeviceStatus(std::wstring_view deviceId, winrt::hstring const& status, DeviceAction action)
{
	// XAML only — hop back to the UI thread if a coroutine called us.
	if (GetCurrentThreadId() != g_uiThreadId)
	{
		std::wstring id(deviceId);
		winrt::hstring text(status);
		RunOnUiThread([id = std::move(id), text = std::move(text), action]
		{
			UpdateDeviceStatus(id, text, action);
		});
		return;
	}

	if (g_flyoutRoot)
	{
		try
		{
			for (auto& row : g_deviceRows)
			{
				if (std::wstring_view(row.deviceId) != deviceId) continue;

				if (row.state)
				{
					row.state->action = action;
				}
				if (row.statusText)
				{
					row.statusText.Text(status);
					row.statusText.Foreground(StatusBrush(action));
				}
				if (row.actionButton)
				{
					row.actionButton.Content(winrt::box_value(ActionLabel(action)));
					row.actionButton.IsEnabled(action != DeviceAction::None);
				}
				if (row.reconnectButton)
				{
					// Owning a live connection is the only case where a
					// reconnect means anything; every other state is served by
					// the action button next to it.
					row.reconnectButton.Visibility(action == DeviceAction::Disconnect
						? Visibility::Visible : Visibility::Collapsed);
				}
				break;
			}
		}
		catch (winrt::hresult_error const&)
		{
			// A stale status update from a coroutine must never crash the app.
			LOG_CAUGHT_EXCEPTION();
		}
	}

	UpdateFlyoutSubtitle();
}

void DisconnectDevice(std::wstring const& deviceId)
{
	bool closed = false;
	std::wstring closedName;
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it != g_audioPlaybackConnections.end())
		{
			closedName = it->second.first.Name().c_str();
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
			closed = true;
			// StateChanged → WM_CONNECTION_CLOSED handles async endpoint
			// cleanup on the UI thread (and finds nothing, the entry is gone).
		}
	}

	// A manual disconnect arms the reconnect cooldown too: an immediate
	// reconnect would land on an endpoint that has not been released yet.
	MarkDeviceClosed(deviceId);

	if (closed)
	{
		LogEvent(L"Disconnected by request: %s", closedName.c_str());
	}
	else
	{
		// The link was established outside this app (for example from Windows
		// Settings), so it is not in our map. Create a throwaway connection
		// object for the device and close it so the button still does something.
		try
		{
			if (auto connection = AudioPlaybackConnection::TryCreateFromId(winrt::hstring(deviceId.c_str())))
			{
				connection.Close();
				LogEvent(L"Disconnect (external link): %s", deviceId.c_str());
			}
		}
		catch (winrt::hresult_error const&)
		{
			LOG_CAUGHT_EXCEPTION();
		}
	}

	UpdateDeviceStatus(deviceId, _(L"Not connected"), DeviceAction::Connect);
}

// The one-click equivalent of the manual workaround for the intermittent
// "connected but silent" failure: close the link, give Windows a moment to
// release the sink endpoint, then open the device again. DisconnectDevice arms
// the cooldown and ConnectDevice waits it out, so the two calls in a row are
// exactly the sequence the workaround prescribes.
void ReconnectDeviceNow(std::wstring const& deviceId)
{
	LogEvent(L"Reconnect by request: %s", deviceId.c_str());
	DisconnectDevice(deviceId);
	ConnectDevice(deviceId);
}

void ShowExitConfirmation()
{
	bool hasConnections;
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		hasConnections = !g_audioPlaybackConnections.empty();
	}
	if (!hasConnections)
	{
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
		return;
	}

	TASKDIALOGCONFIG config{};
	config.cbSize = sizeof(config);
	config.hwndParent = g_hWnd;
	config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
	config.pszWindowTitle = L"AudioPlaybackConnector";
	config.pszMainIcon = TD_INFORMATION_ICON;
	config.pszMainInstruction = _(L"All connections will be closed.\nExit anyway?");
	config.pszVerificationText = _(L"Reconnect on next start");

	// The verification checkbox state is an in/out parameter of
	// TaskDialogIndirect; pre-setting it to TRUE checks it by default.
	BOOL verifyChecked = g_reconnect ? TRUE : FALSE;

	int buttonPressed = 0;
	if (SUCCEEDED(TaskDialogIndirect(&config, &buttonPressed, nullptr, &verifyChecked)) && buttonPressed == IDOK)
	{
		g_reconnect = verifyChecked ? true : false;
		SaveSettings();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	}
}

HMENU BuildPopupMenu()
{
	HMENU menu = CreatePopupMenu();

	AppendMenuW(menu, MF_STRING, IDM_BLUETOOTH_SETTINGS, _(L"Bluetooth Settings"));

	HMENU langMenu = CreatePopupMenu();
	AppendMenuW(langMenu, MF_STRING, IDM_LANG_EN, L"English");
	AppendMenuW(langMenu, MF_STRING, IDM_LANG_ZH, L"中文");
	AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(langMenu), _(L"Language"));

	AppendMenuW(menu, MF_STRING, IDM_VIEW_LOGS, _(L"View Logs"));
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, IDM_DISCONNECT_ALL, _(L"Disconnect All"));
	AppendMenuW(menu, MF_STRING, IDM_RESTART_AUDIO, _(L"Restart Bluetooth Audio"));
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING | (g_fixSilentConnection ? MF_CHECKED : 0), IDM_FIX_SILENT, _(L"Reconnect when silent"));
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, IDM_EXIT, _(L"Exit"));

	return menu;
}

void HandleMenuCommand(int cmd)
{
	switch (cmd)
	{
	case IDM_BLUETOOTH_SETTINGS:
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
		break;
	case IDM_LANG_EN:
		g_language = L"en";
		SaveSettings();
		RebuildUi();
		break;
	case IDM_LANG_ZH:
		g_language = L"zh-CN";
		SaveSettings();
		RebuildUi();
		break;
	case IDM_VIEW_LOGS:
	{
		auto result = ShellExecuteW(nullptr, L"open", GetLogFilePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		LOG_LAST_ERROR_IF(reinterpret_cast<INT_PTR>(result) <= 32);
		break;
	}
	case IDM_DISCONNECT_ALL:
		DisconnectAllDevices();
		break;
	case IDM_RESTART_AUDIO:
		RestoreAudioService();
		break;
	case IDM_FIX_SILENT:
		g_fixSilentConnection = !g_fixSilentConnection;
		SaveSettings();
		LogEvent(L"Reconnect when silent: %s", g_fixSilentConnection ? L"on" : L"off");
		break;
	case IDM_EXIT:
		ShowExitConfirmation();
		break;
	}
}

void RebuildUi()
{
	// Reload the translation maps for the newly selected language. The Win32
	// popup menu is rebuilt from _() strings on every open. The flyout tree is
	// persistent (unlike the picker it replaced), so its fixed labels and its
	// device rows are re-created here as well.
	ReloadTranslations();
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	if (g_flyoutRoot)
	{
		ApplyFlyoutTexts();
		RefreshDeviceList();
	}
}

winrt::fire_and_forget ConnectDevice(DeviceInformation device, int autoReconnectAttempt)
{
	if (g_shuttingDown) co_return;

	// Kept in locals so they outlive every suspension point below.
	const std::wstring deviceId(device.Id());
	const std::wstring deviceName(device.Name());

	// One line per attempt, so a log sent in by a user shows exactly how many
	// automatic reconnects the connection went through.
	if (autoReconnectAttempt > 0)
	{
		LogEvent(L"Reconnecting: %s  (auto reconnect %d/%d)",
			deviceName.c_str(), autoReconnectAttempt, kMaxAutoReconnects);
	}
	else
	{
		LogEvent(L"Connecting: %s", deviceName.c_str());
	}

	// Disable the row's button while the request is in flight so a second
	// click cannot start a competing connection to the same device.
	UpdateDeviceStatus(deviceId, _(L"Connecting"), DeviceAction::None);

	bool success = false;
	bool opened = false;
	bool replaced = false;
	std::wstring errorMessage;
	AudioPlaybackConnection connection{ nullptr };

	try
	{
		// Never reconnect into a sink endpoint Windows is still releasing: the
		// reconnected phone would show up as connected while every sample is
		// dropped. See kReconnectCooldown.
		const auto cooldown = ReconnectCooldownRemaining(deviceId);
		if (cooldown.count() > 0)
		{
			LogEvent(L"Waiting %lld ms before reconnecting: %s", static_cast<long long>(cooldown.count()), deviceName.c_str());
			co_await winrt::resume_after(cooldown);
			if (g_shuttingDown) co_return;
		}

		connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			{
				std::lock_guard<std::mutex> lock(g_connectionsMutex);

				// Replace, never emplace: the object in the map is the one that
				// owns the sink configuration, so emplace() silently keeping an
				// older object leaves the new connection to be destroyed at the
				// end of this coroutine — the phone stays "connected" while no
				// audio reaches the speakers.
				auto existing = g_audioPlaybackConnections.find(deviceId);
				if (existing != g_audioPlaybackConnections.end())
				{
					g_lastCloseTime.insert_or_assign(deviceId, Clock::now());
					existing->second.second.Close();
					g_audioPlaybackConnections.erase(existing);
					replaced = true;
				}

				g_audioPlaybackConnections.insert_or_assign(deviceId, std::pair(device, connection));
			}

			if (replaced)
			{
				// A tracked connection was closed just now, so this attempt is a
				// reconnect as far as Windows is concerned: give the old sink
				// endpoint the same moment to go away before claiming a new one.
				LogEvent(L"Replacing the tracked connection for: %s", deviceName.c_str());
				co_await winrt::resume_after(kReconnectCooldown);
				if (g_shuttingDown) co_return;
			}

			connection.StateChanged([deviceId](const auto& sender, const auto&) {
				// StateChanged fires on a background Bluetooth/audio thread.
				// PostMessage marshals the work to the UI thread so we don't
				// touch XAML objects or the global connection map from the wrong
				// thread. The connection identity travels with the message so a
				// stale one can never touch a newer connection.
				//
				// Nothing may escape a WinRT event handler: an unhandled
				// exception here would take the whole process down.
				try
				{
					const auto state = sender.State();
					auto info = new ConnectionEventInfo{ deviceId, sender };
					if (!PostMessageW(g_hWnd,
						state == AudioPlaybackConnectionState::Opened ? WM_CONNECTION_OPENED : WM_CONNECTION_CLOSED,
						reinterpret_cast<WPARAM>(info), 0))
					{
						LOG_LAST_ERROR();
						delete info;
					}
				}
				catch (winrt::hresult_error const&)
				{
					LOG_CAUGHT_EXCEPTION();
				}
			});

			co_await connection.StartAsync();
			if (g_shuttingDown) co_return;
			auto result = co_await connection.OpenAsync();
			if (g_shuttingDown) co_return;

			// The state after the request is what decides whether audio actually
			// flows — OpenAsync returning Success only means the request was
			// accepted. Both are logged so that "connected but silent" can be
			// told apart from "could not connect" in a log sent in by a user.
			auto state = AudioPlaybackConnectionState::Closed;
			try
			{
				state = connection.State();
			}
			catch (winrt::hresult_error const&)
			{
				// A throwing State() must not be mistaken for a failed open:
				// that path would close a connection that may well be working.
				LOG_CAUGHT_EXCEPTION();
			}
			opened = state == AudioPlaybackConnectionState::Opened;
			LogEvent(L"Open result: %s  status=%d  state=%d",
				deviceName.c_str(), static_cast<int>(result.Status()), static_cast<int>(state));

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				winrt::throw_hresult(result.ExtendedError());
				break;
			}
		}
		else
		{
			success = false;
			errorMessage = _(L"Unknown error");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		errorMessage.resize(64);
		while (1)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				errorMessage.resize(errorMessage.size() * 2);
			}
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		LOG_CAUGHT_EXCEPTION();
	}
	catch (...)
	{
		// Never let an unexpected exception escape a fire_and_forget coroutine,
		// which would terminate the whole process.
		success = false;
		errorMessage = _(L"Unknown error");
	}

	if (success)
	{
		// "The request was accepted" and "audio is flowing" are different
		// states, and only the second one is worth telling the user about.
		// StateChanged(Opened) flips the row to Connected if the sink comes up
		// after this point.
		UpdateDeviceStatus(deviceId, opened ? _(L"Connected") : _(L"Connected, no audio"), DeviceAction::Disconnect);
		LogEvent(L"Connected: %s  (audio %s)", deviceName.c_str(), opened ? L"flowing" : L"not flowing yet");

		// Success here only means Windows accepted the request. Whether audio
		// actually arrives is checked against the output endpoint, and a silent
		// one is reconnected automatically — see VerifyAudioAfterConnect.
		if (g_fixSilentConnection && autoReconnectAttempt < kMaxAutoReconnects)
		{
			VerifyAudioAfterConnect(device, autoReconnectAttempt + 1);
		}
	}
	else
	{
		// Close only a connection this coroutine owns. The map may hold a
		// different object — a second attempt, or a link the phone established
		// on its own — and closing that would drop a working connection.
		if (connection)
		{
			const bool owned = IsCurrentConnection(deviceId, connection);
			if (owned)
			{
				// Close the connection; the StateChanged → WM_CONNECTION_CLOSED
				// path handles the async endpoint cleanup and UI update on the
				// correct thread — no Sleep() needed here. The entry is erased
				// first so that message finds nothing instead of a half-closed
				// connection.
				std::lock_guard<std::mutex> lock(g_connectionsMutex);
				auto it = g_audioPlaybackConnections.find(deviceId);
				if (it != g_audioPlaybackConnections.end() && it->second.second == connection)
				{
					it->second.second.Close();
					g_audioPlaybackConnections.erase(it);
				}
			}
			else
			{
				LogEvent(L"Not closing the tracked connection for: %s", deviceName.c_str());
				connection.Close(); // the throwaway object created above
			}

			if (owned) MarkDeviceClosed(deviceId);
		}

		LogEvent(L"Connect failed: %s  (%s)", deviceName.c_str(), errorMessage.c_str());
		UpdateDeviceStatus(deviceId, winrt::hstring(errorMessage.c_str()), DeviceAction::Retry);
	}
}

winrt::fire_and_forget ConnectDevice(std::wstring deviceId)
{
	if (g_shuttingDown) co_return;

	// Kept in a local so it outlives the suspension point.
	const winrt::hstring id(deviceId.c_str());
	auto device = co_await DeviceInformation::CreateFromIdAsync(id);
	if (g_shuttingDown) co_return;
	ConnectDevice(device);
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	// Same registry value the flyout palette uses (see IsSystemLightTheme).
	g_nid.hIcon = IsSystemLightTheme() ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}

void DisconnectAllDevices()
{
	std::vector<std::wstring> closedIds;
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		if (g_audioPlaybackConnections.empty())
		{
			return;
		}

		// Close all active connections. StateChanged → WM_CONNECTION_CLOSED
		// handles async endpoint cleanup on the UI thread. We clear the map
		// immediately — pending WM_CONNECTION_CLOSED messages will find
		// nothing, which is harmless.
		closedIds.reserve(g_audioPlaybackConnections.size());
		for (auto& pair : g_audioPlaybackConnections)
		{
			closedIds.push_back(pair.first);
			pair.second.second.Close();
		}

		g_audioPlaybackConnections.clear();
	}

	// Rows are updated after the lock is released — see WM_CONNECTION_CLOSED.
	for (auto const& id : closedIds)
	{
		MarkDeviceClosed(id);
		UpdateDeviceStatus(id, _(L"Not connected"), DeviceAction::Connect);
	}

	LogEvent(L"Disconnect All: closed all connections");

	TaskDialog(nullptr, nullptr,
		_(L"All Devices Disconnected"),
		nullptr,
		_(L"All Bluetooth audio connections have been closed.\n\nYou can now reconnect your devices."),
		TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
}

winrt::fire_and_forget RestoreAudioService()
{
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		if (g_audioPlaybackConnections.empty())
		{
			co_return;
		}
	}

	int result = TaskDialog(nullptr, nullptr,
		_(L"Restart Bluetooth Audio"),
		nullptr,
		_(L"This will disconnect and reconnect the audio connections managed by AudioPlaybackConnector.\n\n"
		  L"No system services are touched — only this app's own connections are affected.\n"
		  L"Other Bluetooth devices (mouse, keyboard, etc.) will NOT be interrupted.\n\n"
		  L"Do you want to continue?"),
		TDCBF_YES_BUTTON | TDCBF_CANCEL_BUTTON, TD_INFORMATION_ICON, nullptr);

	if (result != IDYES)
	{
		co_return;
	}

	// Save device IDs before closing so we can reconnect
	std::vector<std::wstring> deviceIds;
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		deviceIds.reserve(g_audioPlaybackConnections.size());
		for (const auto& pair : g_audioPlaybackConnections)
		{
			deviceIds.push_back(pair.first);
			pair.second.second.Close();
		}
		g_audioPlaybackConnections.clear();
	}

	for (auto const& id : deviceIds)
	{
		MarkDeviceClosed(id);
		UpdateDeviceStatus(id, _(L"Not connected"), DeviceAction::Connect);
	}

	LogEvent(L"Restart Bluetooth Audio: closed %zu connection(s), reconnecting", deviceIds.size());

	// Let Windows finish releasing the old sink endpoints before we create
	// fresh ones for the same devices. Without enough time, the new connection
	// can collide with an endpoint that is still being torn down, which shows
	// up as silent audio even though the phone reconnects.
	co_await winrt::resume_after(std::chrono::seconds(3));

	// If the user exited during the wait, don't reconnect
	if (g_shuttingDown) co_return;

	// Reconnect each device with staggered delays to avoid flooding the
	// Bluetooth stack with simultaneous connection requests, which degrades
	// both Bluetooth quality and 2.4 GHz Wi-Fi coexistence.
	for (const auto& id : deviceIds)
	{
		ConnectDevice(id);
		co_await winrt::resume_after(std::chrono::milliseconds(500));
	}
}

winrt::fire_and_forget ReconnectDevices(std::vector<std::wstring> deviceIds)
{
	LogEvent(L"Reconnecting %zu device(s)", deviceIds.size());

	// Startup auto-reconnect: stagger each device so simultaneous requests
	// don't flood the Bluetooth stack (same reasoning as RestoreAudioService).
	for (const auto& id : deviceIds)
	{
		if (g_shuttingDown) co_return;
		ConnectDevice(id);
		co_await winrt::resume_after(std::chrono::milliseconds(500));
	}
}
