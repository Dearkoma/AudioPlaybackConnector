#pragma once

constexpr auto CONFIG_NAME = L"AudioPlaybackConnector.json";
constexpr auto BUFFER_SIZE = 4096;

void DefaultSettings()
{
	g_reconnect = false;
	g_lastDevices.clear();
	g_language.clear(); // auto-detect
}

void LoadSettings()
{
	try
	{
		DefaultSettings();

		wil::unique_hfile hFile(CreateFileW((GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string string;
		while (1)
		{
			size_t size = string.size();
			string.resize(size + BUFFER_SIZE);
			DWORD read = 0;
			THROW_IF_WIN32_BOOL_FALSE(ReadFile(hFile.get(), string.data() + size, BUFFER_SIZE, &read, nullptr));
			string.resize(size + read);
			if (read == 0)
				break;
		}

		std::wstring utf16 = Utf8ToUtf16(string);
		auto jsonObj = JsonObject::Parse(utf16);

		// Tolerate older/missing keys instead of throwing and losing whatever
		// was already parsed.
		auto reconnect = jsonObj.TryLookup(L"reconnect");
		if (reconnect && reconnect.ValueType() == JsonValueType::Boolean)
			g_reconnect = reconnect.GetBoolean();

		auto language = jsonObj.TryLookup(L"language");
		if (language && language.ValueType() == JsonValueType::String)
			g_language = std::wstring(language.GetString());

		auto lastDevices = jsonObj.TryLookup(L"lastDevices");
		if (lastDevices && lastDevices.ValueType() == JsonValueType::Array)
		{
			auto devices = lastDevices.GetArray();
			g_lastDevices.reserve(devices.Size());
			for (const auto& i : devices)
			{
				if (i.ValueType() == JsonValueType::String)
					g_lastDevices.push_back(std::wstring(i.GetString()));
			}
		}
	}
	CATCH_LOG();
}

void SaveSettings()
{
	try
	{
		JsonObject jsonObj;
		jsonObj.Insert(L"reconnect", JsonValue::CreateBooleanValue(g_reconnect));
		// "fixSilentConnection" used to live here. It is gone on purpose rather
		// than defaulted to false: the file is rewritten from scratch below, so
		// simply not writing the key removes it from every existing config —
		// and a key that is still read would have kept the old behaviour alive
		// on any machine whose config was written by a version that defaulted
		// it ON. A setting that survives a feature is how a "default off" ships
		// enabled.
		if (!g_language.empty())
			jsonObj.Insert(L"language", JsonValue::CreateStringValue(g_language));

		JsonArray lastDevices;
		{
			std::lock_guard<std::mutex> lock(g_connectionsMutex);
			for (const auto& i : g_audioPlaybackConnections)
			{
				lastDevices.Append(JsonValue::CreateStringValue(i.first));
			}
		}
		jsonObj.Insert(L"lastDevices", lastDevices);

		wil::unique_hfile hFile(CreateFileW((GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string utf8 = Utf16ToUtf8(jsonObj.Stringify());
		DWORD written = 0;
		THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
		THROW_HR_IF(E_FAIL, written != utf8.size());
	}
	CATCH_LOG();
}
