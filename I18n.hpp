#pragma once
#include "FnvHash.hpp"

// External: set by the language menu, persisted in settings.
// Empty = auto-detect from system. "en" = English, "zh-CN" = Simplified Chinese.
extern std::wstring g_language;

// ── Built-in Chinese translation table ──────────────────────────
// Hash-based lookup map populated at startup when language is zh-CN.
// No external .po/.ymo files needed — everything is compiled in.
std::unordered_map<uint32_t, const wchar_t*> hashToStrMap;

void BuildChineseMap()
{
	// English source → FNV-1a hash → Chinese translation
	// Hash is computed over the UTF-16LE bytes of the English source string,
	// matching what the _() macro passes to Translate().

	#define H(s) fnv1a_32((s), wcslen(s) * sizeof(wchar_t))

	hashToStrMap.emplace(H(L"Unsupported Operating System"), L"操作系统不支持");
	hashToStrMap.emplace(H(L"AudioPlaybackConnector is not supported on this operating system version."), L"此操作系统版本不支持 AudioPlaybackConnector。");
	hashToStrMap.emplace(H(L"All connections will be closed.\nExit anyway?"), L"所有连接将被关闭。\n确定退出吗？");
	hashToStrMap.emplace(H(L"Reconnect on next start"), L"下次启动时重新连接");
	hashToStrMap.emplace(H(L"Exit"), L"退出");
	hashToStrMap.emplace(H(L"Language"), L"语言");
	hashToStrMap.emplace(H(L"Bluetooth Settings"), L"蓝牙设置");
	hashToStrMap.emplace(H(L"Disconnect All"), L"断开全部");
	hashToStrMap.emplace(H(L"Restart Bluetooth Audio"), L"重启蓝牙音频");
	hashToStrMap.emplace(H(L"View Logs"), L"查看日志");
	hashToStrMap.emplace(H(L"Connecting"), L"正在连接");
	hashToStrMap.emplace(H(L"The request timed out"), L"请求超时");
	hashToStrMap.emplace(H(L"The operation was denied by the system"), L"操作被系统拒绝");
	hashToStrMap.emplace(H(L"Unknown error"), L"未知错误");
	hashToStrMap.emplace(H(L"Connected"), L"已连接");
	hashToStrMap.emplace(H(L"Connected, no audio"), L"已连接，无音频");
	hashToStrMap.emplace(H(L"All Devices Disconnected"), L"所有设备已断开");
	hashToStrMap.emplace(H(L"All Bluetooth audio connections have been closed.\n\nYou can now reconnect your devices."), L"所有蓝牙音频连接已关闭。\n\n现在可以重新连接设备。");

	// ── Self-drawn device flyout ─────────────────────────────────
	hashToStrMap.emplace(H(L"Bluetooth Audio Devices"), L"蓝牙音频设备");
	hashToStrMap.emplace(H(L"%d connected"), L"%d 台已连接");
	hashToStrMap.emplace(H(L"Checking connection status"), L"正在检测连接状态");
	hashToStrMap.emplace(H(L"No devices found"), L"未找到设备");
	hashToStrMap.emplace(H(L"Not connected"), L"未连接");
	hashToStrMap.emplace(H(L"Connect"), L"连接");
	hashToStrMap.emplace(H(L"Disconnect"), L"断开");
	hashToStrMap.emplace(H(L"Retry"), L"重试");
	hashToStrMap.emplace(H(L"Reconnect"), L"重连");
	hashToStrMap.emplace(H(L"Reconnect when silent"), L"连接后无音频时自动重连");
	hashToStrMap.emplace(H(L"Refresh"), L"刷新");
	hashToStrMap.emplace(H(L"This will disconnect and reconnect the audio connections managed by AudioPlaybackConnector.\n\n"
	                      L"No system services are touched — only this app's own connections are affected.\n"
	                      L"Other Bluetooth devices (mouse, keyboard, etc.) will NOT be interrupted.\n\n"
	                      L"Do you want to continue?"),
	                      L"这将断开并重新连接由 AudioPlaybackConnector 管理的音频连接。\n\n"
	                      L"不会触及其他系统服务 — 仅影响本程序自身的连接。\n"
	                      L"其他蓝牙设备（鼠标、键盘等）不会中断。\n\n"
	                      L"是否继续？");

	#undef H
}

#pragma pack(push, 1)
struct YMOData
{
	uint16_t len;
	struct
	{
		uint32_t hash;
		uint16_t offset;
	} table[1];
};
#pragma pack(pop)

LANGID LanguageToLangId(const std::wstring& lang)
{
	if (lang == L"zh-CN")  return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
	if (lang == L"zh-TW")  return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
	return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}

void LoadTranslateData()
{
	hashToStrMap.clear();

	// Determine language
	std::wstring lang = g_language;
	if (lang.empty())
	{
		// Auto-detect: check if system UI language is Chinese
		LANGID sysLang = GetThreadUILanguage();
		LANGID primary = PRIMARYLANGID(sysLang);
		if (primary == LANG_CHINESE)
			lang = L"zh-CN";
	}

	// Built-in Chinese map — no external files, no encoding issues
	if (lang == L"zh-CN")
	{
		BuildChineseMap();
		return;
	}

	// For other languages, try external .ymo resource (kept for future expansion)
	LANGID langId = LanguageToLangId(lang);
	auto hRes = FindResourceExW(g_hInst, L"YMO", MAKEINTRESOURCEW(1), langId);
	if (hRes)
	{
		auto hResData = LoadResource(g_hInst, hRes);
		if (hResData)
		{
			auto ymo = reinterpret_cast<const YMOData*>(LockResource(hResData));
			if (ymo)
			{
				hashToStrMap.reserve(ymo->len);
				for (int i = 0; i < ymo->len; ++i)
				{
					auto hash = ymo->table[i].hash;
					auto offset = ymo->table[i].offset;
					auto str = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(hResData) + offset);
					hashToStrMap.emplace(hash, str);
				}
			}
		}
	}
}

std::unordered_map<const wchar_t*, const wchar_t*> ptrToStrMap;

const wchar_t* Translate(const wchar_t* str)
{
	auto translation = str;

	auto i = ptrToStrMap.find(str);
	if (i == ptrToStrMap.end())
	{
		auto hash = fnv1a_32(str, wcslen(str) * sizeof(wchar_t));
		auto j = hashToStrMap.find(hash);
		if (j != hashToStrMap.end())
			translation = j->second;

		ptrToStrMap.emplace(str, translation);
	}
	else
		translation = i->second;

	return translation;
}

// Clear both caches and reload translations for the current language,
// then let the caller rebuild any UI that was already constructed.
void ReloadTranslations()
{
	ptrToStrMap.clear();
	LoadTranslateData();
}

const wchar_t* TranslateContext(const wchar_t* str, const wchar_t* ctxtStr)
{
	auto translation = Translate(ctxtStr);
	if (translation == ctxtStr)
		return str;
	return translation;
}

#define _(str) Translate(str)
#define C_(ctxt, str) TranslateContext(str, ctxt L"\004" str)
