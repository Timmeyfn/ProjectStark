#pragma once

// Project Stark client features. These run inside YOUR Fortnite (not the server):
// the launcher injects a copy of this DLL named "StarkClient.dll" into the game client.
//
//  - Build sensitivity: mouse sensitivity multiplier while holding the building tool
//  - Edit sensitivity:  mouse sensitivity multiplier while editing
//
// Settings live in %LOCALAPPDATA%\ProjectStark\client_settings.ini (the launcher's Settings page writes it)
// and are re-read every second, so you can change them while playing.

#include <Windows.h>
#include <string>
#include <atomic>
#include <cmath>

#include "reboot.h"
#include "log.h"
#include "addresses.h"
#include "inc.h"
#include <MinHook/MinHook.h>

namespace StarkClient
{
	struct FSettings
	{
		float BuildSens = 1.f;
		float EditSens = 1.f;
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

	inline void ReadSettings()
	{
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

		auto PlayerInput = PC->Get<UObject*>(PlayerInputOffset);
		if (!PlayerInput) return;

		auto SensPtr = GetMouseXSensitivityPtr(PlayerInput);
		if (!SensPtr) return;

		auto Mode = GetMode(PC->Get<UObject*>(AcknowledgedPawnOffset));

		if (Mode == EMode::Normal)
		{
			if (AppliedMode != EMode::Normal && BaseSensitivity > 0.f)
				SetMouseSensitivity(PlayerInput, BaseSensitivity); // back to normal

			AppliedMode = EMode::Normal;
			BaseSensitivity = *SensPtr; // keep up with changes from Fortnite's own settings
			return;
		}

		if (BaseSensitivity <= 0.f)
			BaseSensitivity = *SensPtr;

		float Multiplier = Mode == EMode::Build ? Settings.BuildSens : Settings.EditSens;
		float Wanted = BaseSensitivity * Multiplier;

		if (AppliedMode != Mode || std::fabs(*SensPtr - Wanted) > 0.0001f)
		{
			SetMouseSensitivity(PlayerInput, Wanted);
			AppliedMode = Mode;
		}
	}

	// We tick from inside ProcessEvent so we're always on the game thread.
	inline void (*ProcessEventOriginal)(UObject*, UFunction*, void*) = nullptr;

	inline void ProcessEventHook(UObject* Object, UFunction* Function, void* Params)
	{
		if (!bInTick)
		{
			auto Now = GetTickCount64();

			if (Now - LastTick >= 30)
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

	inline void Start()
	{
		ReadSettings();

		if (!Addresses::ProcessEvent)
		{
			LOG_ERROR(LogInit, "[StarkClient] Couldn't find ProcessEvent, client features are off.");
			return;
		}

		MH_CreateHook((PVOID)Addresses::ProcessEvent, (PVOID)ProcessEventHook, (PVOID*)&ProcessEventOriginal);
		MH_EnableHook((PVOID)Addresses::ProcessEvent);

		LOG_INFO(LogInit, "[StarkClient] Running! Build sensitivity x{}, edit sensitivity x{}", Settings.BuildSens, Settings.EditSens);
	}
}
