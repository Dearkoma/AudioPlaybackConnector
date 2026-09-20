// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H

#include "targetver.h"

// Windows Header Files
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <d2d1_3.h>
#include <shlwapi.h>
// Core Audio, used to meter the default output endpoint. Windows can report an
// A2DP sink connection as open while nothing is playing, so the only way to
// tell "connected" from "connected and actually playing" is to measure it.
// mmdeviceapi.h needs objbase.h (excluded by WIN32_LEAN_AND_MEAN) and the
// PROPVARIANT declarations that come with it.
#include <objbase.h>
#include <propidl.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
// IPropertyStore is used to read an endpoint's friendly name into the log.
#include <propsys.h>

// C++ RunTime Header Files
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <unordered_map>
#include <filesystem>

// wil
#ifndef _DEBUG
#define RESULT_DIAGNOSTICS_LEVEL 1
#endif

#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>

// C++/WinRT
// Fixes warning C4002: too many arguments for function-like macro invocation 'GetCurrentTime'
#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.System.h>
// Needed by the self-drawn device flyout (Thickness, GridLength, Visibility,
// SolidColorBrush, ...). Controls.h pulls most of this in transitively, but
// the dependencies are stated explicitly so the flyout never depends on them
// being included by accident.
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
// TextBlock.FontWeight hands out a Windows.UI.Text.FontWeight; without this
// header FontWeights' static getters are only declared, not defined ("C3779:
// a function that returns 'auto' cannot be used before it is defined").
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Windows.UI.Xaml.Markup.h>

#endif //PCH_H
