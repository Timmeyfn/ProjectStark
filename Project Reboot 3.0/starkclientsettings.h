#pragma once

// Your own (client) settings: build/edit sensitivity and reset on release.
// Stored in %LOCALAPPDATA%\ProjectStark\client_settings.ini. The server window and the
// Project Stark Launcher both edit it, and StarkClient (inside your game) reads it every second.

#include <Windows.h>
#include <string>
#include <cstdio>

namespace StarkClientSettings
{
	inline std::string Path()
	{
		char Buffer[MAX_PATH] = {};
		GetEnvironmentVariableA("LOCALAPPDATA", Buffer, MAX_PATH);
		std::string Dir = std::string(Buffer) + "\\ProjectStark";
		CreateDirectoryA(Dir.c_str(), nullptr);
		return Dir + "\\client_settings.ini";
	}

	inline std::string ReadString(const char* Key, const char* Default)
	{
		char Value[64] = {};
		GetPrivateProfileStringA("Client", Key, Default, Value, sizeof(Value), Path().c_str());
		return Value;
	}

	inline float ReadFloat(const char* Key, float Default)
	{
		auto Value = ReadString(Key, "");
		if (Value.empty()) return Default;
		float Result = (float)atof(Value.c_str());
		return Result < 0.05f ? 0.05f : Result > 5.f ? 5.f : Result;
	}

	inline void WriteString(const char* Key, const std::string& Value)
	{
		WritePrivateProfileStringA("Client", Key, Value.c_str(), Path().c_str());
	}

	inline void WriteFloat(const char* Key, float Value)
	{
		char Buffer[32];
		sprintf_s(Buffer, "%.2f", Value);
		WriteString(Key, Buffer);
	}
}

#ifdef IMGUI_VERSION
// Drawn inside the Project Stark server window.
inline void StarkClientSettingsUI()
{
	static bool bLoaded = false;
	static float BuildSens = 1.f, EditSens = 1.f;
	static bool bResetOnRelease = false;
	static char EditKey[32] = "G", ResetKey[32] = "RightMouse";

	if (!bLoaded)
	{
		bLoaded = true;
		BuildSens = StarkClientSettings::ReadFloat("BuildSensitivity", 1.f);
		EditSens = StarkClientSettings::ReadFloat("EditSensitivity", 1.f);
		bResetOnRelease = StarkClientSettings::ReadString("ResetOnRelease", "0") == "1";
		strncpy_s(EditKey, StarkClientSettings::ReadString("EditKey", "G").c_str(), sizeof(EditKey) - 1);
		strncpy_s(ResetKey, StarkClientSettings::ReadString("ResetKey", "RightMouse").c_str(), sizeof(ResetKey) - 1);
	}

	ImGui::TextWrapped("These change YOUR game while you play. They need StarkClient.dll in your game: Reboot Launcher -> Settings -> Internal files -> Unreal engine console -> pick StarkClient.dll (next to your built DLL).");

	if (ImGui::SliderFloat("Build sensitivity", &BuildSens, 0.1f, 2.f, "%.2fx"))
		StarkClientSettings::WriteFloat("BuildSensitivity", BuildSens);

	if (ImGui::SliderFloat("Edit sensitivity", &EditSens, 0.1f, 2.f, "%.2fx"))
		StarkClientSettings::WriteFloat("EditSensitivity", EditSens);

	if (ImGui::Checkbox("Reset on release (letting go of reset while editing confirms the edit)", &bResetOnRelease))
		StarkClientSettings::WriteString("ResetOnRelease", bResetOnRelease ? "1" : "0");

	if (ImGui::InputText("Edit key (e.g. G, F, Mouse4)", EditKey, sizeof(EditKey)))
		StarkClientSettings::WriteString("EditKey", EditKey);

	if (ImGui::InputText("Reset key (e.g. RightMouse, R, Mouse5)", ResetKey, sizeof(ResetKey)))
		StarkClientSettings::WriteString("ResetKey", ResetKey);
}
#endif
