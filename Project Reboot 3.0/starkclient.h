#pragma once

// Project Stark client features. These run inside YOUR Fortnite (not the server):
// the launcher injects a copy of this DLL named "StarkClient.dll" into the game client.
//
//  - Build sensitivity: mouse sensitivity multiplier while holding the building tool
//  - Edit sensitivity:  mouse sensitivity multiplier while editing
//  - Reset on release:  while editing, letting go of your reset key confirms the edit for you
//
// Settings live in %LOCALAPPDATA%\ProjectStark\client_settings.ini (the launcher's Settings page writes it)
// and are re-read every second, so you can change them while playing.

#include <Windows.h>
#include <string>
#include <atomic>
#include <cmath>
#include <algorithm>

#include "reboot.h"
#include "log.h"
#include "addresses.h"
#include "inc.h"
#include "GameplayStatics.h"
#include <MinHook/MinHook.h>

namespace StarkClient
{
	struct FSettings
	{
		float BuildSens = 1.f;
		float EditSens = 1.f;
		bool bResetOnRelease = false;
		int EditKey = 'G';             // virtual-key codes
		int ResetKey = VK_RBUTTON;
	};

	inline FSettings Settings;
	inline ULONGLONG LastSettingsRead = 0;
	inline ULONGLONG LastTick = 0;
	inline thread_local bool bInTick = false;

	enum class EMode { Normal, Build, Edit };

	inline EMode AppliedMode = EMode::Normal;
	inline float BaseSensitivity = -1.f; // the player's normal mouse sensitivity, saved before we change it

	inline std::string SettingsPath()
	{
		char Buffer[MAX_PATH] = {};
		GetEnvironmentVariableA("LOCALAPPDATA", Buffer, MAX_PATH);
		return std::string(Buffer) + "\\ProjectStark\\client_settings.ini";
	}

	inline float ReadFloat(const char* Key, float Default)
	{
		char Value[64] = {};
		GetPrivateProfileStringA("Client", Key, "", Value, sizeof(Value), SettingsPath().c_str());
		if (!Value[0]) return Default;
		float Result = (float)atof(Value);
		return Result < 0.05f ? 0.05f : Result > 5.f ? 5.f : Result;
	}

	// Scroll wheel isn't a real key, so it gets made-up codes
	constexpr int VK_WHEELDOWN = 0x10001;
	constexpr int VK_WHEELUP = 0x10002;

	// Counted by a low-level mouse hook (the scroll wheel has no "held down" state to poll)
	inline std::atomic<int> WheelDownCount = 0;
	inline std::atomic<int> WheelUpCount = 0;

	inline LRESULT CALLBACK MouseHookProc(int Code, WPARAM wParam, LPARAM lParam)
	{
		if (Code == HC_ACTION && wParam == WM_MOUSEWHEEL)
		{
			auto Info = (MSLLHOOKSTRUCT*)lParam;
			short Delta = (short)HIWORD(Info->mouseData);
			if (Delta < 0) WheelDownCount++;
			else if (Delta > 0) WheelUpCount++;
		}

		return CallNextHookEx(nullptr, Code, wParam, lParam);
	}

	inline DWORD WINAPI MouseHookThread(LPVOID)
	{
		SetWindowsHookExA(WH_MOUSE_LL, MouseHookProc, GetModuleHandleA(nullptr), 0);
		MSG Msg;
		while (GetMessageA(&Msg, nullptr, 0, 0) > 0) { TranslateMessage(&Msg); DispatchMessageA(&Msg); }
		return 0;
	}

	// "G", "F", "F1", "RightMouse", "Mouse4", "Space"... -> Windows virtual-key code
	inline int KeyNameToVK(std::string Name, int Default)
	{
		for (auto& c : Name) c = (char)toupper((unsigned char)c);
		Name.erase(std::remove(Name.begin(), Name.end(), ' '), Name.end());

		if (Name.empty()) return Default;
		if (Name == "WHEELDOWN" || Name == "SCROLLDOWN" || Name == "MOUSEWHEELDOWN") return VK_WHEELDOWN;
		if (Name == "WHEELUP" || Name == "SCROLLUP" || Name == "MOUSEWHEELUP") return VK_WHEELUP;
		if (Name.size() == 1 && ((Name[0] >= 'A' && Name[0] <= 'Z') || (Name[0] >= '0' && Name[0] <= '9'))) return Name[0];
		if (Name[0] == 'F' && Name.size() <= 3 && isdigit((unsigned char)Name[1])) { int N = atoi(Name.c_str() + 1); if (N >= 1 && N <= 12) return VK_F1 + N - 1; }

		if (Name == "LEFTMOUSE" || Name == "LMB" || Name == "MOUSE1") return VK_LBUTTON;
		if (Name == "RIGHTMOUSE" || Name == "RMB" || Name == "MOUSE2") return VK_RBUTTON;
		if (Name == "MIDDLEMOUSE" || Name == "MMB" || Name == "MOUSE3") return VK_MBUTTON;
		if (Name == "MOUSE4" || Name == "THUMBMOUSEBUTTON") return VK_XBUTTON1;
		if (Name == "MOUSE5" || Name == "THUMBMOUSEBUTTON2") return VK_XBUTTON2;
		if (Name == "SPACE") return VK_SPACE;
		if (Name == "SHIFT" || Name == "LEFTSHIFT") return VK_LSHIFT;
		if (Name == "CTRL" || Name == "LEFTCTRL") return VK_LCONTROL;
		if (Name == "ALT" || Name == "LEFTALT") return VK_LMENU;
		if (Name == "TAB") return VK_TAB;
		if (Name == "CAPSLOCK") return VK_CAPITAL;
		return Default;
	}

	inline std::string ReadString(const char* Key, const char* Default)
	{
		char Value[64] = {};
		GetPrivateProfileStringA("Client", Key, Default, Value, sizeof(Value), SettingsPath().c_str());
		return Value;
	}

	inline void ReadSettings()
	{
		Settings.bResetOnRelease = ReadString("ResetOnRelease", "0") == std::string("1");
		Settings.EditKey = KeyNameToVK(ReadString("EditKey", "G"), 'G');
		Settings.ResetKey = KeyNameToVK(ReadString("ResetKey", "RightMouse"), VK_RBUTTON);
		Settings.BuildSens = ReadFloat("BuildSensitivity", 1.f);
		Settings.EditSens = ReadFloat("EditSensitivity", 1.f);
	}

	// FInputAxisConfigEntry { FName AxisKeyName; FInputAxisProperties AxisProperties { DeadZone, Sensitivity, Exponent, bInvert } }
	inline float* GetMouseXSensitivityPtr(UObject* PlayerInput)
	{
		static auto AxisConfigOffset = PlayerInput->GetOffset("AxisConfig", false);
		if (AxisConfigOffset == -1) return nullptr;

		static auto AxisPropertiesOffset = FindOffsetStruct("/Script/Engine.InputAxisConfigEntry", "AxisProperties", false);
		static auto SensitivityOffset = FindOffsetStruct("/Script/Engine.InputAxisProperties", "Sensitivity", false);
		if (AxisPropertiesOffset == -1 || SensitivityOffset == -1) return nullptr;

		constexpr int EntrySize = 0x18;
		auto& AxisConfig = PlayerInput->Get<TArray<uint8>>(AxisConfigOffset);
		auto Data = *(uint8**)&AxisConfig; // TArray: Data, Num, Max

		for (int i = 0; i < AxisConfig.Num(); ++i)
		{
			auto Entry = Data + i * EntrySize;
			auto& KeyName = *(FName*)Entry;

			if (KeyName.ToString() == "MouseX")
				return (float*)(Entry + AxisPropertiesOffset + SensitivityOffset);
		}

		return nullptr;
	}

	inline void SetMouseSensitivity(UObject* PlayerInput, float Value)
	{
		static auto Fn = FindObject<UFunction>(L"/Script/Engine.PlayerInput.SetMouseSensitivity");
		if (!Fn) return;
		PlayerInput->ProcessEvent(Fn, &Value);
	}

	// ---- Fortnite's own sensitivity setting (FortClientSettingsRecord) ----
	inline float BaseX = -1.f, BaseY = -1.f;

	inline std::string ReadIni(const char* Key, const char* Default)
	{
		char Value[64] = {};
		GetPrivateProfileStringA("Client", Key, Default, Value, sizeof(Value), SettingsPath().c_str());
		return Value;
	}

	inline void WriteIni(const char* Key, const char* Value)
	{
		WritePrivateProfileStringA("Client", Key, Value, SettingsPath().c_str());
	}

	inline UObject* GetSettingsRecord()
	{
		static UObject* Cached = nullptr;
		static ULONGLONG LastSearch = 0;

		if (Cached && Cached->IsValidLowLevel())
			return Cached;

		auto Now = GetTickCount64();
		if (Now - LastSearch < 2000) return nullptr;
		LastSearch = Now;

		static auto RecordClass = FindObject<UClass>(L"/Script/FortniteGame.FortClientSettingsRecord");
		if (!RecordClass) return nullptr;

		for (auto Record : GetAllObjectsOfClass(RecordClass))
		{
			if (Record && !Record->GetName().starts_with("Default__"))
			{
				Cached = Record;
				LOG_INFO(LogInit, "[StarkClient] Found your settings: {}", Record->GetFullName());
				break;
			}
		}

		return Cached;
	}

	inline float GetRecordFloat(UObject* Record, const wchar_t* FnPath)
	{
		auto Fn = FindObject<UFunction>(FnPath);
		struct { float ReturnValue = 0.f; uint8 Padding[16]; } Params;
		if (Fn) Record->ProcessEvent(Fn, &Params);
		return Params.ReturnValue;
	}

	inline void SetRecordFloat(UObject* Record, const wchar_t* FnPath, float Value)
	{
		auto Fn = FindObject<UFunction>(FnPath);
		struct { float Value; uint8 Padding[16]; } Params{ Value };
		if (Fn) Record->ProcessEvent(Fn, &Params);
	}

	inline EMode GetMode(UObject* Pawn)
	{
		if (!Pawn) return EMode::Normal;

		static auto CurrentWeaponOffset = Pawn->GetOffset("CurrentWeapon", false);
		if (CurrentWeaponOffset == -1) return EMode::Normal;

		auto Weapon = Pawn->Get<UObject*>(CurrentWeaponOffset);
		if (!Weapon) return EMode::Normal;

		static auto BuildingToolClass = FindObject<UClass>(L"/Script/FortniteGame.FortWeap_BuildingTool");
		static auto EditingToolClass = FindObject<UClass>(L"/Script/FortniteGame.FortWeap_EditingTool");

		if (EditingToolClass && Weapon->IsA(EditingToolClass)) return EMode::Edit;
		if (BuildingToolClass && Weapon->IsA(BuildingToolClass)) return EMode::Build;
		return EMode::Normal;
	}

	// ---- Reset on release ----
	inline bool bResetPressedWhileEditing = false;
	inline bool bResetWasDown = false;
	inline int PendingKeyUp = 0;
	inline ULONGLONG PendingKeyUpTime = 0;

	inline bool IsMouseVK(int VK) { return VK == VK_LBUTTON || VK == VK_RBUTTON || VK == VK_MBUTTON || VK == VK_XBUTTON1 || VK == VK_XBUTTON2; }

	inline void SendKey(int VK, bool bDown)
	{
		INPUT Input = {};

		if (IsMouseVK(VK))
		{
			Input.type = INPUT_MOUSE;
			switch (VK)
			{
			case VK_LBUTTON: Input.mi.dwFlags = bDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
			case VK_RBUTTON: Input.mi.dwFlags = bDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
			case VK_MBUTTON: Input.mi.dwFlags = bDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
			default:
				Input.mi.dwFlags = bDown ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
				Input.mi.mouseData = VK == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2;
				break;
			}
		}
		else
		{
			Input.type = INPUT_KEYBOARD;
			Input.ki.wVk = (WORD)VK;
			Input.ki.wScan = (WORD)MapVirtualKeyA(VK, MAPVK_VK_TO_VSC);
			Input.ki.dwFlags = KEYEVENTF_SCANCODE | (bDown ? 0 : KEYEVENTF_KEYUP);
		}

		SendInput(1, &Input, sizeof(INPUT));
	}

	inline bool IsGameFocused()
	{
		DWORD Pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &Pid);
		return Pid == GetCurrentProcessId();
	}

	// Runs on its own thread every millisecond so the confirm happens almost instantly.
	inline std::atomic<bool> bEditingNow = false;

	inline void TapKey(int VK, int HoldMs)
	{
		SendKey(VK, true);
		Sleep(HoldMs);
		SendKey(VK, false);
	}

	inline DWORD WINAPI ResetOnReleaseThread(LPVOID)
	{
		bool bResetWasDown = false;
		bool bResetPressedWhileEditing = false;
		int LastWheelDown = WheelDownCount.load(), LastWheelUp = WheelUpCount.load();

		while (true)
		{
			Sleep(1);

			bool bEditing = bEditingNow.load();

			if (!Settings.bResetOnRelease || !IsGameFocused())
			{
				bResetWasDown = bResetPressedWhileEditing = false;
				LastWheelDown = WheelDownCount.load();
				LastWheelUp = WheelUpCount.load();
				continue;
			}

			// Scroll wheel: a scroll while editing = reset, then confirm right away
			if (Settings.ResetKey == VK_WHEELDOWN || Settings.ResetKey == VK_WHEELUP)
			{
				int& Last = Settings.ResetKey == VK_WHEELDOWN ? LastWheelDown : LastWheelUp;
				int Count = Settings.ResetKey == VK_WHEELDOWN ? WheelDownCount.load() : WheelUpCount.load();

				if (Count != Last)
				{
					Last = Count;

					if (bEditing)
					{
						Sleep(8); // one frame for the game to apply the reset
						TapKey(Settings.EditKey, 12);
					}
				}

				continue;
			}

			bool bResetDown = (GetAsyncKeyState(Settings.ResetKey) & 0x8000) != 0;

			if (bEditing && bResetDown)
				bResetPressedWhileEditing = true;

			// Let go of reset while editing -> confirm straight away
			if (bResetWasDown && !bResetDown && bResetPressedWhileEditing && bEditing)
				TapKey(Settings.EditKey, 12);

			if (!bResetDown)
				bResetPressedWhileEditing = false;

			bResetWasDown = bResetDown;
		}

		return 0;
	}

	inline void Tick()
	{
		auto Now = GetTickCount64();

		if (Now - LastSettingsRead > 1000)
		{
			LastSettingsRead = Now;
			ReadSettings();
		}

		auto& LocalPlayers = GetLocalPlayers();
		if (LocalPlayers.Num() <= 0) return;

		auto LocalPlayer = LocalPlayers.at(0);
		if (!LocalPlayer) return;

		static auto PlayerControllerOffset = LocalPlayer->GetOffset("PlayerController");
		auto PC = LocalPlayer->Get<UObject*>(PlayerControllerOffset);
		if (!PC) return;

		static auto PlayerInputOffset = PC->GetOffset("PlayerInput", false);
		static auto AcknowledgedPawnOffset = PC->GetOffset("AcknowledgedPawn", false);
		if (PlayerInputOffset == -1 || AcknowledgedPawnOffset == -1) return;

		auto Mode = GetMode(PC->Get<UObject*>(AcknowledgedPawnOffset));

		if (Mode == EMode::Normal)
		{
			static auto IsInBuildModeFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.IsInBuildMode");
			struct { bool ReturnValue = false; } BuildParams;
			if (IsInBuildModeFn) PC->ProcessEvent(IsInBuildModeFn, &BuildParams);
			if (BuildParams.ReturnValue) Mode = EMode::Build;
		}

		bEditingNow = Mode == EMode::Edit;

		auto Record = GetSettingsRecord();
		if (!Record) return;

		float CurrentX = GetRecordFloat(Record, L"/Script/FortniteGame.FortClientSettingsRecord.GetMouseSensitivityX");
		float CurrentY = GetRecordFloat(Record, L"/Script/FortniteGame.FortClientSettingsRecord.GetMouseSensitivityY");

		// Last time the game closed while we had build/edit sens on: put the real sensitivity back
		static bool bCheckedLeftover = false;

		if (!bCheckedLeftover)
		{
			bCheckedLeftover = true;

			if (ReadIni("SensApplied", "0") == "1")
			{
				float SavedX = (float)atof(ReadIni("SavedBaseX", "0").c_str());
				float SavedY = (float)atof(ReadIni("SavedBaseY", "0").c_str());

				if (SavedX > 0.f && SavedY > 0.f)
				{
					SetRecordFloat(Record, L"/Script/FortniteGame.FortClientSettingsRecord.SetMouseSensitivityX", SavedX);
					SetRecordFloat(Record, L"/Script/FortniteGame.FortClientSettingsRecord.SetMouseSensitivityY", SavedY);
					CurrentX = SavedX; CurrentY = SavedY;
				}

				WriteIni("SensApplied", "0");
			}
		}

		if (Mode == EMode::Normal)
		{
			if (AppliedMode != EMode::Normal && BaseX > 0.f)
			{
				// back to normal
				SetRecordFloat(Record, L"/Script/FortniteGame.FortClientSettingsRecord.SetMouseSensitivityX", BaseX);
				SetRecordFloat(Record, L"/Script/FortniteGame.FortClientSettingsRecord.SetMouseSensitivityY", BaseY);
				WriteIni("SensApplied", "0");
			}
			else
			{
				BaseX = CurrentX; // keep up with changes in Fortnite's own settings
				BaseY = CurrentY;
			}

			AppliedMode = EMode::Normal;
			return;
		}

		if (BaseX <= 0.f) { BaseX = CurrentX; BaseY = CurrentY; }

		float Multiplier = Mode == EMode::Build ? Settings.BuildSens : Settings.EditSens;
		float WantedX = BaseX * Multiplier, WantedY = BaseY * Multiplier;

		if (AppliedMode != Mode || std::fabs(CurrentX - WantedX) > 0.0001f || std::fabs(CurrentY - WantedY) > 0.0001f)
		{
			if (AppliedMode == EMode::Normal)
			{
				char Buffer[32];
				sprintf_s(Buffer, "%f", BaseX); WriteIni("SavedBaseX", Buffer);
				sprintf_s(Buffer, "%f", BaseY); WriteIni("SavedBaseY", Buffer);
				WriteIni("SensApplied", "1");
			}

			SetRecordFloat(Record, L"/Script/FortniteGame.FortClientSettingsRecord.SetMouseSensitivityX", WantedX);
			SetRecordFloat(Record, L"/Script/FortniteGame.FortClientSettingsRecord.SetMouseSensitivityY", WantedY);
			AppliedMode = Mode;

			static int Logged = 0;
			if (Logged++ < 3)
				LOG_INFO(LogInit, "[StarkClient] {} sensitivity: {} -> {}", Mode == EMode::Build ? "Build" : "Edit", BaseX, WantedX);
		}
	}

	// We tick from inside ProcessEvent so we're always on the game thread.
	inline void (*ProcessEventOriginal)(UObject*, UFunction*, void*) = nullptr;

	inline void ProcessEventHook(UObject* Object, UFunction* Function, void* Params)
	{
		if (!bInTick)
		{
			auto Now = GetTickCount64();

			if (Now - LastTick >= 10)
			{
				LastTick = Now;
				bInTick = true;
				__try { Tick(); }
				__except (EXCEPTION_EXECUTE_HANDLER) {}
				bInTick = false;
			}
		}

		return ProcessEventOriginal(Object, Function, Params);
	}

	// Same as Reboot's console DLL: give the game a working ` console, so StarkClient.dll
	// can be picked as the "Unreal engine console" DLL in Reboot Launcher.
	inline void SpawnConsole()
	{
		auto Engine = GetEngine();
		if (!Engine) return;

		auto GameViewport = Engine->Get("GameViewport");
		if (!GameViewport) return;

		auto ViewportConsolePtr = GameViewport->GetPtr("ViewportConsole");
		if (ViewportConsolePtr && !*ViewportConsolePtr)
			*ViewportConsolePtr = UGameplayStatics::SpawnObject(FindObject<UClass>(L"/Script/Engine.Console"), GameViewport);
	}

	inline void Start()
	{
		ReadSettings();
		SpawnConsole();
		CreateThread(nullptr, 0, MouseHookThread, nullptr, 0, nullptr);
		CreateThread(nullptr, 0, ResetOnReleaseThread, nullptr, 0, nullptr);

		if (!Addresses::ProcessEvent)
		{
			LOG_ERROR(LogInit, "[StarkClient] Couldn't find ProcessEvent, client features are off.");
			return;
		}

		MH_CreateHook((PVOID)Addresses::ProcessEvent, (PVOID)ProcessEventHook, (PVOID*)&ProcessEventOriginal);
		MH_EnableHook((PVOID)Addresses::ProcessEvent);

		LOG_INFO(LogInit, "[StarkClient] Running! Build sensitivity x{}, edit sensitivity x{}, reset on release {}", Settings.BuildSens, Settings.EditSens, Settings.bResetOnRelease);
	}
}
