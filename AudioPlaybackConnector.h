#pragma once

#include "resource.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
namespace fs = std::filesystem;

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_CONNECTION_CLOSED = WM_APP + 3; // StateChanged → UI thread marshal
constexpr UINT WM_RUNONUITHREAD = WM_APP + 4;     // marshals a callable onto the UI thread

// Action offered by a device row's button in the flyout. None disables the
// button (a connect is already in flight for that device).
enum class DeviceAction
{
	None,
	Connect,
	Disconnect,
	Retry
};

// Holds what a row's button click handler needs. The handler captures a
// shared_ptr to this, so the row's action can be re-targeted later (for
// example connect → disconnect once the connection succeeds) without
// rebuilding the row.
struct DeviceRowState
{
	std::wstring deviceId;
	DeviceAction action = DeviceAction::None;
};

// One rendered device row in the self-drawn flyout.
struct DeviceRow
{
	std::wstring deviceId;
	TextBlock statusText{ nullptr };
	Button actionButton{ nullptr };
	std::shared_ptr<DeviceRowState> state;
};

HINSTANCE g_hInst;
HWND g_hWnd;

// The device flyout is drawn by this app instead of using the system device
// picker, because the picker exposes no way to add a refresh indicator or a
// refresh button. It lives in its own top-level popup window with its own XAML
// island; the window and the XAML tree are created once and reused, so
// connection state survives between opens.
HWND g_hWndFlyout = nullptr;
DesktopWindowXamlSource g_flyoutSource = nullptr;
winrt::com_ptr<IDesktopWindowXamlSourceNative2> g_flyoutSourceNative2;
Grid g_flyoutRoot = nullptr;
TextBlock g_flyoutTitle = nullptr;
TextBlock g_flyoutSubtitle = nullptr;
Button g_flyoutRefreshButton = nullptr;
TextBlock g_flyoutRefreshLabel = nullptr;
ProgressBar g_flyoutProgressBar = nullptr;
StackPanel g_flyoutDeviceList = nullptr;
std::vector<DeviceRow> g_deviceRows;
bool g_flyoutVisible = false;
bool g_flyoutRefreshing = false;
ULONGLONG g_flyoutHiddenTick = 0; // guards against reopening on the click that just closed it

// The XAML island's own child window (IDesktopWindowXamlSourceNative2::
// get_WindowHandle). It starts out at 0x0 and the framework never resizes it
// with the host window, so the app has to size it — without that the flyout
// window opens but stays entirely unpainted.
HWND g_hWndFlyoutXaml = nullptr;

// XAML framework core window for the UI thread. Must be initialised before the
// first DesktopWindowXamlSource is created on that thread.
WindowsXamlManager g_xamlManager{ nullptr };

// Thread that owns g_hWnd, the message loop and every XAML object. XAML objects
// are not agile, so anything that touches them has to run here.
DWORD g_uiThreadId = 0;

std::unordered_map<std::wstring, std::pair<DeviceInformation, AudioPlaybackConnection>> g_audioPlaybackConnections;
std::mutex g_connectionsMutex; // Protects g_audioPlaybackConnections
HICON g_hIconLight = nullptr;
HICON g_hIconDark = nullptr;
NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;
bool g_reconnect = false;
bool g_shuttingDown = false;
std::vector<std::wstring> g_lastDevices;
std::wstring g_language; // "" = auto, "en" = English, "zh-CN" = 简体中文

#include "Util.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "Direct2DSvg.hpp"
