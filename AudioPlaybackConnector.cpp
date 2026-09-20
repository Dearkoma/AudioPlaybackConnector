#include "pch.h"
#include "AudioPlaybackConnector.h"

#include <stdarg.h>
#include <strsafe.h>

// Posted with WM_CONNECTION_CLOSED: the device id of the connection that
// closed, plus the AudioPlaybackConnection instance that actually closed.
// WM_CONNECTION_CLOSED compares this against the current map entry so a stale
// message from a previous connection to the same device can never remove a
// newer, still-open connection.
struct ConnectionClosedInfo
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
	IDM_EXIT,
};

// SetDisplayStatus may be called by a ConnectDevice coroutine after the picker
// was dismissed and its island torn down (g_devicePicker can be null); swallow
// any failure so a stale status update can never crash the app.
void SafeSetDisplayStatus(DevicePicker const& picker, DeviceInformation const& device, winrt::param::hstring const& status, DevicePickerDisplayStatusOptions options)
{
	try { if (picker) picker.SetDisplayStatus(device, status, options); } catch (...) {}
}

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
winrt::fire_and_forget ConnectDevice(DevicePicker, DeviceInformation);
void SetupSvgIcon();
void UpdateNotifyIcon();
void DisconnectAllDevices();
winrt::fire_and_forget RestoreAudioService();
winrt::fire_and_forget ReconnectDevices(std::vector<std::wstring> deviceIds);
void RebuildUi();
void CreateIsland();
void DestroyIsland();
void ShowDevicePicker(Rect);
HMENU BuildPopupMenu();
void HandleMenuCommand(int cmd);

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

	winrt::init_apartment();

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
	// popup menu, and the island (hosting the DevicePicker) is created on
	// demand only while the picker is open, then torn down when it dismisses.
	// This avoids keeping a DirectComposition surface alive in the background.
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
		if (g_desktopSourceNative2)
		{
			winrt::check_hresult(g_desktopSourceNative2->PreTranslateMessage(&msg, &processed));
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
		DestroyIsland();

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
				if (g_devicePicker) g_devicePicker.SetDisplayStatus(pair.second.first, {}, DevicePickerDisplayStatusOptions::None);
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
			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			ShowDevicePicker(rect);
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
		std::unique_ptr<ConnectionClosedInfo> info(reinterpret_cast<ConnectionClosedInfo*>(wParam));
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		auto it = g_audioPlaybackConnections.find(info->deviceId);
		// Only remove the entry if the connection that closed is still the one
		// in the map. A stale message from an earlier connection to the same
		// device must not drop a newer, still-open connection.
		if (it != g_audioPlaybackConnections.end() && it->second.second == info->connection)
		{
			if (g_devicePicker) g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
			LogEvent(L"Disconnected: %s", it->second.first.Name().c_str());
			g_audioPlaybackConnections.erase(it);
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

void CreateIsland()
{
	if (g_desktopSource) return;

	g_desktopSource = DesktopWindowXamlSource();
	g_desktopSourceNative2 = g_desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(g_desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(g_desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	g_desktopSource.Content(g_xamlCanvas);
}

void DestroyIsland()
{
	g_devicePicker = nullptr;
	if (g_desktopSource)
	{
		try
		{
			g_desktopSource.Content(nullptr);
		}
		catch (...)
		{
			// The island may already be partially torn down; never let
			// teardown crash the app.
		}
		g_desktopSource = nullptr;
	}
	g_desktopSourceNative2 = nullptr;
	g_hWndXaml = nullptr;
	g_xamlCanvas = nullptr;
}

// Refreshes the connection state of every device in the picker. Called right
// after the picker is shown: the picker object is recreated on each open, so
// any display status set during a previous session is lost — without this an
// already-connected device would look disconnected and offer no way to
// disconnect it.
//
// Pass 1 is synchronous and instant: whatever this process is managing is
// marked immediately. Pass 2 is authoritative: it asks the system which A2DP
// devices are actually connected, covering links established outside this app
// (e.g. from Windows Settings).
winrt::fire_and_forget RefreshDeviceConnectionStates(DevicePicker picker)
{
	if (!picker) co_return;

	// Collect under the lock, but never call into XAML while holding it.
	std::vector<DeviceInformation> managedDevices;
	try
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		managedDevices.reserve(g_audioPlaybackConnections.size());
		for (auto const& entry : g_audioPlaybackConnections)
		{
			managedDevices.push_back(entry.second.first);
		}
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
	}

	for (auto const& device : managedDevices)
	{
		SafeSetDisplayStatus(picker, device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}

	try
	{
		std::vector<winrt::hstring> requestedProperties{ L"System.Devices.Aep.IsConnected" };
		auto devices = co_await DeviceInformation::FindAllAsync(
			AudioPlaybackConnection::GetDeviceSelector(),
			winrt::single_threaded_vector(std::move(requestedProperties)));

		if (g_shuttingDown) co_return;

		for (auto const& device : devices)
		{
			bool isConnected = false;
			if (auto properties = device.Properties(); properties.HasKey(L"System.Devices.Aep.IsConnected"))
			{
				isConnected = winrt::unbox_value_or<bool>(properties.Lookup(L"System.Devices.Aep.IsConnected"), false);
			}

			bool managed = false;
			{
				std::lock_guard<std::mutex> lock(g_connectionsMutex);
				managed = g_audioPlaybackConnections.find(std::wstring(device.Id())) != g_audioPlaybackConnections.end();
			}

			if (isConnected || managed)
			{
				SafeSetDisplayStatus(picker, device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			}
			else
			{
				// Not connected: clear any stale status left on the list entry.
				SafeSetDisplayStatus(picker, device, {}, DevicePickerDisplayStatusOptions::None);
			}
		}
	}
	catch (winrt::hresult_error const&)
	{
		// Enumeration can fail (no radio, session issues). Pass 1 is already on
		// screen, so the picker stays usable — just log and move on.
		LOG_CAUGHT_EXCEPTION();
	}
	catch (...)
	{
	}
}

void ShowDevicePicker(Rect rect)
{
	// The picker needs the host window visible/foreground; size it full screen
	// (layered alpha-0 => invisible) so XAML DPI is correct. It is restored to
	// 1x1, hidden, non-topmost when the picker closes. The island itself is
	// created lazily on first use and then kept alive (hidden) — recreating a
	// DesktopWindowXamlSource on the same host window proved crash-prone.
	SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_SHOWWINDOW);
	SetForegroundWindow(g_hWnd);

	try
	{
		CreateIsland();

		g_devicePicker = DevicePicker();
		winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

		g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
		g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
			// Null the global picker (in-flight coroutines keep their own copy)
			// and restore the tiny hidden non-topmost window. The island itself
			// is kept alive (created lazily on first use): recreating a
			// DesktopWindowXamlSource on the same host window on repeated opens
			// proved crash-prone.
			g_devicePicker = nullptr;
			SetWindowPos(g_hWnd, HWND_NOTOPMOST, 0, 0, 1, 1, SWP_NOZORDER | SWP_HIDEWINDOW);
		});
		g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
			ConnectDevice(sender, args.SelectedDevice());
		});
		g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
			auto device = args.Device();
			bool closed = false;
			{
				std::lock_guard<std::mutex> lock(g_connectionsMutex);
				auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
				if (it != g_audioPlaybackConnections.end())
				{
					it->second.second.Close();
					g_audioPlaybackConnections.erase(it);
					closed = true;
					// StateChanged → WM_CONNECTION_CLOSED handles async
					// endpoint cleanup on the UI thread.
				}
			}

			if (!closed)
			{
				// The link was established outside this app (for example from
				// Windows Settings), so it is not in our map. Create a
				// throwaway connection object for the device and close it so
				// the disconnect button still does something.
				try
				{
					if (auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id()))
					{
						connection.Close();
						LogEvent(L"Disconnect (external link): %s", device.Name().c_str());
					}
				}
				catch (winrt::hresult_error const&)
				{
					LOG_CAUGHT_EXCEPTION();
				}
			}

			SafeSetDisplayStatus(sender, device, {}, DevicePickerDisplayStatusOptions::None);
		});

		using namespace winrt::Windows::UI::Popups;
		g_devicePicker.Show(rect, Placement::Above);

		// Detect the connection state of every device in the list, so an
		// already-connected device is shown as connected (left-click refresh).
		RefreshDeviceConnectionStates(g_devicePicker);
	}
	catch (winrt::hresult_error const&)
	{
		// Picker failed to open — clean up and restore the hidden window.
		LOG_CAUGHT_EXCEPTION();
		DestroyIsland();
		SetWindowPos(g_hWnd, HWND_NOTOPMOST, 0, 0, 1, 1, SWP_NOZORDER | SWP_HIDEWINDOW);
	}
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
	case IDM_EXIT:
		ShowExitConfirmation();
		break;
	}
}

void RebuildUi()
{
	// Reload the translation maps for the newly selected language. The Win32
	// popup menu is rebuilt from _() strings on every open, so no persistent
	// XAML UI needs reconstructing here.
	ReloadTranslations();
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	if (g_shuttingDown) co_return;

	LogEvent(L"Connecting: %s", device.Name().c_str());

	SafeSetDisplayStatus(picker, device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	std::wstring errorMessage;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			{
				std::lock_guard<std::mutex> lock(g_connectionsMutex);
				g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));
			}

			connection.StateChanged([](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					// StateChanged fires on a background Bluetooth/audio thread.
					// PostMessage marshals the work to the UI thread so we don't
					// touch XAML objects (g_devicePicker) or the global connection
					// map from the wrong thread. The connection identity is
					// included so WM_CONNECTION_CLOSED can detect stale messages.
					auto info = new ConnectionClosedInfo{ std::wstring(sender.DeviceId()), sender };
					PostMessageW(g_hWnd, WM_CONNECTION_CLOSED, reinterpret_cast<WPARAM>(info), 0);
				}
			});

			co_await connection.StartAsync();
			if (g_shuttingDown) co_return;
			auto result = co_await connection.OpenAsync();
			if (g_shuttingDown) co_return;

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
		SafeSetDisplayStatus(picker, device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
		LogEvent(L"Connected: %s", device.Name().c_str());
	}
	else
	{
		{
			std::lock_guard<std::mutex> lock(g_connectionsMutex);
			auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
			if (it != g_audioPlaybackConnections.end())
			{
				// Close the connection; the StateChanged → WM_CONNECTION_CLOSED
				// path handles the async endpoint cleanup and UI update on the
				// correct thread — no Sleep() needed here.
				it->second.second.Close();
				g_audioPlaybackConnections.erase(it);
			}
		}
		LogEvent(L"Connect failed: %s  (%s)", device.Name().c_str(), errorMessage.c_str());
		SafeSetDisplayStatus(picker, device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	if (g_shuttingDown) co_return;

	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	if (g_shuttingDown) co_return;
	ConnectDevice(picker, device);
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
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

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
		for (auto& pair : g_audioPlaybackConnections)
		{
			pair.second.second.Close();
			if (g_devicePicker) g_devicePicker.SetDisplayStatus(pair.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}

		g_audioPlaybackConnections.clear();
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
			if (g_devicePicker) g_devicePicker.SetDisplayStatus(pair.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}
		g_audioPlaybackConnections.clear();
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
		ConnectDevice(g_devicePicker, id);
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
		ConnectDevice(g_devicePicker, id);
		co_await winrt::resume_after(std::chrono::milliseconds(500));
	}
}
