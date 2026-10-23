#pragma once

// ============================================================================================
//  Reboot bots
//
//  Bots are real AI-controlled Fortnite pawns (the same pawn/controller classes Epic used for
//  their "Phoebe" bots), driven by a small brain written here:
//
//    Warmup   : wander around spawn island, jump, emote
//    Bus      : ride the bus (hidden), pick a place to land, drop out when the bus gets close
//    Skydive  : skydive + steer towards the landing spot, open the glider near the ground
//    Ground   : loot chests and floor loot, heal, move with the storm, fight anyone they can see
//
//  Why AI controllers: a PlayerController with no real player behind it never gets its movement
//  simulated by the server, which is why the old bots froze in the air after leaving the bus.
//
//  Safety: every bot is ticked inside an SEH guard, so a bad bot gets removed instead of taking
//  the whole server down, and every object is re-validated through the global object array
//  before it is touched, so dead/garbage-collected pawns can't crash us.
// ============================================================================================

#include "FortGameModeAthena.h"
#include "FortGameStateAthena.h"
#include "OnlineReplStructs.h"
#include "FortAthenaAIBotController.h"
#include "BuildingContainer.h"
#include "FortPlayerController.h"
#include "FortPlayerControllerAthena.h"
#include "FortPlayerPawnAthena.h"
#include "FortPlayerStateAthena.h"
#include "FortSafeZoneIndicator.h"
#include "FortPickup.h"
#include "FortWeapon.h"
#include "FortWeaponItemDefinition.h"
#include "FortInventory.h"
#include "GameplayStatics.h"
#include "KismetSystemLibrary.h"
#include "KismetStringLibrary.h"
#include "UObjectArray.h"
#include "botnames.h"
#include "botsettings.h"

#include <random>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <cctype>

// --------------------------------------------------------------------------------------------
//  Small math helpers (FVector's operators in Reboot aren't const, so keep our own)
// --------------------------------------------------------------------------------------------

namespace BotMath
{
	constexpr float PI_F = 3.14159265f;

	inline FVector Add(const FVector& A, const FVector& B) { return FVector(A.X + B.X, A.Y + B.Y, A.Z + B.Z); }
	inline FVector Sub(const FVector& A, const FVector& B) { return FVector(A.X - B.X, A.Y - B.Y, A.Z - B.Z); }
	inline FVector Mul(const FVector& A, float S) { return FVector(A.X * S, A.Y * S, A.Z * S); }
	inline float Size(const FVector& A) { return std::sqrt((float)(A.X * A.X + A.Y * A.Y + A.Z * A.Z)); }
	inline float Size2D(const FVector& A) { return std::sqrt((float)(A.X * A.X + A.Y * A.Y)); }
	inline float Dist(const FVector& A, const FVector& B) { return Size(Sub(A, B)); }
	inline float Dist2D(const FVector& A, const FVector& B) { return Size2D(Sub(A, B)); }

	inline FVector Normal2D(const FVector& A)
	{
		float L = Size2D(A);
		if (L < 0.001f) return FVector(0, 0, 0);
		return FVector(A.X / L, A.Y / L, 0);
	}

	inline float YawTo(const FVector& From, const FVector& To)
	{
		return std::atan2((float)(To.Y - From.Y), (float)(To.X - From.X)) * 180.f / PI_F;
	}

	inline float YawOf(const FVector& Direction)
	{
		return std::atan2((float)Direction.Y, (float)Direction.X) * 180.f / PI_F;
	}

	inline float PitchTo(const FVector& From, const FVector& To)
	{
		float Horizontal = Dist2D(From, To);
		return std::atan2((float)(To.Z - From.Z), Horizontal) * 180.f / PI_F;
	}

	inline float NormalizeAngle(float A)
	{
		while (A > 180.f) A -= 360.f;
		while (A < -180.f) A += 360.f;
		return A;
	}

	inline FVector DirFromYaw(float YawDeg)
	{
		float R = YawDeg * PI_F / 180.f;
		return FVector(std::cos(R), std::sin(R), 0);
	}

	inline std::mt19937& Rng()
	{
		static std::mt19937 Gen{ std::random_device{}() };
		return Gen;
	}

	inline float RandF(float Min, float Max)
	{
		if (Max <= Min) return Min;
		std::uniform_real_distribution<float> Dist(Min, Max);
		return Dist(Rng());
	}

	inline int RandI(int Min, int Max) // inclusive
	{
		if (Max <= Min) return Min;
		std::uniform_int_distribution<int> Dist(Min, Max);
		return Dist(Rng());
	}

	inline bool Chance(float P) { return RandF(0.f, 1.f) < P; }

	inline float Lerp(float A, float B, float T) { return A + (B - A) * T; }
	inline float Clamp(float V, float Min, float Max) { return V < Min ? Min : (V > Max ? Max : V); }
}

// --------------------------------------------------------------------------------------------
//  Object validity: keep a pointer + its slot in the global object array, and only use the
//  pointer if the slot still holds the same object and it isn't pending kill.
// --------------------------------------------------------------------------------------------

struct FBotObjectRef
{
	UObject* Object = nullptr;
	int Index = -1;

	void Set(UObject* InObject)
	{
		Object = InObject;
		Index = InObject ? InObject->InternalIndex : -1;
	}

	void Reset() { Object = nullptr; Index = -1; }

	bool IsValid() const
	{
		if (!Object || Index < 0)
			return false;

		auto Item = GetItemByIndex(Index);

		if (!Item || Item->Object != Object || Item->IsPendingKill())
			return false;

		return true;
	}

	template <typename T = UObject>
	T* Get() const { return IsValid() ? (T*)Object : nullptr; }
};

inline bool IsActorUsable(AActor* Actor)
{
	if (!Actor)
		return false;

	auto Item = GetItemByIndex(Actor->InternalIndex);

	if (!Item || Item->Object != Actor || Item->IsPendingKill())
		return false;

	return !Actor->IsActorBeingDestroyed();
}

// --------------------------------------------------------------------------------------------
//  Reflection-based UFunction calls. Parameters are looked up by name at runtime, so a wrong
//  guess at a struct layout can't corrupt memory; a missing parameter just stays zeroed.
// --------------------------------------------------------------------------------------------

inline std::vector<std::string> BotListFunctionParams(UFunction* Fn)
{
	std::vector<std::string> Names;

	if (!Fn)
		return Names;

	void* Property = *(void**)(__int64(Fn) + Offsets::Children);

	while (Property)
	{
		Names.push_back(GetFNameOfProp(Property)->ToString() + "@" + std::to_string(*(int*)(__int64(Property) + Offsets::Offset_Internal)));
		Property = GetNext(Property);
	}

	return Names;
}

inline int BotParamOffset(UFunction* Fn, const std::string& Name)
{
	static std::unordered_map<UFunction*, std::unordered_map<std::string, int>> Cache;

	auto& FnCache = Cache[Fn];
	auto It = FnCache.find(Name);

	if (It != FnCache.end())
		return It->second;

	int Offset = Fn->GetOffsetFunc(Name, false);
	FnCache[Name] = Offset;

	if (Offset == -1)
	{
		std::string Params;
		for (auto& P : BotListFunctionParams(Fn)) Params += P + " ";
		LOG_WARN(LogBots, "Function {} has no parameter '{}'. Parameters: {}", Fn->GetName(), Name, Params);
	}

	return Offset;
}

class FBotCall
{
public:
	UFunction* Fn = nullptr;
	std::vector<uint8_t> Buffer;

	explicit FBotCall(UFunction* InFn) : Fn(InFn)
	{
		int Size = Fn ? Fn->GetPropertiesSize() : 0;
		Buffer.resize((Size > 0 ? Size : 1) + 32, 0);
	}

	template <typename T>
	FBotCall& Set(const std::string& Name, const T& Value)
	{
		if (!Fn) return *this;

		int Offset = BotParamOffset(Fn, Name);

		if (Offset >= 0 && Offset + sizeof(T) <= Buffer.size())
			memcpy(Buffer.data() + Offset, &Value, sizeof(T));

		return *this;
	}

	template <typename T>
	T Get(const std::string& Name, T Default = T())
	{
		if (!Fn) return Default;

		int Offset = BotParamOffset(Fn, Name);

		if (Offset < 0 || Offset + sizeof(T) > Buffer.size())
			return Default;

		T Out;
		memcpy(&Out, Buffer.data() + Offset, sizeof(T));
		return Out;
	}

	void* Ptr(const std::string& Name)
	{
		int Offset = Fn ? BotParamOffset(Fn, Name) : -1;
		return Offset >= 0 ? Buffer.data() + Offset : nullptr;
	}

	bool Call(UObject* Object)
	{
		if (!Fn || !Object)
			return false;

		Object->ProcessEvent(Fn, Buffer.data());
		return true;
	}
};

inline UFunction* BotFindFn(const wchar_t* Path)
{
	return FindObject<UFunction>(Path);
}

// --------------------------------------------------------------------------------------------
//  Thin wrappers around the engine functions the brain uses
// --------------------------------------------------------------------------------------------

namespace BotEngine
{
	inline void AddMovementInput(UObject* Pawn, const FVector& Direction, float Scale = 1.f)
	{
		static auto Fn = BotFindFn(L"/Script/Engine.Pawn.AddMovementInput");
		FBotCall(Fn).Set("WorldDirection", Direction).Set("ScaleValue", Scale).Set("bForce", true).Call(Pawn);
	}

	inline void SetControlRotation(UObject* Controller, const FRotator& Rotation)
	{
		static auto Fn = BotFindFn(L"/Script/Engine.Controller.SetControlRotation");
		FBotCall(Fn).Set("NewRotation", Rotation).Call(Controller);
	}

	inline void Jump(UObject* Pawn)
	{
		static auto Fn = BotFindFn(L"/Script/Engine.Character.Jump");
		if (Fn) Pawn->ProcessEvent(Fn);
	}

	inline void StartFire(UObject* Pawn)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortPawn.PawnStartFire");
		FBotCall(Fn).Set<uint8>("FireModeNum", 0).Call(Pawn);
	}

	inline void StopFire(UObject* Pawn)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortPawn.PawnStopFire");
		FBotCall(Fn).Set<uint8>("FireModeNum", 0).Call(Pawn);
	}

	inline bool CallBoolGetter(UObject* Object, UFunction* Fn)
	{
		if (!Fn || !Object) return false;
		FBotCall Call(Fn);
		Call.Call(Object);
		return Call.Get<bool>("ReturnValue", false);
	}

	inline bool IsSkydiving(UObject* Pawn)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortPlayerPawn.IsSkydiving");
		return CallBoolGetter(Pawn, Fn);
	}

	inline bool IsParachuteOpen(UObject* Pawn)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortPlayerPawn.IsParachuteOpen");
		return CallBoolGetter(Pawn, Fn);
	}

	inline void BeginSkydiving(UObject* Pawn)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortPlayerPawn.BeginSkydiving");
		FBotCall(Fn).Set("bFromBus", true).Call(Pawn);
	}

	inline void OpenParachute(UObject* Pawn)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortPlayerPawn.BP_ForceOpenParachute");
		static auto FnFallback = BotFindFn(L"/Script/FortniteGame.FortPlayerPawn.ForceOpenParachuteAndOverrideGlider");

		if (Fn)
			FBotCall(Fn).Call(Pawn);
		else if (FnFallback)
			FBotCall(FnFallback).Call(Pawn);
	}

	inline void ResetFallingHeight(UObject* Pawn)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortPawn.ResetFallingHeight");
		if (Fn) FBotCall(Fn).Call(Pawn);
	}

	inline void SetHidden(UObject* Actor, bool bHidden)
	{
		static auto Fn = BotFindFn(L"/Script/Engine.Actor.SetActorHiddenInGame");
		FBotCall(Fn).Set("bNewHidden", bHidden).Call(Actor);
	}

	inline FVector GetVelocity(UObject* Actor)
	{
		static auto Fn = BotFindFn(L"/Script/Engine.Actor.GetVelocity");
		FBotCall Call(Fn);
		Call.Call(Actor);
		return Call.Get<FVector>("ReturnValue", FVector());
	}

	inline void StopEpicBrain(AController* Controller)
	{
		static auto StopLogicFn = BotFindFn(L"/Script/AIModule.BrainComponent.StopLogic");

		if (!StopLogicFn || !Controller)
			return;

		static auto BrainComponentOffset = Controller->GetOffset("BrainComponent", false);

		if (BrainComponentOffset == -1)
			return;

		auto Brain = Controller->Get<UObject*>(BrainComponentOffset);

		if (Brain)
			FBotCall(StopLogicFn).Call(Brain); // Reason (FString) left empty
	}

	// Calls a function whose first parameter is a bool, whatever that parameter is called.
	inline void CallWithFirstBool(UObject* Object, UFunction* Fn, bool bValue)
	{
		if (!Fn || !Object) return;

		auto Params = BotListFunctionParams(Fn);
		FBotCall Call(Fn);

		if (!Params.empty())
		{
			auto Name = Params[0].substr(0, Params[0].find('@'));
			if (Name != "ReturnValue") Call.Set(Name, bValue);
		}

		Call.Call(Object);
	}

	// Fortnite puts AI to "sleep" (frozen in place, no movement or gravity) when it thinks nobody
	// cares about it. Keep our bots awake and make sure their movement is actually running.
	inline void KeepAwakeAndMoving(UObject* Controller, UObject* Pawn, bool bResetMovementMode)
	{
		static auto WakeUpFn = BotFindFn(L"/Script/FortniteGame.FortAIController.WakeUp");
		static auto SetIsSleepingFn = BotFindFn(L"/Script/FortniteGame.FortAIController.SetIsSleeping");
		static auto SetAlwaysRelevantFn = BotFindFn(L"/Script/FortniteGame.FortAIController.SetAlwaysGameplayRelevant");
		static auto SetActorTickEnabledFn = BotFindFn(L"/Script/Engine.Actor.SetActorTickEnabled");
		static auto SetComponentTickEnabledFn = BotFindFn(L"/Script/Engine.ActorComponent.SetComponentTickEnabled");
		static auto ActivateFn = BotFindFn(L"/Script/Engine.ActorComponent.Activate");
		static auto SetMovementModeFn = BotFindFn(L"/Script/Engine.CharacterMovementComponent.SetMovementMode");

		static auto FortAIControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortAIController");

		if (Controller && FortAIControllerClass && Controller->IsA(FortAIControllerClass))
		{
			CallWithFirstBool(Controller, SetIsSleepingFn, false);
			if (WakeUpFn) FBotCall(WakeUpFn).Call(Controller);
			CallWithFirstBool(Controller, SetAlwaysRelevantFn, true);
		}

		if (!Pawn)
			return;

		CallWithFirstBool(Pawn, SetActorTickEnabledFn, true);

		// Some pawn flags that can stop the server from simulating movement
		static auto SkipSimOffset = Pawn->GetOffset("bShouldSkipMovementFullSimulation", false);

		if (SkipSimOffset != -1)
		{
			static auto SkipSimMask = GetFieldMask(Pawn->GetProperty("bShouldSkipMovementFullSimulation"));
			Pawn->SetBitfieldValue(SkipSimOffset, SkipSimMask, false);
		}

		static auto CharacterMovementOffset = Pawn->GetOffset("CharacterMovement", false);

		if (CharacterMovementOffset == -1)
			return;

		auto Movement = *(UObject**)(__int64(Pawn) + CharacterMovementOffset);

		if (!Movement)
			return;

		CallWithFirstBool(Movement, SetComponentTickEnabledFn, true);
		CallWithFirstBool(Movement, ActivateFn, false);

		if (bResetMovementMode && SetMovementModeFn)
			FBotCall(SetMovementModeFn).Set<uint8>("NewMovementMode", 3 /* MOVE_Falling: gravity takes over, lands into walking */).Call(Movement);
	}

	inline int GetRole(UObject* Actor, const char* Name);

	inline std::string DescribeMovement(UObject* Pawn, UObject* Controller)
	{
		std::string Out;

		static auto IsLocalControllerFn = BotFindFn(L"/Script/Engine.Controller.IsLocalController");
		if (Controller) Out += " local=" + std::to_string(CallBoolGetter(Controller, IsLocalControllerFn));
		if (Controller) Out += " ctrlrole=" + std::to_string(GetRole(Controller, "Role")) + "/" + std::to_string(GetRole(Controller, "RemoteRole"));
		Out += " pawnrole=" + std::to_string(GetRole(Pawn, "Role")) + "/" + std::to_string(GetRole(Pawn, "RemoteRole"));
		auto PawnController = (UObject*)((APawn*)Pawn)->GetController();
		Out += " pawnctrl=" + std::string(PawnController ? PawnController->ClassPrivate->GetName() : "NONE");

		static auto CharacterMovementOffset = Pawn->GetOffset("CharacterMovement", false);
		auto Movement = CharacterMovementOffset != -1 ? *(UObject**)(__int64(Pawn) + CharacterMovementOffset) : nullptr;

		if (Movement)
		{
			static auto IsTickEnabledFn = BotFindFn(L"/Script/Engine.ActorComponent.IsComponentTickEnabled");
			static auto IsActiveFn = BotFindFn(L"/Script/Engine.ActorComponent.IsActive");
			Out += " movecomp=" + Movement->ClassPrivate->GetName();
			Out += " movetick=" + std::to_string(CallBoolGetter(Movement, IsTickEnabledFn));
			Out += " moveactive=" + std::to_string(CallBoolGetter(Movement, IsActiveFn));
		}

		static auto IsActorTickEnabledFn = BotFindFn(L"/Script/Engine.Actor.IsActorTickEnabled");
		Out += " actortick=" + std::to_string(CallBoolGetter(Pawn, IsActorTickEnabledFn));

		static auto AILODOffset = Pawn->GetOffset("AILODComponent", false);
		auto AILOD = AILODOffset != -1 ? *(UObject**)(__int64(Pawn) + AILODOffset) : nullptr;
		Out += " ailod=" + std::string(AILOD ? AILOD->ClassPrivate->GetName() : "none");

		auto Velocity = GetVelocity(Pawn);
		Out += " vel=" + std::to_string((int)Velocity.X) + "," + std::to_string((int)Velocity.Y) + "," + std::to_string((int)Velocity.Z);
		return Out;
	}

	inline int GetRole(UObject* Actor, const char* Name)
	{
		int Offset = Actor->GetOffset(Name, false);
		return Offset == -1 ? -1 : (int)Actor->Get<uint8>(Offset);
	}

	// The server only simulates movement for pawns whose controller it considers "local"
	// (AController::IsLocalController: RemoteRole != AutonomousProxy && Role == Authority).
	// Epic's bot controller replicates like a remote player, so force it to be local.
	inline void MakeControllerLocal(UObject* Controller)
	{
		static auto RemoteRoleOffset = Controller->GetOffset("RemoteRole", false);
		static auto RoleOffset = Controller->GetOffset("Role", false);

		if (RemoteRoleOffset != -1 && Controller->Get<uint8>(RemoteRoleOffset) == 2 /* ROLE_AutonomousProxy */)
			Controller->Get<uint8>(RemoteRoleOffset) = 1; // ROLE_SimulatedProxy

		if (RoleOffset != -1)
			Controller->Get<uint8>(RoleOffset) = 3; // ROLE_Authority
	}

	inline void LetMovementRunWithoutController(UObject* Pawn)
	{
		static auto CharacterMovementOffset = Pawn->GetOffset("CharacterMovement", false);
		if (CharacterMovementOffset == -1) return;
		auto Movement = *(UObject**)(__int64(Pawn) + CharacterMovementOffset);
		if (!Movement) return;

		static auto Offset = Movement->GetOffset("bRunPhysicsWithNoController", false);

		if (Offset != -1)
		{
			static auto Mask = GetFieldMask(Movement->GetProperty("bRunPhysicsWithNoController"));
			Movement->SetBitfieldValue(Offset, Mask, true);
		}
	}

	inline int GetMovementMode(UObject* Pawn)
	{
		static auto CharacterMovementOffset = Pawn->GetOffset("CharacterMovement", false);
		if (CharacterMovementOffset == -1) return -1;
		auto Movement = *(UObject**)(__int64(Pawn) + CharacterMovementOffset);
		if (!Movement) return -2;
		static auto MovementModeOffset = Movement->GetOffset("MovementMode", false);
		return MovementModeOffset == -1 ? -3 : (int)Movement->Get<uint8>(MovementModeOffset);
	}

	inline void ThankBusDriver(UObject* Controller)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortAthenaAIBotController.ThankBusDriver");
		if (Fn) FBotCall(Fn).Call(Controller);
	}

	inline bool PlayEmote(UObject* Controller, UObject* EmoteDefinition)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortAthenaAIBotController.PlayEmote");
		static std::string EmoteParamName;
		static bool bLookedUp = false;

		if (!Fn || !EmoteDefinition)
			return false;

		if (!bLookedUp)
		{
			bLookedUp = true;
			auto Params = BotListFunctionParams(Fn);

			for (auto& P : Params)
			{
				auto Name = P.substr(0, P.find('@'));

				if (EmoteParamName.empty() && Name != "ReturnValue")
					EmoteParamName = Name; // first parameter is the emote asset
			}

			std::string All; for (auto& P : Params) All += P + " ";
			LOG_INFO(LogBots, "PlayEmote params: {} (using '{}')", All, EmoteParamName);
		}

		if (EmoteParamName.empty())
			return false;

		struct { FName PrimaryAssetType; FName PrimaryAssetName; } AssetId;
		static FName DanceType = UKismetStringLibrary::Conv_StringToName(L"AthenaDance");
		AssetId.PrimaryAssetType = DanceType;
		AssetId.PrimaryAssetName = EmoteDefinition->NamePrivate;

		FBotCall(Fn).Set(EmoteParamName, AssetId).Call(Controller);
		return true;
	}

	inline void ForceKill(UObject* Pawn, UObject* KillerController, UObject* KillerActor)
	{
		static auto Fn = BotFindFn(L"/Script/FortniteGame.FortPawn.ForceKill");

		if (!Fn)
			return;

		FBotCall Call(Fn);
		if (KillerController) Call.Set("KillerController", KillerController);
		if (KillerActor) Call.Set("KillerActor", KillerActor);
		Call.Call(Pawn);
	}

	inline UObject* SpawnDefaultController(UObject* Pawn)
	{
		static auto Fn = BotFindFn(L"/Script/Engine.Pawn.SpawnDefaultController");
		if (!Fn) return nullptr;
		FBotCall(Fn).Call(Pawn);
		return ((APawn*)Pawn)->GetController();
	}

	// Line trace on the Visibility channel. Returns true if something was hit.
	inline bool Trace(UObject* WorldContext, const FVector& Start, const FVector& End, FVector* OutHitLocation = nullptr)
	{
		static auto Fn = BotFindFn(L"/Script/Engine.KismetSystemLibrary.LineTraceSingle");
		static auto Lib = FindObject(L"/Script/Engine.Default__KismetSystemLibrary");

		if (!Fn || !Lib)
			return false;

		FBotCall Call(Fn);
		Call.Set("WorldContextObject", WorldContext);
		Call.Set("Start", Start);
		Call.Set("End", End);
		Call.Set<uint8>("TraceChannel", 0); // TraceTypeQuery1 = Visibility
		Call.Set("bTraceComplex", false);
		Call.Set("bIgnoreSelf", true);
		Call.Call(Lib);

		bool bHit = Call.Get<bool>("ReturnValue", false);

		if (bHit && OutHitLocation)
		{
			auto HitPtr = (FHitResult*)Call.Ptr("OutHit");
			if (HitPtr) *OutHitLocation = HitPtr->GetLocation();
		}

		return bHit;
	}

	// Height of Location above the ground below it (huge number if there's no ground).
	inline float HeightAboveGround(UObject* WorldContext, const FVector& Location, FVector* OutGround = nullptr)
	{
		FVector Hit;
		FVector End = FVector(Location.X, Location.Y, Location.Z - 200000.f);

		if (Trace(WorldContext, Location, End, &Hit))
		{
			if (OutGround) *OutGround = Hit;
			return (float)(Location.Z - Hit.Z);
		}

		return 100000.f;
	}
}

// --------------------------------------------------------------------------------------------
//  Items
// --------------------------------------------------------------------------------------------

enum class EBotWeaponKind : uint8
{
	None, Pickaxe, Assault, SMG, Shotgun, Pistol, Sniper, Other
};

enum class EBotHealKind : uint8
{
	None, ShieldSmall, ShieldBig, Medkit, Bandage, Slurp, Chug
};

struct FBotWeaponStats
{
	float Damage;        // per hit, body shot
	float FireInterval;  // seconds between shots
	float Range;         // effective range in UU
	float ReloadTime;
	float PreferredDist; // how far they like to stand from the target
};

inline FBotWeaponStats GetWeaponStats(EBotWeaponKind Kind)
{
	switch (Kind)
	{
	case EBotWeaponKind::Pickaxe: return { 20.f, 0.6f, 220.f, 0.f, 120.f };
	case EBotWeaponKind::Assault: return { 30.f, 0.18f, 5000.f, 2.2f, 1800.f };
	case EBotWeaponKind::SMG:     return { 17.f, 0.09f, 2200.f, 2.0f, 900.f };
	case EBotWeaponKind::Shotgun: return { 75.f, 0.9f, 900.f, 4.0f, 350.f };
	case EBotWeaponKind::Pistol:  return { 24.f, 0.3f, 2500.f, 1.4f, 1200.f };
	case EBotWeaponKind::Sniper:  return { 100.f, 2.5f, 12000.f, 3.0f, 4000.f };
	default:                      return { 25.f, 0.3f, 2500.f, 2.0f, 1200.f };
	}
}

inline bool ContainsI(const std::string& Haystack, const char* Needle)
{
	std::string H = Haystack, N = Needle;
	std::transform(H.begin(), H.end(), H.begin(), ::tolower);
	std::transform(N.begin(), N.end(), N.begin(), ::tolower);
	return H.find(N) != std::string::npos;
}

inline EBotWeaponKind ClassifyWeapon(UObject* Def)
{
	if (!Def) return EBotWeaponKind::None;

	auto Name = Def->GetName();

	if (ContainsI(Name, "Harvest") || ContainsI(Name, "Pickaxe")) return EBotWeaponKind::Pickaxe;

	// Things bots shouldn't use (explosives, traps, building tools...)
	if (ContainsI(Name, "Launcher") || ContainsI(Name, "Grenade") || ContainsI(Name, "RPG") || ContainsI(Name, "Rocket")
		|| ContainsI(Name, "Trap") || ContainsI(Name, "Build") || ContainsI(Name, "Edit") || ContainsI(Name, "C4")
		|| ContainsI(Name, "Remote") || ContainsI(Name, "Bow") || ContainsI(Name, "Fish") || ContainsI(Name, "Gadget"))
		return EBotWeaponKind::None;

	if (!ContainsI(Name, "WID_")) return EBotWeaponKind::None;

	if (ContainsI(Name, "Shotgun")) return EBotWeaponKind::Shotgun;
	if (ContainsI(Name, "SMG") || ContainsI(Name, "MachinePistol") || ContainsI(Name, "Minigun") || ContainsI(Name, "LMG")) return EBotWeaponKind::SMG;
	if (ContainsI(Name, "Sniper") || ContainsI(Name, "DMR")) return EBotWeaponKind::Sniper;
	if (ContainsI(Name, "Pistol") || ContainsI(Name, "Revolver") || ContainsI(Name, "HandCannon")) return EBotWeaponKind::Pistol;
	if (ContainsI(Name, "Assault") || ContainsI(Name, "Rifle") || ContainsI(Name, "Burst") || ContainsI(Name, "Scar")) return EBotWeaponKind::Assault;

	return EBotWeaponKind::Other;
}

inline EBotHealKind ClassifyHeal(UObject* Def)
{
	if (!Def) return EBotHealKind::None;

	auto Name = Def->GetName();

	if (ContainsI(Name, "ShieldSmall")) return EBotHealKind::ShieldSmall;
	if (ContainsI(Name, "Medkit")) return EBotHealKind::Medkit;
	if (ContainsI(Name, "Bandage")) return EBotHealKind::Bandage;
	if (ContainsI(Name, "PurpleStuff") || ContainsI(Name, "Slurp")) return EBotHealKind::Slurp;
	if (ContainsI(Name, "ChillBronco") || ContainsI(Name, "Chug")) return EBotHealKind::Chug;
	if (ContainsI(Name, "Shields") || ContainsI(Name, "Shield")) return EBotHealKind::ShieldBig;

	return EBotHealKind::None;
}

inline int WeaponRarityScore(UObject* Def)
{
	if (!Def) return 0;
	auto Name = Def->GetName();

	if (ContainsI(Name, "_SR_") || ContainsI(Name, "Legendary")) return 5;
	if (ContainsI(Name, "_VR_") || ContainsI(Name, "Epic")) return 4;
	if (ContainsI(Name, "_R_") || ContainsI(Name, "Rare")) return 3;
	if (ContainsI(Name, "_UC_") || ContainsI(Name, "Uncommon")) return 2;
	return 1;
}

inline int WeaponScore(UObject* Def)
{
	auto Kind = ClassifyWeapon(Def);
	int Base = 0;

	switch (Kind)
	{
	case EBotWeaponKind::Assault: Base = 60; break;
	case EBotWeaponKind::Shotgun: Base = 58; break;
	case EBotWeaponKind::SMG: Base = 52; break;
	case EBotWeaponKind::Pistol: Base = 35; break;
	case EBotWeaponKind::Sniper: Base = 30; break;
	case EBotWeaponKind::Other: Base = 30; break;
	default: return 0;
	}

	return Base + WeaponRarityScore(Def) * 8;
}

struct FBotItem
{
	UFortItemDefinition* Definition = nullptr;
	int Count = 1;
	int LoadedAmmo = 0;
	FGuid Guid{};
	EBotWeaponKind WeaponKind = EBotWeaponKind::None;
	EBotHealKind HealKind = EBotHealKind::None;
	int Score = 0;
};

inline FGuid MakeBotGuid()
{
	FGuid Guid;
	Guid.A = (unsigned int)BotMath::Rng()();
	Guid.B = (unsigned int)BotMath::Rng()();
	Guid.C = (unsigned int)BotMath::Rng()();
	Guid.D = (unsigned int)BotMath::Rng()();
	return Guid;
}

// --------------------------------------------------------------------------------------------
//  World caches shared by all bots (refreshed on a timer, not every frame)
// --------------------------------------------------------------------------------------------

struct FBotCachedPickup
{
	FBotObjectRef Ref;
	FVector Location;
	UFortItemDefinition* Definition = nullptr;
};

struct FBotCachedContainer
{
	FBotObjectRef Ref;
	FVector Location;
};

namespace BotWorld
{
	inline UObject* LastWorld = nullptr;

	inline std::vector<FBotObjectRef> Pawns;            // every FortPlayerPawnAthena (players + bots)
	inline float NextPawnRefresh = 0.f;

	inline std::vector<FBotCachedPickup> Pickups;
	inline float NextPickupRefresh = 0.f;

	inline std::vector<FBotCachedContainer> Chests;      // chests + ammo boxes
	inline bool bBuiltChestCache = false;

	inline EAthenaGamePhase LastPhase = EAthenaGamePhase::None;
	inline float AircraftPhaseStartTime = -1.f;

	inline bool bRealFireVerified = false;     // we saw a bot's real gun consume ammo
	inline bool bUseSimulatedShots = false;    // real firing doesn't work for AI here, so simulate hits
	inline bool bWonAnnounced = false;
}

inline UClass* GetPlayerPawnAthenaClass()
{
	static auto Class = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerPawnAthena");
	return Class;
}

inline void RefreshPawnCache(float Now)
{
	if (Now < BotWorld::NextPawnRefresh)
		return;

	BotWorld::NextPawnRefresh = Now + 0.5f;
	BotWorld::Pawns.clear();

	auto Class = GetPlayerPawnAthenaClass();

	if (!Class)
		return;

	auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), Class);

	for (int i = 0; i < Actors.Num(); ++i)
	{
		FBotObjectRef Ref;
		Ref.Set(Actors.at(i));
		BotWorld::Pawns.push_back(Ref);
	}

	Actors.Free();
}

inline void RefreshPickupCache(float Now)
{
	if (Now < BotWorld::NextPickupRefresh)
		return;

	BotWorld::NextPickupRefresh = Now + 1.5f;
	BotWorld::Pickups.clear();

	auto Class = AFortPickup::StaticClass();

	if (!Class)
		return;

	static int bPickedUpOffset = -2;

	auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), Class);

	for (int i = 0; i < Actors.Num(); ++i)
	{
		auto Pickup = (AFortPickup*)Actors.at(i);

		if (!IsActorUsable(Pickup))
			continue;

		if (bPickedUpOffset == -2)
			bPickedUpOffset = Pickup->GetOffset("bPickedUp", false);

		if (bPickedUpOffset != -1 && Pickup->Get<bool>(bPickedUpOffset))
			continue;

		auto Entry = Pickup->GetPrimaryPickupItemEntry();

		if (!Entry)
			continue;

		auto Def = Entry->GetItemDefinition();

		if (!Def)
			continue;

		// Only remember things bots actually want: weapons and heals
		if (ClassifyWeapon(Def) == EBotWeaponKind::None && ClassifyHeal(Def) == EBotHealKind::None)
			continue;

		FBotCachedPickup Cached;
		Cached.Ref.Set(Pickup);
		Cached.Location = Pickup->GetActorLocation();
		Cached.Definition = Def;
		BotWorld::Pickups.push_back(Cached);
	}

	Actors.Free();
}

inline void BuildChestCache()
{
	BotWorld::Chests.clear();
	BotWorld::bBuiltChestCache = true;

	auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABuildingContainer::StaticClass());

	for (int i = 0; i < Actors.Num(); ++i)
	{
		auto Container = (ABuildingContainer*)Actors.at(i);

		if (!IsActorUsable(Container) || Container->IsAlreadySearched())
			continue;

		auto Tier = Container->GetSearchLootTierGroup().ToString();

		if (!ContainsI(Tier, "Treasure") && !ContainsI(Tier, "Ammo") && !ContainsI(Tier, "Chest"))
			continue;

		FBotCachedContainer Cached;
		Cached.Ref.Set(Container);
		Cached.Location = Container->GetActorLocation();
		BotWorld::Chests.push_back(Cached);
	}

	Actors.Free();

	LOG_INFO(LogBots, "Bots know about {} chests/ammo boxes.", BotWorld::Chests.size());
}

inline AActor* GetAircraft()
{
	auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());

	if (!GameState)
		return nullptr;

	static auto AircraftsOffset = GameState->GetOffset("Aircrafts", false);

	if (AircraftsOffset == -1)
	{
		static auto AircraftOffset = GameState->GetOffset("Aircraft", false);
		return AircraftOffset == -1 ? nullptr : GameState->Get<AActor*>(AircraftOffset);
	}

	auto Aircrafts = GameState->GetPtr<TArray<AActor*>>(AircraftsOffset);
	return Aircrafts->Num() > 0 ? Aircrafts->at(0) : nullptr;
}

struct FBotSafeZone
{
	bool bValid = false;
	FVector Center;      // current circle
	float Radius = 0.f;
	FVector NextCenter;  // the circle it's shrinking to
	float NextRadius = 0.f;
	int Phase = 0;
};

inline FBotSafeZone GetSafeZone()
{
	FBotSafeZone Zone;

	auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());

	if (!GameState)
		return Zone;

	static auto SafeZoneIndicatorOffset = GameState->GetOffset("SafeZoneIndicator", false);

	if (SafeZoneIndicatorOffset == -1)
		return Zone;

	auto Indicator = GameState->Get<AFortSafeZoneIndicator*>(SafeZoneIndicatorOffset);

	if (!IsActorUsable(Indicator))
		return Zone;

	static auto LastCenterOffset = Indicator->GetOffset("LastCenter", false);
	static auto NextCenterOffset = Indicator->GetOffset("NextCenter", false);
	static auto LastRadiusOffset = Indicator->GetOffset("LastRadius", false);
	static auto NextRadiusOffset = Indicator->GetOffset("NextRadius", false);

	if (NextCenterOffset == -1 || NextRadiusOffset == -1)
		return Zone;

	Zone.NextCenter = Indicator->Get<FVector>(NextCenterOffset);
	Zone.NextRadius = Indicator->Get<float>(NextRadiusOffset);

	FVector LastCenter = LastCenterOffset != -1 ? Indicator->Get<FVector>(LastCenterOffset) : Zone.NextCenter;
	float LastRadius = LastRadiusOffset != -1 ? Indicator->Get<float>(LastRadiusOffset) : Zone.NextRadius;

	float Now = GameState->GetServerWorldTimeSeconds();
	float Start = Indicator->GetSafeZoneStartShrinkTime();
	float Finish = Indicator->GetSafeZoneFinishShrinkTime();

	float Alpha = 0.f;

	if (Finish > Start && Now > Start)
		Alpha = BotMath::Clamp((Now - Start) / (Finish - Start), 0.f, 1.f);

	Zone.Center = BotMath::Add(LastCenter, BotMath::Mul(BotMath::Sub(Zone.NextCenter, LastCenter), Alpha));
	Zone.Radius = BotMath::Lerp(LastRadius, Zone.NextRadius, Alpha);

	static auto SafeZonePhaseOffset = GameState->GetOffset("SafeZonePhase", false);
	Zone.Phase = SafeZonePhaseOffset != -1 ? (int)GameState->Get<uint8>(SafeZonePhaseOffset) : 0;

	Zone.bValid = Zone.Radius > 1.f && Zone.NextRadius > 1.f;
	return Zone;
}

// --------------------------------------------------------------------------------------------
//  Debug dump: written once so we can see the real property/parameter names on this version
// --------------------------------------------------------------------------------------------

inline void BotDumpStruct(std::ofstream& Out, UObject* Struct, const std::string& Title)
{
	Out << "== " << Title << " ==\n";

	if (!Struct)
	{
		Out << "  (not found)\n";
		return;
	}

	for (auto Current = Struct; Current; Current = *(UObject**)(__int64(Current) + Offsets::SuperStruct))
	{
		Out << " [" << Current->GetName() << "]\n";
		void* Property = *(void**)(__int64(Current) + Offsets::Children);

		while (Property)
		{
			Out << "   " << GetFNameOfProp(Property)->ToString() << " @ 0x" << std::hex << *(int*)(__int64(Property) + Offsets::Offset_Internal) << std::dec << "\n";
			Property = GetNext(Property);
		}

		if (Current->GetName() == "Actor" || Current->GetName() == "Object")
			break;
	}
}

inline void BotWriteDebugDump(UObject* Pawn, UObject* Controller, UObject* PlayerState)
{
	static bool bDumped = false;

	if (bDumped)
		return;

	bDumped = true;

	std::ofstream Out("bot_debug.txt");

	if (!Out.is_open())
		return;

	Out << "Reboot bot debug dump\n\n";

	if (Pawn) BotDumpStruct(Out, Pawn->ClassPrivate, "Bot pawn: " + Pawn->ClassPrivate->GetName());
	if (Controller) BotDumpStruct(Out, Controller->ClassPrivate, "Bot controller: " + Controller->ClassPrivate->GetName());
	if (PlayerState) BotDumpStruct(Out, PlayerState->ClassPrivate, "Bot playerstate: " + PlayerState->ClassPrivate->GetName());

	BotDumpStruct(Out, FindObject(L"/Script/FortniteGame.FortSafeZoneIndicator"), "FortSafeZoneIndicator");

	const wchar_t* Functions[] = {
		L"/Script/FortniteGame.FortAthenaAIBotController.PlayEmote",
		L"/Script/FortniteGame.FortPawn.ForceKill",
		L"/Script/FortniteGame.FortPawn.PawnStartFire",
		L"/Script/FortniteGame.FortPlayerPawn.BeginSkydiving",
		L"/Script/FortniteGame.FortPlayerPawn.BP_ForceOpenParachute",
		L"/Script/FortniteGame.FortPlayerPawnAthena.TeleportToSkyDive",
		L"/Script/FortniteGame.FortPlayerControllerAthena.ClientNotifyWon",
		L"/Script/FortniteGame.FortPlayerControllerAthena.PlayWinEffects",
		L"/Script/Engine.Pawn.AddMovementInput",
		L"/Script/AIModule.BrainComponent.StopLogic",
	};

	Out << "\n== Function parameters ==\n";

	for (auto Path : Functions)
	{
		auto Fn = FindObject<UFunction>(Path);
		std::wstring WPath = Path;
		Out << std::string(WPath.begin(), WPath.end()) << ": ";

		if (!Fn) { Out << "(not found)\n"; continue; }

		for (auto& P : BotListFunctionParams(Fn)) Out << P << " ";
		Out << "\n";
	}

	LOG_INFO(LogBots, "Wrote bot_debug.txt");
}

// --------------------------------------------------------------------------------------------
//  The bot
// --------------------------------------------------------------------------------------------

enum class EBotState : uint8
{
	Warmup,
	Bus,
	Skydive,
	Glide,
	Ground,
	WaitingToRespawn,
	Dead
};

enum class EBotGoal : uint8
{
	None,
	Wander,
	Pickup,
	Chest,
	Storm,
	Fight
};

class PlayerBot
{
public:
	static inline UClass* PawnClass = nullptr;
	static inline UClass* ControllerClass = nullptr;

	// Kept for compatibility with the rest of Reboot (commands.cpp etc.)
	AController* Controller = nullptr;
	AFortPlayerPawnAthena* Pawn = nullptr;
	AFortPlayerStateAthena* PlayerState = nullptr;
	bool bIsAthenaController = false;

	FBotObjectRef ControllerRef, PawnRef, PlayerStateRef;

	std::string Name;
	EBotState State = EBotState::Warmup;
	EBotGoal Goal = EBotGoal::None;

	// Inventory (AI controllers can't use Reboot's player inventory code, so bots keep their own)
	std::vector<FBotItem> Items;
	int EquippedIndex = -1;
	FBotObjectRef EquippedWeaponRef;

	// Movement
	FVector MoveTarget;
	float GoalExpireTime = 0.f;
	FVector LastKnownLocation;
	FVector LastStuckCheckLocation;
	float NextStuckCheckTime = 0.f;
	int StuckCount = 0;
	float DetourUntil = 0.f;
	float DetourYaw = 0.f;
	float NextJumpTime = 0.f;
	float CurrentYaw = 0.f;
	FVector HomeLocation;

	// Warmup
	float NextWarmupDecision = 0.f;
	bool bWarmupWalking = false;
	float EmoteUntil = 0.f;

	// Bus / landing
	FVector LandingTarget;
	bool bHasLandingTarget = false;
	float MinBusDistance = 1e12f;
	float BusJumpDeadline = 0.f;
	float NextAltitudeCheck = 0.f;
	float LastAltitude = 100000.f;
	bool bOpenedParachute = false;
	bool bThankedDriver = false;
	float ThankDriverTime = 0.f;

	// Looting
	FBotObjectRef TargetPickupRef;
	FBotObjectRef TargetChestRef;
	float NextLootScan = 0.f;
	std::unordered_map<UObject*, float> IgnoreUntil; // pickups/chests we gave up on

	// Combat
	FBotObjectRef TargetPawnRef;
	FVector LastSeenTargetLocation;
	float LastSeenTargetTime = -100.f;
	float NextTargetScan = 0.f;
	float ReactionReadyTime = 0.f;
	bool bFiring = false;
	float FireStateUntil = 0.f;
	float NextSimulatedShot = 0.f;
	float ReloadUntil = 0.f;
	int SimulatedMag = 0;
	float StrafeYawOffset = 90.f;
	float NextStrafeSwitch = 0.f;
	int AmmoAtFireStart = -1;
	float FireStartTime = 0.f;
	float NextStormTick = 0.f;

	// Healing
	float HealUntil = 0.f;
	int HealingItemIndex = -1;

	// Damage bookkeeping (for kill credit)
	FBotObjectRef LastDamagerControllerRef;
	FBotObjectRef LastDamagerPlayerStateRef;
	float LastDamagedTime = -100.f;
	int Kills = 0;

	float RespawnTime = 0.f;
	bool bBroken = false;

	// Diagnostics/keep-awake
	float NextKeepAwake = 0.f;
	float DiagnosticTime = -1.f;
	FVector DiagnosticLocation;
	int DiagnosticsLogged = 0;

	// ---- setup -------------------------------------------------------------------------------

	static bool ShouldUseAIBotController() { return true; }

	static void InitializeBotClasses()
	{
		static auto BlueprintGeneratedClassClass = FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass");

		PawnClass = FindObject<UClass>(L"/Game/Athena/AI/Phoebe/BP_PlayerPawn_Athena_Phoebe.BP_PlayerPawn_Athena_Phoebe_C");

		if (!PawnClass)
			PawnClass = LoadObject<UClass>(L"/Game/Athena/AI/Phoebe/BP_PlayerPawn_Athena_Phoebe.BP_PlayerPawn_Athena_Phoebe_C", BlueprintGeneratedClassClass);

		ControllerClass = FindObject<UClass>(L"/Game/Athena/AI/Phoebe/BP_PhoebePlayerController.BP_PhoebePlayerController_C");

		if (!ControllerClass)
			ControllerClass = LoadObject<UClass>(L"/Game/Athena/AI/Phoebe/BP_PhoebePlayerController.BP_PhoebePlayerController_C", BlueprintGeneratedClassClass);

		if (!ControllerClass)
			ControllerClass = AFortAthenaAIBotController::StaticClass();

		LOG_INFO(LogBots, "Bot classes: pawn {} controller {}", PawnClass ? PawnClass->GetName() : "MISSING", ControllerClass ? ControllerClass->GetName() : "MISSING");

		if (!PawnClass || !ControllerClass)
			LOG_ERROR(LogBots, "This Fortnite version doesn't have the AI bot classes, bots won't spawn!");
	}

	static bool IsReadyToSpawnBot()
	{
		if (!PawnClass || !ControllerClass)
			InitializeBotClasses();

		return PawnClass && ControllerClass;
	}

	static FString GetRandomName()
	{
		static int CurrentBotNum = 1;

		if (PlayerBotNames.empty())
		{
			std::wstring Generated = L"Bot" + std::to_wstring(CurrentBotNum++);
			FString NewName = Generated.c_str();
			return NewName;
		}

		FString NewName = PlayerBotNames.back();
		PlayerBotNames.pop_back();
		return NewName;
	}

	// Spawn through Fortnite's own bot spawner. This sets up the pawn, its Phoebe controller,
	// playerstate and possession the way the game expects (what OGS does).
	bool SpawnWithBotMutator(const FTransform& SpawnTransform, AActor* SpawnLocator)
	{
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		if (!GameMode || !SpawnLocator)
			return false;

		auto BotManager = GameMode->GetServerBotManager();
		auto Mutator = BotManager ? BotManager->GetCachedBotMutator() : nullptr;

		if (!Mutator)
		{
			static bool bWarned = false;
			if (!bWarned) { bWarned = true; LOG_WARN(LogBots, "No bot mutator, using manual bot spawning."); }
			return false;
		}

		FRotator Rotation = SpawnTransform.Rotation.Rotator();
		auto NewPawn = (AFortPlayerPawnAthena*)Mutator->SpawnBot(PawnClass, SpawnLocator, SpawnTransform.Translation, Rotation, false);

		if (!NewPawn || !NewPawn->GetController())
		{
			LOG_WARN(LogBots, "Bot mutator SpawnBot failed (pawn {}), using manual bot spawning.", NewPawn != nullptr);
			if (NewPawn) NewPawn->K2_DestroyActor();
			return false;
		}

		Pawn = NewPawn;
		Controller = (AController*)NewPawn->GetController();
		PawnRef.Set(Pawn);
		ControllerRef.Set(Controller);

		if (!BotSettings::bUseEpicAI)
			BotEngine::StopEpicBrain(Controller);

		static bool bLogged = false;
		if (!bLogged) { bLogged = true; LOG_INFO(LogBots, "Spawning bots through Fortnite's bot mutator."); }
		return true;
	}

	bool SpawnPawnAndController(const FTransform& SpawnTransform)
	{
		auto World = GetWorld();

		auto NewPawn = World->SpawnActor<AFortPlayerPawnAthena>(PawnClass, SpawnTransform, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn));

		if (!NewPawn)
		{
			LOG_ERROR(LogBots, "Failed to spawn bot pawn!");
			return false;
		}

		AController* NewController = nullptr;

		if (Controller && ControllerRef.IsValid())
		{
			// Respawn/new body: reuse our controller (and with it our playerstate/name/team)
			NewController = Controller;
			auto AutoController = (AController*)NewPawn->GetController();

			if (AutoController && AutoController != NewController)
				AutoController->K2_DestroyActor();

			NewController->Possess(NewPawn);
		}
		else
		{
			NewController = (AController*)NewPawn->GetController(); // AutoPossessAI may already have made one

			if (!NewController)
				NewController = (AController*)BotEngine::SpawnDefaultController(NewPawn);

			if (!NewController)
			{
				NewController = World->SpawnActor<AController>(ControllerClass, SpawnTransform, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AlwaysSpawn));

				if (NewController)
					NewController->Possess(NewPawn);
			}
		}

		if (!NewController)
		{
			LOG_ERROR(LogBots, "Failed to get a controller for the bot!");
			NewPawn->K2_DestroyActor();
			return false;
		}

		Controller = NewController;
		Pawn = NewPawn;
		ControllerRef.Set(Controller);
		PawnRef.Set(Pawn);

		if (!BotSettings::bUseEpicAI)
			BotEngine::StopEpicBrain(Controller);

		return true;
	}

	void ApplyHeroToCharacterData(UObject* HeroType)
	{
		static auto CharacterDataOffset = PlayerState->GetOffset("CharacterData", false);
		static auto PartsOffset = FindOffsetStruct("/Script/FortniteGame.CustomCharacterData", "Parts", false);
		static auto SpecializationsOffset = HeroType->GetOffset("Specializations", false);
		static auto FortHeroSpecializationClass = FindObject<UClass>(L"/Script/FortniteGame.FortHeroSpecialization");
		static auto CustomCharacterPartClass = FindObject<UClass>(L"/Script/FortniteGame.CustomCharacterPart");

		if (CharacterDataOffset == -1 || PartsOffset == -1 || SpecializationsOffset == -1)
			return;

		auto Parts = (UObject**)(__int64(PlayerState) + CharacterDataOffset + PartsOffset);
		auto& Specializations = HeroType->Get<TArray<TSoftObjectPtr<UObject>>>(SpecializationsOffset);

		for (int i = 0; i < Specializations.Num(); i++)
		{
			auto Specialization = Specializations.at(i).Get(FortHeroSpecializationClass, true);
			if (!Specialization) continue;

			static auto CharacterPartsOffset = Specialization->GetOffset("CharacterParts");
			auto& CharacterParts = Specialization->Get<TArray<TSoftObjectPtr<UObject>>>(CharacterPartsOffset);

			for (int z = 0; z < CharacterParts.Num(); z++)
			{
				auto Part = CharacterParts.at(z, GetSoftObjectSize()).Get(CustomCharacterPartClass, true);
				if (!Part) continue;

				static auto PartTypeOffset = Part->GetOffset("CharacterPartType", false);
				if (PartTypeOffset == -1) continue;

				uint8 Type = Part->Get<uint8>(PartTypeOffset);
				if (Type < 7) Parts[Type] = Part; // EFortCustomPartType::NumTypes == 7
			}
		}

		static auto OnRep_CharacterDataFn = BotFindFn(L"/Script/FortniteGame.FortPlayerState.OnRep_CharacterData");
		if (OnRep_CharacterDataFn) PlayerState->ProcessEvent(OnRep_CharacterDataFn);
	}

	void ApplyCosmetics()
	{
		static auto HeroTypeOffset = PlayerState->GetOffset("HeroType", false);

		if (HeroTypeOffset == -1)
			return;

		auto HeroType = PlayerState->Get(HeroTypeOffset);

		if (!HeroType)
		{
			static std::vector<UObject*> AthenaHeroTypes;

			if (AthenaHeroTypes.empty())
			{
				auto AllHeroTypes = GetAllObjectsOfClass(FindObject<UClass>(L"/Script/FortniteGame.FortHeroType"));

				for (auto HT : AllHeroTypes)
				{
					if (HT && HT->GetPathName().starts_with("/Game/Athena/Heroes/"))
						AthenaHeroTypes.push_back(HT);
				}
			}

			HeroType = AthenaHeroTypes.empty()
				? FindObject(L"/Game/Athena/Heroes/HID_030_Athena_Commando_M_Halloween.HID_030_Athena_Commando_M_Halloween")
				: AthenaHeroTypes.at(BotMath::RandI(0, (int)AthenaHeroTypes.size() - 1));

			PlayerState->Get(HeroTypeOffset) = HeroType;
		}

		if (HeroType)
		{
			ApplyHID(Pawn, HeroType, false);
			ApplyHeroToCharacterData(HeroType);
		}
	}

	void SetupNewPawn()
	{
		Pawn->SetMaxHealth(100);
		Pawn->SetHealth(100);
		Pawn->SetMaxShield(100);
		Pawn->SetShield(0);

		ApplyCosmetics();

		EquippedIndex = -1;
		EquippedWeaponRef.Reset();
		EquipBestWeapon(true);

		CurrentYaw = Pawn->GetActorRotation().Yaw;
		LastStuckCheckLocation = Pawn->GetActorLocation();
		LastKnownLocation = LastStuckCheckLocation;

		EnsurePossessed(true);
		BotEngine::KeepAwakeAndMoving(Controller, Pawn, true);
		DiagnosticTime = -1.f;
	}

	// Make sure our controller really drives our pawn, and that the server will simulate it.
	void EnsurePossessed(bool bLog)
	{
		if (!Controller || !Pawn)
			return;

		BotEngine::MakeControllerLocal(Controller);
		BotEngine::LetMovementRunWithoutController(Pawn);

		auto Current = (AController*)Pawn->GetController();

		if (Current != Controller)
		{
			static int Logged = 0;

			if (bLog || Logged < 5)
			{
				Logged++;
				LOG_INFO(LogBots, "{}: pawn was controlled by {}, re-possessing with our controller.", Name, Current ? Current->ClassPrivate->GetName() : "nothing");
			}

			if (Current && Current != Controller)
				Current->UnPossess();

			Controller->Possess(Pawn);
		}
	}

	bool Initialize(const FTransform& SpawnTransform, AActor* SpawnLocator = nullptr)
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		if (!GameState || !GameMode || !IsReadyToSpawnBot())
		{
			LOG_ERROR(LogBots, "We are not prepared to spawn a bot!");
			return false;
		}

		int PlayersLeftBefore = GameState->GetPlayersLeft();

		if (!SpawnWithBotMutator(SpawnTransform, SpawnLocator) && !SpawnPawnAndController(SpawnTransform))
			return false;

		PlayerState = Cast<AFortPlayerStateAthena>(Controller->GetPlayerState());

		if (!PlayerState)
		{
			LOG_ERROR(LogBots, "Bot controller {} has no playerstate! Destroying it.", Controller->ClassPrivate->GetName());
			Pawn->K2_DestroyActor();
			Controller->K2_DestroyActor();
			Controller = nullptr;
			Pawn = nullptr;
			return false;
		}

		PlayerStateRef.Set(PlayerState);
		PlayerState->SetIsBot(true);

		// The weapon code may look for an inventory on the controller, so make sure there is one
		static auto InventoryOffset = Controller->GetOffset("Inventory", false);

		if (InventoryOffset != -1 && !Controller->Get<AFortInventory*>(InventoryOffset))
		{
			static auto FortInventoryClass = FindObject<UClass>(L"/Script/FortniteGame.FortInventory");
			auto Inventory = GetWorld()->SpawnActor<AFortInventory>(FortInventoryClass, FTransform{}, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AlwaysSpawn, false, Controller));

			if (Inventory)
			{
				Inventory->GetInventoryType() = EFortInventoryType::World;
				Controller->Get<AFortInventory*>(InventoryOffset) = Inventory;
			}
		}

		FString BotName = GetRandomName();
		Name = BotName.ToString();
		GameMode->ChangeName(Controller, BotName, true);
		PlayerState->OnRep_PlayerName();

		PlayerState->GetTeamIndex() = GameMode->Athena_PickTeamHook(GameMode, 0, Controller);

		static auto SquadIdOffset = PlayerState->GetOffset("SquadId", false);

		if (SquadIdOffset != -1)
			PlayerState->GetSquadId() = PlayerState->GetTeamIndex() - NumToSubtractFromSquadId;

		GameState->AddPlayerStateToGameMemberInfo(PlayerState);

		auto PlayerAbilitySet = GetPlayerAbilitySet();
		auto AbilitySystemComponent = PlayerState->GetAbilitySystemComponent();

		if (PlayerAbilitySet && AbilitySystemComponent)
			PlayerAbilitySet->GiveToAbilitySystem(AbilitySystemComponent);

		GiveItem(FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01"), 1, 0);

		SetupNewPawn();

		// Count the bot as a player still in the game (unless the game already did it for us)
		if (GameState->GetPlayersLeft() == PlayersLeftBefore)
		{
			GameState->GetPlayersLeft()++;
			GameState->OnRep_PlayersLeft();
		}

		HomeLocation = Pawn->GetActorLocation();
		State = GameState->GetGamePhase() <= EAthenaGamePhase::Warmup ? EBotState::Warmup : EBotState::Ground;

		BotWriteDebugDump(Pawn, Controller, PlayerState);

		LOG_INFO(LogBots, "Spawned bot {} (team {}, controller {}, pawn {})", Name, (int)PlayerState->GetTeamIndex(), Controller->ClassPrivate->GetName(), Pawn->ClassPrivate->GetName());
		return true;
	}

	// ---- inventory -----------------------------------------------------------------------------

	void SpawnItemPickup(const FBotItem& Item, const FVector& Location, bool bFromElimination)
	{
		if (!Item.Definition)
			return;

		PickupCreateData CreateData;
		CreateData.ItemEntry = FFortItemEntry::MakeItemEntry(Item.Definition, Item.Count, Item.LoadedAmmo);
		CreateData.SpawnLocation = Location;
		CreateData.bToss = true;
		CreateData.bRandomRotation = true;
		CreateData.SourceType = EFortPickupSourceTypeFlag::GetPlayerValue();
		CreateData.Source = bFromElimination ? EFortPickupSpawnSource::GetPlayerEliminationValue() : 0;
		CreateData.bShouldFreeItemEntryWhenDeconstructed = true;

		AFortPickup::SpawnPickup(CreateData);
	}

	void DropItem(int Index)
	{
		if (Index < 0 || Index >= (int)Items.size())
			return;

		auto& Item = Items[Index];

		if (Item.WeaponKind != EBotWeaponKind::Pickaxe && IsActorUsable(Pawn))
			SpawnItemPickup(Item, Pawn->GetActorLocation(), false);

		if (Index == EquippedIndex)
		{
			EquippedIndex = -1;
			EquippedWeaponRef.Reset();
		}
		else if (Index < EquippedIndex)
		{
			EquippedIndex--;
		}

		Items.erase(Items.begin() + Index);
	}

	bool GiveItem(UFortItemDefinition* Def, int Count, int LoadedAmmo)
	{
		if (!Def)
			return false;

		auto WeaponKind = ClassifyWeapon(Def);
		auto HealKind = ClassifyHeal(Def);

		if (WeaponKind == EBotWeaponKind::None && HealKind == EBotHealKind::None)
			return false;

		if (HealKind != EBotHealKind::None)
		{
			for (auto& Item : Items)
			{
				if (Item.Definition == Def)
				{
					Item.Count += Count > 0 ? Count : 1;
					return true;
				}
			}
		}
		else if (WeaponKind != EBotWeaponKind::Pickaxe)
		{
			// Max 3 guns: replace the worst one if this is better
			int GunCount = 0, WorstIndex = -1, WorstScore = 1 << 30;

			for (int i = 0; i < (int)Items.size(); ++i)
			{
				if (Items[i].WeaponKind == EBotWeaponKind::None || Items[i].WeaponKind == EBotWeaponKind::Pickaxe)
					continue;

				GunCount++;

				if (Items[i].Score < WorstScore)
				{
					WorstScore = Items[i].Score;
					WorstIndex = i;
				}
			}

			if (GunCount >= 3)
			{
				if (WeaponScore(Def) <= WorstScore)
					return false;

				DropItem(WorstIndex);
			}
		}

		FBotItem Item;
		Item.Definition = Def;
		Item.Count = Count > 0 ? Count : 1;
		Item.WeaponKind = WeaponKind;
		Item.HealKind = HealKind;
		Item.Score = WeaponScore(Def);
		Item.Guid = MakeBotGuid();

		if (WeaponKind != EBotWeaponKind::None && WeaponKind != EBotWeaponKind::Pickaxe)
		{
			auto WeaponDef = Cast<UFortWeaponItemDefinition>(Def);
			int Clip = WeaponDef ? WeaponDef->GetClipSize() : 30;
			Item.LoadedAmmo = LoadedAmmo > 0 ? LoadedAmmo : (Clip > 0 ? Clip : 30);
		}

		Items.push_back(Item);
		return true;
	}

	bool WantsItem(UFortItemDefinition* Def)
	{
		auto HealKind = ClassifyHeal(Def);

		if (HealKind != EBotHealKind::None)
		{
			int TotalHeals = 0;
			for (auto& Item : Items) if (Item.HealKind != EBotHealKind::None) TotalHeals += Item.Count;
			return TotalHeals < 8;
		}

		auto Kind = ClassifyWeapon(Def);

		if (Kind == EBotWeaponKind::None || Kind == EBotWeaponKind::Pickaxe)
			return false;

		int GunCount = 0, WorstScore = 1 << 30;

		for (auto& Item : Items)
		{
			if (Item.Definition == Def)
				return false; // already have this exact gun

			if (Item.WeaponKind == EBotWeaponKind::None || Item.WeaponKind == EBotWeaponKind::Pickaxe)
				continue;

			GunCount++;
			WorstScore = (std::min)(WorstScore, Item.Score);
		}

		return GunCount < 3 || WeaponScore(Def) > WorstScore;
	}

	int BestWeaponIndex(float DistanceToTarget = -1.f)
	{
		int Best = -1, BestScore = -1000;

		for (int i = 0; i < (int)Items.size(); ++i)
		{
			auto& Item = Items[i];

			if (Item.WeaponKind == EBotWeaponKind::None)
				continue;

			int Score = Item.WeaponKind == EBotWeaponKind::Pickaxe ? 1 : Item.Score;

			// Use the right tool for the range
			if (DistanceToTarget >= 0.f && Item.WeaponKind != EBotWeaponKind::Pickaxe)
			{
				auto Stats = GetWeaponStats(Item.WeaponKind);
				if (DistanceToTarget > Stats.Range) Score -= 40;
				if (Item.WeaponKind == EBotWeaponKind::Shotgun && DistanceToTarget < 700.f) Score += 25;
				if (Item.WeaponKind == EBotWeaponKind::Sniper && DistanceToTarget < 1500.f) Score -= 30;
			}

			if (Score > BestScore)
			{
				BestScore = Score;
				Best = i;
			}
		}

		return Best;
	}

	bool HasGun()
	{
		for (auto& Item : Items)
			if (Item.WeaponKind != EBotWeaponKind::None && Item.WeaponKind != EBotWeaponKind::Pickaxe)
				return true;

		return false;
	}

	EBotWeaponKind EquippedKind()
	{
		if (EquippedIndex < 0 || EquippedIndex >= (int)Items.size())
			return EBotWeaponKind::None;

		return Items[EquippedIndex].WeaponKind;
	}

	AFortWeapon* GetEquippedWeapon()
	{
		auto Weapon = EquippedWeaponRef.Get<AFortWeapon>();
		return IsActorUsable(Weapon) ? Weapon : nullptr;
	}

	void StopFiring()
	{
		if (bFiring && IsActorUsable(Pawn))
			BotEngine::StopFire(Pawn);

		bFiring = false;
	}

	void EquipIndex(int Index)
	{
		if (Index < 0 || Index >= (int)Items.size() || !IsActorUsable(Pawn))
			return;

		if (Index == EquippedIndex && GetEquippedWeapon())
			return;

		auto WeaponDef = Cast<UFortWeaponItemDefinition>(Items[Index].Definition);

		if (!WeaponDef)
			return;

		StopFiring();

		// Remember how much ammo the old gun had
		if (EquippedIndex >= 0 && EquippedIndex < (int)Items.size())
		{
			if (auto OldWeapon = GetEquippedWeapon())
				Items[EquippedIndex].LoadedAmmo = OldWeapon->GetAmmoCount();
		}

		auto Weapon = Pawn->EquipWeaponDefinition(WeaponDef, Items[Index].Guid);

		EquippedIndex = Index;
		EquippedWeaponRef.Set(Weapon);

		if (Weapon && Items[Index].WeaponKind != EBotWeaponKind::Pickaxe)
		{
			Weapon->GetAmmoCount() = Items[Index].LoadedAmmo;
			SimulatedMag = Items[Index].LoadedAmmo;
		}
	}

	void EquipBestWeapon(bool bForce = false, float DistanceToTarget = -1.f)
	{
		int Best = BestWeaponIndex(DistanceToTarget);

		if (Best == -1)
			return;

		if (bForce || Best != EquippedIndex || !GetEquippedWeapon())
			EquipIndex(Best);
	}

	// ---- helpers ---------------------------------------------------------------------------------

	float Skill01() const { return BotMath::Clamp(BotSettings::Skill / 100.f, 0.f, 1.f); }

	FVector Location()
	{
		LastKnownLocation = Pawn->GetActorLocation();
		return LastKnownLocation;
	}

	void FaceYaw(float TargetYaw, float Pitch, float DeltaTime, float TurnSpeed)
	{
		float Delta = BotMath::NormalizeAngle(TargetYaw - CurrentYaw);
		float MaxStep = TurnSpeed * DeltaTime;
		Delta = BotMath::Clamp(Delta, -MaxStep, MaxStep);
		CurrentYaw = BotMath::NormalizeAngle(CurrentYaw + Delta);

		FRotator Rotation;
		Rotation.Pitch = Pitch;
		Rotation.Yaw = CurrentYaw;
		Rotation.Roll = 0;
		BotEngine::SetControlRotation(Controller, Rotation);
	}

	// Walk towards a point, dealing with getting stuck. Returns distance left (2D).
	float MoveTowards(const FVector& Target, float Now, float DeltaTime)
	{
		auto MyLocation = Location();
		float Distance = BotMath::Dist2D(MyLocation, Target);

		FVector Direction = Now < DetourUntil
			? BotMath::DirFromYaw(DetourYaw)
			: BotMath::Normal2D(BotMath::Sub(Target, MyLocation));

		BotEngine::AddMovementInput(Pawn, Direction, 1.f);
		FaceYaw(BotMath::YawOf(Direction), 0.f, DeltaTime, 540.f);

		// Stuck detection
		if (Now >= NextStuckCheckTime)
		{
			NextStuckCheckTime = Now + 1.0f;

			float Moved = BotMath::Dist2D(MyLocation, LastStuckCheckLocation);
			LastStuckCheckLocation = MyLocation;

			if (Moved < 60.f && Distance > 150.f)
			{
				StuckCount++;
				BotEngine::Jump(Pawn);

				if (StuckCount >= 2)
				{
					DetourYaw = BotMath::NormalizeAngle(BotMath::YawOf(Direction) + (BotMath::Chance(0.5f) ? 90.f : -90.f) + BotMath::RandF(-30.f, 30.f));
					DetourUntil = Now + BotMath::RandF(0.8f, 1.8f);
				}
			}
			else
			{
				StuckCount = 0;
			}
		}

		return Distance;
	}

	bool IsStuckForGood() const { return StuckCount >= 6; }

	void IgnoreObject(UObject* Object, float Now, float Seconds)
	{
		if (Object)
			IgnoreUntil[Object] = Now + Seconds;
	}

	bool IsIgnored(UObject* Object, float Now)
	{
		auto It = IgnoreUntil.find(Object);
		return It != IgnoreUntil.end() && It->second > Now;
	}

	// ---- damage --------------------------------------------------------------------------------

	// Used for storm damage and for simulated shots. Returns true if this killed the pawn.
	static bool ApplyDamageToPawn(AFortPawn* Victim, float Damage, AController* InstigatorController, AActor* InstigatorActor)
	{
		if (!IsActorUsable(Victim) || Damage <= 0.f)
			return false;

		float Shield = Victim->GetShield();
		float Health = Victim->GetHealth();

		if (Health <= 0.f)
			return false;

		float FromShield = (std::min)(Shield, Damage);
		Shield -= FromShield;
		Health -= (Damage - FromShield);

		Victim->SetShield(Shield);

		if (Health <= 0.f)
		{
			Victim->SetHealth(1.f);
			BotEngine::ForceKill(Victim, InstigatorController, InstigatorActor);
			return true;
		}

		Victim->SetHealth(Health);
		return false;
	}

	void NoteDamagedBy(AController* InstigatorController, float Now)
	{
		if (!InstigatorController)
			return;

		LastDamagerControllerRef.Set(InstigatorController);
		LastDamagerPlayerStateRef.Set(InstigatorController->GetPlayerState());
		LastDamagedTime = Now;
	}
};

inline std::vector<PlayerBot*> AllPlayerBotsToTick;

inline PlayerBot* FindBotByPawn(UObject* Pawn)
{
	if (!Pawn)
		return nullptr;

	for (auto Bot : AllPlayerBotsToTick)
		if (Bot && Bot->Pawn == Pawn && Bot->State != EBotState::Dead)
			return Bot;

	return nullptr;
}

namespace Bots
{
	inline AController* SpawnBot(FTransform SpawnTransform, AActor* InSpawnLocator = nullptr)
	{
		auto Bot = new PlayerBot();

		if (!Bot->Initialize(SpawnTransform, InSpawnLocator))
		{
			delete Bot;
			return nullptr;
		}

		AllPlayerBotsToTick.push_back(Bot);
		BotSettings::AliveBots++;
		return Bot->Controller;
	}

	inline std::vector<AActor*> GetWarmupStarts()
	{
		std::vector<AActor*> Result;

		static auto FortPlayerStartCreativeClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerStartCreative");
		static auto FortPlayerStartWarmupClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerStartWarmup");

		TArray<AActor*> PlayerStarts = UGameplayStatics::GetAllActorsOfClass(GetWorld(), Globals::bCreative ? FortPlayerStartCreativeClass : FortPlayerStartWarmupClass);

		for (int i = 0; i < PlayerStarts.Num(); ++i)
			if (PlayerStarts.at(i))
				Result.push_back(PlayerStarts.at(i));

		PlayerStarts.Free();
		return Result;
	}

	inline void SpawnBotsAtPlayerStarts(int AmountOfBots)
	{
		if (AmountOfBots <= 0)
			return;

		auto Starts = GetWarmupStarts();

		if (Starts.empty())
		{
			LOG_ERROR(LogBots, "No player starts to spawn bots at!");
			return;
		}

		int Spawned = 0;

		for (int i = 0; i < AmountOfBots; ++i)
		{
			auto Start = Starts.at(BotMath::RandI(0, (int)Starts.size() - 1));
			FTransform Transform = Start->GetTransform();

			// Spread them out a little so they don't spawn inside each other
			Transform.Translation.X += BotMath::RandF(-250.f, 250.f);
			Transform.Translation.Y += BotMath::RandF(-250.f, 250.f);
			Transform.Translation.Z += 60.f;

			if (SpawnBot(Transform, Start))
				Spawned++;
		}

		LOG_INFO(LogBots, "Spawned {}/{} bots.", Spawned, AmountOfBots);
	}

	inline void RemoveAllBots()
	{
		for (auto Bot : AllPlayerBotsToTick)
		{
			if (!Bot) continue;

			if (Bot->PawnRef.IsValid()) Bot->Pawn->K2_DestroyActor();
			if (Bot->ControllerRef.IsValid()) Bot->Controller->K2_DestroyActor();
			delete Bot;
		}

		AllPlayerBotsToTick.clear();
		BotSettings::AliveBots = 0;
	}

	// ------------------------------------------------------------------------------------------
	//  Elimination handling (AI bots don't go through Reboot's player death code)
	// ------------------------------------------------------------------------------------------

	inline void TryAnnounceWinner()
	{
		if (BotWorld::bWonAnnounced || BotSettings::AliveBots > 0)
			return;

		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());

		if (!GameMode || !GameState || GameState->GetGamePhase() < EAthenaGamePhase::SafeZones)
			return;

		auto& AlivePlayers = GameMode->GetAlivePlayers();

		if (AlivePlayers.Num() != 1)
			return;

		auto Winner = AlivePlayers.at(0);

		if (!IsActorUsable(Winner))
			return;

		auto WinnerState = Cast<AFortPlayerStateAthena>(Winner->GetPlayerState());

		if (!WinnerState)
			return;

		BotWorld::bWonAnnounced = true;
		LOG_INFO(LogBots, "Every bot is dead and only one player is left, giving them the win.");

		WinnerState->GetPlace() = 1;

		static auto OnRep_PlaceFn = BotFindFn(L"/Script/FortniteGame.FortPlayerStateAthena.OnRep_Place");
		if (OnRep_PlaceFn) WinnerState->ProcessEvent(OnRep_PlaceFn);

		static auto WinningPlayerStateOffset = GameState->GetOffset("WinningPlayerState", false);
		static auto WinningTeamOffset = GameState->GetOffset("WinningTeam", false);

		if (WinningPlayerStateOffset != -1)
		{
			GameState->Get<AFortPlayerStateAthena*>(WinningPlayerStateOffset) = WinnerState;
			static auto Fn = BotFindFn(L"/Script/FortniteGame.FortGameStateAthena.OnRep_WinningPlayerState");
			if (Fn) GameState->ProcessEvent(Fn);
		}

		if (WinningTeamOffset != -1)
		{
			GameState->Get<uint8>(WinningTeamOffset) = WinnerState->GetTeamIndex();
			static auto Fn = BotFindFn(L"/Script/FortniteGame.FortGameStateAthena.OnRep_WinningTeam");
			if (Fn) GameState->ProcessEvent(Fn);
		}

		static auto ClientNotifyWonFn = BotFindFn(L"/Script/FortniteGame.FortPlayerControllerAthena.ClientNotifyWon");
		if (ClientNotifyWonFn) FBotCall(ClientNotifyWonFn).Call(Winner);

		static auto ClientNotifyTeamWonFn = BotFindFn(L"/Script/FortniteGame.FortPlayerControllerAthena.ClientNotifyTeamWon");
		if (ClientNotifyTeamWonFn) FBotCall(ClientNotifyTeamWonFn).Call(Winner);
	}

	inline void HandleBotEliminated(PlayerBot* Bot, float Now)
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());

		FVector DeathLocation = Bot->PawnRef.IsValid() ? Bot->Pawn->GetActorLocation() : Bot->LastKnownLocation;

		Bot->StopFiring();

		// Drop everything except the pickaxe
		for (auto& Item : Bot->Items)
		{
			if (Item.WeaponKind == EBotWeaponKind::Pickaxe)
				continue;

			Bot->SpawnItemPickup(Item, DeathLocation, true);
		}

		Bot->Items.clear();
		Bot->EquippedIndex = -1;

		// Who killed us? Prefer whoever we recorded, fall back to the engine's LastHitBy.
		AFortPlayerStateAthena* KillerState = nullptr;
		AController* KillerController = nullptr;

		if (Now - Bot->LastDamagedTime < 15.f)
		{
			KillerState = Bot->LastDamagerPlayerStateRef.Get<AFortPlayerStateAthena>();
			KillerController = Bot->LastDamagerControllerRef.Get<AController>();
		}

		if (!KillerController && Bot->PawnRef.IsValid())
		{
			static auto LastHitByOffset = Bot->Pawn->GetOffset("LastHitBy", false);

			if (LastHitByOffset != -1)
			{
				auto LastHitBy = Bot->Pawn->Get<AController*>(LastHitByOffset);

				if (IsActorUsable(LastHitBy))
				{
					KillerController = LastHitBy;
					KillerState = Cast<AFortPlayerStateAthena>(LastHitBy->GetPlayerState());
				}
			}
		}

		auto DeadState = Bot->PlayerStateRef.Get<AFortPlayerStateAthena>();

		if (KillerState == DeadState)
			KillerState = nullptr;

		std::string KillerName = "the storm/fall";

		if (KillerState)
		{
			if (MemberOffsets::FortPlayerStateAthena::KillScore > 0)
				KillerState->Get<int>(MemberOffsets::FortPlayerStateAthena::KillScore)++;

			if (MemberOffsets::FortPlayerStateAthena::TeamKillScore > 0)
				KillerState->Get<int>(MemberOffsets::FortPlayerStateAthena::TeamKillScore)++;

			if (DeadState)
				KillerState->ClientReportKill(DeadState);

			auto KillerBot = KillerController ? FindBotByPawn(KillerController->GetPawn()) : nullptr;

			if (KillerBot)
			{
				KillerBot->Kills++;
				KillerName = "bot " + KillerBot->Name;
			}
			else
			{
				KillerName = "a player";
			}
		}

		// Kill feed
		if (DeadState && MemberOffsets::FortPlayerStateAthena::DeathInfo > 0 && Fortnite_Version > 1.8)
		{
			auto DeathInfo = DeadState->GetDeathInfo();
			DeadState->ClearDeathInfo();

			AActor* FinisherOrDowner = KillerState ? (AActor*)KillerState : (AActor*)DeadState;

			*(bool*)(__int64(DeathInfo) + MemberOffsets::DeathInfo::bDBNO) = false;

			if (!MemberOffsets::DeathInfo::bIsWeakFinisherOrDowner)
				*(AActor**)(__int64(DeathInfo) + MemberOffsets::DeathInfo::FinisherOrDowner) = FinisherOrDowner;

			if (MemberOffsets::DeathInfo::DeathLocation != -1)
				*(FVector*)(__int64(DeathInfo) + MemberOffsets::DeathInfo::DeathLocation) = DeathLocation;

			if (MemberOffsets::DeathInfo::Distance != -1 && KillerController && IsActorUsable(KillerController->GetPawn()))
				*(float*)(__int64(DeathInfo) + MemberOffsets::DeathInfo::Distance) = BotMath::Dist(KillerController->GetPawn()->GetActorLocation(), DeathLocation);

			if (MemberOffsets::DeathInfo::bInitialized != -1)
				*(bool*)(__int64(DeathInfo) + MemberOffsets::DeathInfo::bInitialized) = true;

			static auto OnRep_DeathInfoFn = BotFindFn(L"/Script/FortniteGame.FortPlayerStateAthena.OnRep_DeathInfo");
			if (OnRep_DeathInfoFn) DeadState->ProcessEvent(OnRep_DeathInfoFn);
		}

		if (GameState)
		{
			if (DeadState)
			{
				DeadState->GetPlace() = GameState->GetPlayersLeft();
				static auto OnRep_PlaceFn = BotFindFn(L"/Script/FortniteGame.FortPlayerStateAthena.OnRep_Place");
				if (OnRep_PlaceFn) DeadState->ProcessEvent(OnRep_PlaceFn);
			}

			if (GameState->GetPlayersLeft() > 0)
			{
				GameState->GetPlayersLeft()--;
				GameState->OnRep_PlayersLeft();
			}
		}

		Bot->State = EBotState::Dead;
		BotSettings::AliveBots--;

		LOG_INFO(LogBots, "Bot {} was eliminated by {} ({} bots left).", Bot->Name, KillerName, BotSettings::AliveBots.load());

		TryAnnounceWinner();
	}

	// ------------------------------------------------------------------------------------------
	//  Brain
	// ------------------------------------------------------------------------------------------

	inline UObject* GetRandomEmote()
	{
		static std::vector<UObject*> Emotes;
		static bool bLoaded = false;

		if (!bLoaded)
		{
			bLoaded = true;
			static auto DanceClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaDanceItemDefinition");

			if (DanceClass)
			{
				for (auto Emote : GetAllObjectsOfClass(DanceClass))
				{
					if (!Emote) continue;
					auto EmoteName = Emote->GetName();

					// Skip toys/sprays that aren't real dances
					if (ContainsI(EmoteName, "EID_") && !ContainsI(EmoteName, "Toy") && !ContainsI(EmoteName, "Spray"))
						Emotes.push_back(Emote);
				}
			}

			LOG_INFO(LogBots, "Bots can use {} emotes.", Emotes.size());
		}

		if (Emotes.empty())
			return nullptr;

		return Emotes.at(BotMath::RandI(0, (int)Emotes.size() - 1));
	}

	inline void TickWarmup(PlayerBot* Bot, float Now, float DeltaTime)
	{
		if (Now < Bot->EmoteUntil)
			return; // let the emote play

		if (Now >= Bot->NextWarmupDecision)
		{
			float Roll = BotMath::RandF(0.f, 1.f);

			if (BotSettings::bEmote && Roll < 0.18f)
			{
				if (BotEngine::PlayEmote(Bot->Controller, GetRandomEmote()))
				{
					Bot->EmoteUntil = Now + BotMath::RandF(4.f, 9.f);
					Bot->NextWarmupDecision = Bot->EmoteUntil + 0.5f;
					Bot->bWarmupWalking = false;
					return;
				}
			}

			if (Roll < 0.75f)
			{
				// Walk somewhere nearby (pull back towards where we spawned if we wandered off)
				float Angle = BotMath::RandF(-180.f, 180.f);
				float Distance = BotMath::RandF(300.f, 1500.f);
				FVector Base = BotMath::Dist2D(Bot->Location(), Bot->HomeLocation) > 2500.f ? Bot->HomeLocation : Bot->Location();
				Bot->MoveTarget = BotMath::Add(Base, BotMath::Mul(BotMath::DirFromYaw(Angle), Distance));
				Bot->bWarmupWalking = true;
			}
			else
			{
				Bot->bWarmupWalking = false; // stand around for a bit
			}

			Bot->NextWarmupDecision = Now + BotMath::RandF(2.f, 6.f);
		}

		if (Bot->bWarmupWalking)
		{
			float Left = Bot->MoveTowards(Bot->MoveTarget, Now, DeltaTime);

			if (Left < 120.f)
				Bot->bWarmupWalking = false;

			if (Now >= Bot->NextJumpTime)
			{
				Bot->NextJumpTime = Now + BotMath::RandF(2.f, 8.f);
				if (BotMath::Chance(0.5f)) BotEngine::Jump(Bot->Pawn);
			}
		}
	}

	inline void PickLandingTarget(PlayerBot* Bot)
	{
		Bot->bHasLandingTarget = false;

		if (!BotWorld::Chests.empty())
		{
			// Land at a random chest; that puts bots at real named locations
			auto& Chest = BotWorld::Chests.at(BotMath::RandI(0, (int)BotWorld::Chests.size() - 1));
			Bot->LandingTarget = Chest.Location;
			Bot->bHasLandingTarget = true;
		}
	}

	inline void EnterBus(PlayerBot* Bot, float Now)
	{
		Bot->StopFiring();
		Bot->State = EBotState::Bus;
		Bot->MinBusDistance = 1e12f;
		Bot->BusJumpDeadline = Now + BotMath::RandF(45.f, 65.f);
		Bot->bOpenedParachute = false;
		Bot->bThankedDriver = false;
		Bot->ThankDriverTime = Now + BotMath::RandF(2.f, 12.f);
		Bot->EmoteUntil = 0.f;

		// Warmup loot doesn't come with you, like for real players
		std::vector<FBotItem> Kept;
		for (auto& Item : Bot->Items)
			if (Item.WeaponKind == EBotWeaponKind::Pickaxe)
				Kept.push_back(Item);

		Bot->Items = Kept;
		Bot->EquippedIndex = -1;
		Bot->EquippedWeaponRef.Reset();

		PickLandingTarget(Bot);

		if (Bot->PawnRef.IsValid())
		{
			// Ride the bus "invisibly": hide the pawn and make it untouchable until we jump.
			BotEngine::SetHidden(Bot->Pawn, true);
			Bot->Pawn->SetCanBeDamaged(false);
		}
	}

	inline void JumpFromBus(PlayerBot* Bot, AActor* Aircraft, float Now)
	{
		FVector BusLocation = Aircraft ? Aircraft->GetActorLocation() : BotMath::Add(Bot->LandingTarget, FVector(0, 0, 15000.f));
		FVector JumpLocation = BotMath::Add(BusLocation, FVector(BotMath::RandF(-400.f, 400.f), BotMath::RandF(-400.f, 400.f), -700.f));

		FRotator Rotation;
		Rotation.Pitch = 0;
		Rotation.Yaw = Bot->bHasLandingTarget ? BotMath::YawTo(JumpLocation, Bot->LandingTarget) : 0.f;
		Rotation.Roll = 0;

		if (!Bot->PawnRef.IsValid() || !IsActorUsable(Bot->Pawn))
		{
			// The game took our pawn away for the bus; give us a new one in the sky
			FTransform Transform;
			Transform.Translation = JumpLocation;
			Transform.Rotation = Rotation.Quaternion();
			Transform.Scale3D = FVector(1, 1, 1);

			if (!Bot->SpawnPawnAndController(Transform))
			{
				Bot->bBroken = true;
				return;
			}

			Bot->SetupNewPawn();
		}
		else
		{
			Bot->Pawn->TeleportTo(JumpLocation, Rotation);
			BotEngine::SetHidden(Bot->Pawn, false);
		}

		Bot->Pawn->SetCanBeDamaged(false); // no fall damage surprises on the way down
		BotEngine::ResetFallingHeight(Bot->Pawn);
		BotEngine::KeepAwakeAndMoving(Bot->Controller, Bot->Pawn, true);
		BotEngine::BeginSkydiving(Bot->Pawn);

		Bot->CurrentYaw = Rotation.Yaw;
		Bot->DiagnosticTime = -1.f;
		Bot->DiagnosticsLogged = 0;
		Bot->State = EBotState::Skydive;
		Bot->NextAltitudeCheck = 0.f;

		static bool bLoggedOnce = false;

		if (!bLoggedOnce)
		{
			bLoggedOnce = true;
			LOG_INFO(LogBots, "First bot jumped from the bus. IsSkydiving: {}", BotEngine::IsSkydiving(Bot->Pawn));
		}
	}

	inline void TickBus(PlayerBot* Bot, float Now, float DeltaTime)
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto Aircraft = GetAircraft();
		bool bAircraftValid = IsActorUsable(Aircraft);

		if (!Bot->bThankedDriver && Now >= Bot->ThankDriverTime)
		{
			Bot->bThankedDriver = true;
			if (BotMath::Chance(0.35f)) BotEngine::ThankBusDriver(Bot->Controller);
		}

		bool bBusGone = !bAircraftValid || (GameState && GameState->GetGamePhase() > EAthenaGamePhase::Aircraft);
		float TimeOnBus = BotWorld::AircraftPhaseStartTime > 0.f ? Now - BotWorld::AircraftPhaseStartTime : 0.f;

		if (bBusGone || Now >= Bot->BusJumpDeadline)
		{
			JumpFromBus(Bot, bAircraftValid ? Aircraft : nullptr, Now);
			return;
		}

		if (TimeOnBus < 7.f) // the drop zone isn't open straight away
			return;

		if (!Bot->bHasLandingTarget)
		{
			if (BotMath::Chance(DeltaTime / 20.f)) // no idea where to go, just jump at some point
				JumpFromBus(Bot, Aircraft, Now);
			return;
		}

		float Distance = BotMath::Dist2D(Aircraft->GetActorLocation(), Bot->LandingTarget);

		// Jump when the bus is near our spot, or once it starts flying away from it
		if (Distance < 18000.f || (Distance > Bot->MinBusDistance + 1500.f && Bot->MinBusDistance < 60000.f))
		{
			JumpFromBus(Bot, Aircraft, Now);
			return;
		}

		Bot->MinBusDistance = (std::min)(Bot->MinBusDistance, Distance);
	}

	inline void Land(PlayerBot* Bot)
	{
		Bot->State = EBotState::Ground;
		Bot->Goal = EBotGoal::None;
		Bot->Pawn->SetCanBeDamaged(true);
		BotEngine::ResetFallingHeight(Bot->Pawn);
		Bot->StuckCount = 0;
		Bot->NextLootScan = 0.f;
	}

	inline void TickAirborne(PlayerBot* Bot, float Now, float DeltaTime)
	{
		auto MyLocation = Bot->Location();

		if (Now >= Bot->NextAltitudeCheck)
		{
			Bot->NextAltitudeCheck = Now + 0.2f;
			FVector Ground = MyLocation;
			Bot->LastAltitude = BotEngine::HeightAboveGround(Bot->Pawn, MyLocation, &Ground);

			bool bSkydiving = BotEngine::IsSkydiving(Bot->Pawn);
			bool bParachute = BotEngine::IsParachuteOpen(Bot->Pawn);

			// Glider time
			if (!Bot->bOpenedParachute && Bot->LastAltitude < BotMath::RandF(3500.f, 5000.f))
			{
				Bot->bOpenedParachute = true;
				BotEngine::OpenParachute(Bot->Pawn);
				Bot->State = EBotState::Glide;
			}

			// Safety net: if the game didn't put us into skydive/glide we'd fall like a rock and die,
			// so put the bot on the ground before that happens.
			auto Velocity = BotEngine::GetVelocity(Bot->Pawn);

			if (Bot->LastAltitude < 1500.f && !bSkydiving && !bParachute && Velocity.Z < -2500.f)
			{
				Bot->Pawn->TeleportTo(BotMath::Add(Ground, FVector(0, 0, 100.f)), Bot->Pawn->GetActorRotation());
				Land(Bot);
				return;
			}

			if (Bot->LastAltitude < 160.f || (!bSkydiving && !bParachute && Bot->LastAltitude < 400.f && std::fabs((float)Velocity.Z) < 50.f))
			{
				Land(Bot);
				return;
			}
		}

		// Steer towards where we want to land
		if (Bot->bHasLandingTarget)
		{
			auto Direction = BotMath::Normal2D(BotMath::Sub(Bot->LandingTarget, MyLocation));
			Bot->FaceYaw(BotMath::YawOf(Direction), Bot->State == EBotState::Skydive ? -45.f : 0.f, DeltaTime, 180.f);

			if (BotMath::Dist2D(MyLocation, Bot->LandingTarget) > 300.f)
				BotEngine::AddMovementInput(Bot->Pawn, Direction, 1.f);
		}
	}

	// Find the closest enemy we can see.
	inline AFortPawn* FindTarget(PlayerBot* Bot, float MaxRange)
	{
		auto MyLocation = Bot->Location();
		auto MyTeam = Bot->PlayerState->GetTeamIndex();
		FVector Eye = BotMath::Add(MyLocation, FVector(0, 0, 60.f));

		AFortPawn* Best = nullptr;
		float BestDistance = MaxRange;

		for (auto& Ref : BotWorld::Pawns)
		{
			auto Other = Ref.Get<AFortPawn>();

			if (!Other || Other == (AFortPawn*)Bot->Pawn || !IsActorUsable(Other))
				continue;

			auto OtherLocation = Other->GetActorLocation();
			float Distance = BotMath::Dist(MyLocation, OtherLocation);

			if (Distance >= BestDistance)
				continue;

			if (OtherLocation.Z - MyLocation.Z > 2500.f) // still skydiving/gliding above us
				continue;

			auto OtherState = Cast<AFortPlayerStateAthena>(Other->GetPlayerState());

			if (!OtherState || OtherState->GetTeamIndex() == MyTeam)
				continue;

			if (auto OtherBot = FindBotByPawn(Other))
			{
				if (OtherBot->State != EBotState::Ground && OtherBot->State != EBotState::Glide)
					continue; // still on the bus/hidden
			}

			if (Other->GetHealth() <= 0.f)
				continue;

			// Line of sight
			FVector Hit;
			FVector TargetChest = BotMath::Add(OtherLocation, FVector(0, 0, 40.f));

			if (BotEngine::Trace(Bot->Pawn, Eye, TargetChest, &Hit) && BotMath::Dist(Hit, TargetChest) > 200.f)
				continue;

			Best = Other;
			BestDistance = Distance;
		}

		return Best;
	}

	inline void TickCombat(PlayerBot* Bot, AFortPawn* Target, float Now, float DeltaTime)
	{
		auto MyLocation = Bot->Location();
		auto TargetLocation = Target->GetActorLocation();
		float Distance = BotMath::Dist(MyLocation, TargetLocation);
		float Skill = Bot->Skill01();

		// Weapon choice for this range
		if (Now >= Bot->ReloadUntil)
			Bot->EquipBestWeapon(false, Distance);

		auto Kind = Bot->EquippedKind();
		auto Stats = GetWeaponStats(Kind == EBotWeaponKind::None ? EBotWeaponKind::Pickaxe : Kind);

		// Aim (with some human-like error)
		FVector Eye = BotMath::Add(MyLocation, FVector(0, 0, 60.f));
		FVector AimPoint = BotMath::Add(TargetLocation, FVector(0, 0, 30.f));
		float AimError = BotMath::Lerp(9.f, 1.5f, Skill);
		float Yaw = BotMath::YawTo(Eye, AimPoint) + BotMath::RandF(-AimError, AimError) * 0.3f;
		float Pitch = BotMath::PitchTo(Eye, AimPoint) + BotMath::RandF(-AimError, AimError) * 0.2f;
		Bot->FaceYaw(Yaw, Pitch, DeltaTime, BotMath::Lerp(240.f, 900.f, Skill));

		// Movement: close the gap, keep distance, strafe
		if (Now >= Bot->NextStrafeSwitch)
		{
			Bot->NextStrafeSwitch = Now + BotMath::RandF(0.6f, 2.0f);
			Bot->StrafeYawOffset = BotMath::Chance(0.5f) ? 90.f : -90.f;
		}

		FVector ToTarget = BotMath::Normal2D(BotMath::Sub(TargetLocation, MyLocation));
		FVector Strafe = BotMath::DirFromYaw(BotMath::YawTo(MyLocation, TargetLocation) + Bot->StrafeYawOffset);
		FVector MoveDirection;

		if (Distance > Stats.PreferredDist * 1.4f)
			MoveDirection = BotMath::Normal2D(BotMath::Add(ToTarget, BotMath::Mul(Strafe, 0.35f)));
		else if (Distance < Stats.PreferredDist * 0.6f && Kind != EBotWeaponKind::Pickaxe)
			MoveDirection = BotMath::Normal2D(BotMath::Add(BotMath::Mul(ToTarget, -1.f), BotMath::Mul(Strafe, 0.5f)));
		else
			MoveDirection = Strafe;

		BotEngine::AddMovementInput(Bot->Pawn, MoveDirection, 1.f);

		if (Now >= Bot->NextJumpTime)
		{
			Bot->NextJumpTime = Now + BotMath::RandF(1.5f, 5.f);
			if (BotMath::Chance(BotMath::Lerp(0.15f, 0.5f, Skill))) BotEngine::Jump(Bot->Pawn);
		}

		// Shooting
		if (Now < Bot->ReactionReadyTime || Now < Bot->ReloadUntil || Distance > Stats.Range * 1.2f)
		{
			Bot->StopFiring();
			return;
		}

		auto Weapon = Bot->GetEquippedWeapon();
		bool bIsGun = Kind != EBotWeaponKind::Pickaxe && Kind != EBotWeaponKind::None;

		// Reload when the mag is empty
		if (bIsGun && Weapon && Bot->EquippedIndex >= 0)
		{
			int Ammo = BotWorld::bUseSimulatedShots ? Bot->SimulatedMag : Weapon->GetAmmoCount();

			if (Ammo <= 0)
			{
				Bot->StopFiring();
				Bot->ReloadUntil = Now + Stats.ReloadTime * BotMath::RandF(0.9f, 1.2f);

				auto WeaponDef = Cast<UFortWeaponItemDefinition>(Bot->Items[Bot->EquippedIndex].Definition);
				int Clip = WeaponDef ? WeaponDef->GetClipSize() : 30;
				if (Clip <= 0) Clip = 30;

				Weapon->GetAmmoCount() = Clip;
				Bot->SimulatedMag = Clip;
				Bot->Items[Bot->EquippedIndex].LoadedAmmo = Clip;
				return;
			}
		}

		if (!BotWorld::bUseSimulatedShots)
		{
			// Fire in bursts like a player would
			if (Now >= Bot->FireStateUntil)
			{
				if (Bot->bFiring)
				{
					Bot->StopFiring();
					Bot->FireStateUntil = Now + (Kind == EBotWeaponKind::Shotgun ? Stats.FireInterval : BotMath::RandF(0.15f, 0.6f));
				}
				else
				{
					BotEngine::StartFire(Bot->Pawn);
					Bot->bFiring = true;
					Bot->FireStateUntil = Now + (Kind == EBotWeaponKind::Shotgun ? 0.1f : BotMath::RandF(0.3f, 1.2f));

					if (!BotWorld::bRealFireVerified && bIsGun && Weapon && Bot->AmmoAtFireStart < 0)
					{
						Bot->AmmoAtFireStart = Weapon->GetAmmoCount();
						Bot->FireStartTime = Now;
					}
				}
			}

			// Find out once whether AI weapons actually fire on this version
			if (!BotWorld::bRealFireVerified && bIsGun && Weapon && Bot->AmmoAtFireStart >= 0 && Now - Bot->FireStartTime > 2.5f)
			{
				if (Weapon->GetAmmoCount() != Bot->AmmoAtFireStart)
				{
					BotWorld::bRealFireVerified = true;
					LOG_INFO(LogBots, "Bot weapons fire for real on this version.");
				}
				else
				{
					BotWorld::bUseSimulatedShots = true;
					Bot->StopFiring();
					LOG_WARN(LogBots, "Bot weapons don't fire on this version, switching to simulated shots.");
				}

				Bot->AmmoAtFireStart = -1;
			}

			// The pickaxe always does its damage through us (melee for AI is unreliable)
			if (Kind != EBotWeaponKind::Pickaxe)
				return;
		}

		// Simulated shot
		if (Now < Bot->NextSimulatedShot)
			return;

		Bot->NextSimulatedShot = Now + Stats.FireInterval * BotMath::RandF(0.9f, 1.3f);

		if (bIsGun)
		{
			Bot->SimulatedMag--;
			if (Weapon) Weapon->GetAmmoCount() = (std::max)(0, Bot->SimulatedMag);

			// Show the shot (muzzle flash/sound) even though the damage is ours
			BotEngine::StartFire(Bot->Pawn);
			BotEngine::StopFire(Bot->Pawn);
		}
		else
		{
			if (Distance > Stats.Range)
				return;

			BotEngine::StartFire(Bot->Pawn); // pickaxe swing
			BotEngine::StopFire(Bot->Pawn);
		}

		float HitChance = BotMath::Lerp(0.2f, 0.65f, Skill) * BotMath::Clamp(1.2f - Distance / (Stats.Range * 1.3f), 0.15f, 1.f);

		if (Kind == EBotWeaponKind::Shotgun)
			HitChance = BotMath::Clamp(HitChance * 1.4f, 0.f, 0.95f);

		if (!BotMath::Chance(HitChance))
			return;

		float Damage = Stats.Damage;

		if (Kind == EBotWeaponKind::Shotgun)
			Damage *= BotMath::Clamp(1.f - Distance / 1200.f, 0.25f, 1.f);

		if (BotMath::Chance(BotMath::Lerp(0.05f, 0.3f, Skill)))
			Damage *= 1.5f; // headshot-ish

		if (auto TargetBot = FindBotByPawn(Target))
			TargetBot->NoteDamagedBy(Bot->Controller, Now);

		PlayerBot::ApplyDamageToPawn(Target, Damage, Bot->Controller, Bot->Pawn);
	}

	inline bool TickHealing(PlayerBot* Bot, float Now)
	{
		if (Bot->HealUntil > 0.f)
		{
			if (Now < Bot->HealUntil)
				return true; // still "using" the item

			// Done: apply it
			Bot->HealUntil = 0.f;

			if (Bot->HealingItemIndex >= 0 && Bot->HealingItemIndex < (int)Bot->Items.size())
			{
				auto& Item = Bot->Items[Bot->HealingItemIndex];
				float Health = Bot->Pawn->GetHealth();
				float Shield = Bot->Pawn->GetShield();

				switch (Item.HealKind)
				{
				case EBotHealKind::ShieldSmall: if (Shield < 50.f) Bot->Pawn->SetShield((std::min)(50.f, Shield + 25.f)); break;
				case EBotHealKind::ShieldBig: Bot->Pawn->SetShield((std::min)(100.f, Shield + 50.f)); break;
				case EBotHealKind::Medkit: Bot->Pawn->SetHealth(100.f); break;
				case EBotHealKind::Bandage: if (Health < 75.f) Bot->Pawn->SetHealth((std::min)(75.f, Health + 15.f)); break;
				case EBotHealKind::Slurp: Bot->Pawn->SetHealth((std::min)(100.f, Health + 75.f)); Bot->Pawn->SetShield((std::min)(100.f, Shield + 25.f)); break;
				case EBotHealKind::Chug: Bot->Pawn->SetHealth(100.f); Bot->Pawn->SetShield(100.f); break;
				default: break;
				}

				Item.Count--;

				if (Item.Count <= 0)
				{
					if (Bot->HealingItemIndex < Bot->EquippedIndex) Bot->EquippedIndex--;
					Bot->Items.erase(Bot->Items.begin() + Bot->HealingItemIndex);
				}
			}

			Bot->HealingItemIndex = -1;
			return false;
		}

		// Should we heal?
		float Health = Bot->Pawn->GetHealth();
		float Shield = Bot->Pawn->GetShield();

		int Best = -1;
		float BestTime = 0.f;

		for (int i = 0; i < (int)Bot->Items.size(); ++i)
		{
			auto Kind = Bot->Items[i].HealKind;
			float UseTime = 0.f;
			bool bUseful = false;

			switch (Kind)
			{
			case EBotHealKind::ShieldSmall: bUseful = Shield < 50.f; UseTime = 2.f; break;
			case EBotHealKind::ShieldBig: bUseful = Shield < 50.f; UseTime = 5.f; break;
			case EBotHealKind::Medkit: bUseful = Health < 60.f; UseTime = 10.f; break;
			case EBotHealKind::Bandage: bUseful = Health < 60.f; UseTime = 4.f; break;
			case EBotHealKind::Slurp: bUseful = Health < 70.f || Shield < 50.f; UseTime = 2.f; break;
			case EBotHealKind::Chug: bUseful = Health < 50.f; UseTime = 15.f; break;
			default: break;
			}

			if (bUseful && (Best == -1 || UseTime < BestTime))
			{
				Best = i;
				BestTime = UseTime;
			}
		}

		if (Best == -1)
			return false;

		Bot->StopFiring();
		Bot->HealingItemIndex = Best;
		Bot->HealUntil = Now + BestTime;
		return true;
	}

	inline bool TryTakePickup(PlayerBot* Bot, AFortPickup* Pickup, float Now)
	{
		static auto bPickedUpOffset = Pickup->GetOffset("bPickedUp", false);

		if (bPickedUpOffset != -1 && Pickup->Get<bool>(bPickedUpOffset))
			return false;

		auto Entry = Pickup->GetPrimaryPickupItemEntry();

		if (!Entry || !Entry->GetItemDefinition())
			return false;

		if (!Bot->WantsItem(Entry->GetItemDefinition()) || !Bot->GiveItem(Entry->GetItemDefinition(), Entry->GetCount(), Entry->GetLoadedAmmo()))
		{
			Bot->IgnoreObject(Pickup, Now, 60.f);
			return false;
		}

		if (bPickedUpOffset != -1)
		{
			Pickup->Get<bool>(bPickedUpOffset) = true;
			static auto OnRep_bPickedUpFn = BotFindFn(L"/Script/FortniteGame.FortPickup.OnRep_bPickedUp");
			if (OnRep_bPickedUpFn) Pickup->ProcessEvent(OnRep_bPickedUpFn);
		}

		Pickup->K2_DestroyActor();
		Bot->EquipBestWeapon();
		return true;
	}

	inline void OpenChest(PlayerBot* Bot, ABuildingContainer* Container)
	{
		static auto bAlreadySearchedOffset = Container->GetOffset("bAlreadySearched");
		static auto SearchBounceDataOffset = Container->GetOffset("SearchBounceData", false);
		static auto bAlreadySearchedFieldMask = GetFieldMask(Container->GetProperty("bAlreadySearched"));
		static auto SearchAnimationCountOffset = FindOffsetStruct("/Script/FortniteGame.FortSearchBounceData", "SearchAnimationCount", false);

		if (Container->ReadBitfieldValue(bAlreadySearchedOffset, bAlreadySearchedFieldMask))
			return;

		Container->SpawnLoot(Bot->Pawn);
		Container->SetBitfieldValue(bAlreadySearchedOffset, bAlreadySearchedFieldMask, true);

		if (SearchBounceDataOffset != -1 && SearchAnimationCountOffset != -1)
		{
			auto SearchBounceData = Container->GetPtr<void>(SearchBounceDataOffset);
			(*(int*)(__int64(SearchBounceData) + SearchAnimationCountOffset))++;
		}

		Container->BounceContainer();
		Container->ForceNetUpdate();

		BotWorld::NextPickupRefresh = 0.f; // let everyone see the new loot
		Bot->NextLootScan = 0.f;
	}

	// Pick what to do on the ground when not fighting.
	inline void ChooseGroundGoal(PlayerBot* Bot, float Now)
	{
		auto MyLocation = Bot->Location();
		auto Zone = GetSafeZone();

		// 1. Storm: get into the next circle first
		if (Zone.bValid)
		{
			bool bOutsideNow = BotMath::Dist2D(MyLocation, Zone.Center) > Zone.Radius * 0.95f;
			bool bOutsideNext = BotMath::Dist2D(MyLocation, Zone.NextCenter) > Zone.NextRadius * 0.85f;

			if (bOutsideNow || bOutsideNext)
			{
				auto ToCenter = BotMath::Normal2D(BotMath::Sub(Zone.NextCenter, MyLocation));
				float Inset = Zone.NextRadius * BotMath::RandF(0.3f, 0.7f);
				Bot->MoveTarget = BotMath::Sub(Zone.NextCenter, BotMath::Mul(ToCenter, Inset));
				Bot->Goal = EBotGoal::Storm;
				Bot->GoalExpireTime = Now + 20.f;
				return;
			}
		}

		// 2. Loot
		if (BotSettings::bLoot)
		{
			AFortPickup* BestPickup = nullptr;
			float BestPickupDistance = Bot->HasGun() ? 2000.f : 3500.f;

			for (auto& Cached : BotWorld::Pickups)
			{
				float Distance = BotMath::Dist(MyLocation, Cached.Location);

				if (Distance >= BestPickupDistance)
					continue;

				auto Pickup = Cached.Ref.Get<AFortPickup>();

				if (!Pickup || Bot->IsIgnored(Pickup, Now) || !Bot->WantsItem(Cached.Definition))
					continue;

				BestPickup = Pickup;
				BestPickupDistance = Distance;
			}

			if (BestPickup)
			{
				Bot->TargetPickupRef.Set(BestPickup);
				Bot->MoveTarget = BestPickup->GetActorLocation();
				Bot->Goal = EBotGoal::Pickup;
				Bot->GoalExpireTime = Now + 15.f;
				return;
			}

			ABuildingContainer* BestChest = nullptr;
			float BestChestDistance = Bot->HasGun() ? 3000.f : 6000.f;

			for (auto& Cached : BotWorld::Chests)
			{
				float Distance = BotMath::Dist(MyLocation, Cached.Location);

				if (Distance >= BestChestDistance)
					continue;

				auto Chest = Cached.Ref.Get<ABuildingContainer>();

				if (!Chest || Bot->IsIgnored(Chest, Now) || Chest->IsAlreadySearched())
					continue;

				BestChest = Chest;
				BestChestDistance = Distance;
			}

			if (BestChest)
			{
				Bot->TargetChestRef.Set(BestChest);
				Bot->MoveTarget = BestChest->GetActorLocation();
				Bot->Goal = EBotGoal::Chest;
				Bot->GoalExpireTime = Now + 20.f;
				return;
			}
		}

		// 3. Roam: somewhere inside the circle (or somewhere nearby if there's no circle yet)
		FVector Center = Zone.bValid ? Zone.NextCenter : MyLocation;
		float Radius = Zone.bValid ? Zone.NextRadius * 0.6f : 4000.f;
		Bot->MoveTarget = BotMath::Add(Center, BotMath::Mul(BotMath::DirFromYaw(BotMath::RandF(-180.f, 180.f)), BotMath::RandF(0.f, Radius)));
		Bot->Goal = EBotGoal::Wander;
		Bot->GoalExpireTime = Now + BotMath::RandF(8.f, 20.f);
	}

	inline void TickGround(PlayerBot* Bot, float Now, float DeltaTime)
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		bool bCanFight = BotSettings::bFight && GameState && GameState->GetGamePhase() >= EAthenaGamePhase::SafeZones;

		// --- Combat --------------------------------------------------------------------------
		if (bCanFight && Now >= Bot->NextTargetScan)
		{
			Bot->NextTargetScan = Now + BotMath::Lerp(0.6f, 0.25f, Bot->Skill01());

			auto NewTarget = FindTarget(Bot, BotMath::Lerp(4500.f, 7500.f, Bot->Skill01()));

			if (NewTarget)
			{
				if (Bot->TargetPawnRef.Object != NewTarget)
					Bot->ReactionReadyTime = Now + BotMath::Lerp(0.9f, 0.2f, Bot->Skill01()) * BotMath::RandF(0.8f, 1.3f);

				Bot->TargetPawnRef.Set(NewTarget);
				Bot->LastSeenTargetLocation = NewTarget->GetActorLocation();
				Bot->LastSeenTargetTime = Now;
			}
		}

		auto Target = Bot->TargetPawnRef.Get<AFortPawn>();

		if (Target && (!IsActorUsable(Target) || Target->GetHealth() <= 0.f))
		{
			Bot->TargetPawnRef.Reset();
			Target = nullptr;
		}

		if (Target && bCanFight && Now - Bot->LastSeenTargetTime < 3.f)
		{
			Bot->HealUntil = 0.f; // interrupt healing
			Bot->Goal = EBotGoal::Fight;
			TickCombat(Bot, Target, Now, DeltaTime);
			return;
		}

		if (Target && bCanFight && Now - Bot->LastSeenTargetTime < 8.f)
		{
			// Lost sight: push to where we last saw them
			Bot->StopFiring();
			Bot->MoveTowards(Bot->LastSeenTargetLocation, Now, DeltaTime);
			return;
		}

		Bot->StopFiring();
		Bot->TargetPawnRef.Reset();

		if (Bot->Goal == EBotGoal::Fight)
			Bot->Goal = EBotGoal::None;

		// --- Healing ------------------------------------------------------------------------
		if (TickHealing(Bot, Now))
			return;

		// --- Goals --------------------------------------------------------------------------
		if (Bot->Goal == EBotGoal::None || Now >= Bot->GoalExpireTime || (Bot->Goal == EBotGoal::Wander && Now >= Bot->NextLootScan))
		{
			Bot->NextLootScan = Now + BotMath::RandF(1.5f, 3.f);
			ChooseGroundGoal(Bot, Now);
		}

		float Left = Bot->MoveTowards(Bot->MoveTarget, Now, DeltaTime);

		switch (Bot->Goal)
		{
		case EBotGoal::Pickup:
		{
			auto Pickup = Bot->TargetPickupRef.Get<AFortPickup>();

			if (!Pickup || !IsActorUsable(Pickup))
			{
				Bot->Goal = EBotGoal::None;
				break;
			}

			Bot->MoveTarget = Pickup->GetActorLocation();

			if (BotMath::Dist(Bot->Location(), Bot->MoveTarget) < 200.f)
			{
				TryTakePickup(Bot, Pickup, Now);
				Bot->Goal = EBotGoal::None;
			}
			else if (Bot->IsStuckForGood())
			{
				Bot->IgnoreObject(Pickup, Now, 30.f);
				Bot->Goal = EBotGoal::None;
				Bot->StuckCount = 0;
			}
			break;
		}
		case EBotGoal::Chest:
		{
			auto Chest = Bot->TargetChestRef.Get<ABuildingContainer>();

			if (!Chest || !IsActorUsable(Chest) || Chest->IsAlreadySearched())
			{
				Bot->Goal = EBotGoal::None;
				break;
			}

			if (BotMath::Dist(Bot->Location(), Chest->GetActorLocation()) < 250.f)
			{
				OpenChest(Bot, Chest);
				Bot->Goal = EBotGoal::None;
			}
			else if (Bot->IsStuckForGood())
			{
				Bot->IgnoreObject(Chest, Now, 45.f);
				Bot->Goal = EBotGoal::None;
				Bot->StuckCount = 0;
			}
			break;
		}
		default:
			if (Left < 200.f || Bot->IsStuckForGood())
			{
				Bot->Goal = EBotGoal::None;
				Bot->StuckCount = 0;
			}
			break;
		}

		// Keep the best gun out while walking around
		if (Now >= Bot->ReloadUntil)
			Bot->EquipBestWeapon();
	}

	inline void ApplyBotStormDamage(PlayerBot* Bot, float Now)
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());

		if (!GameState || GameState->GetGamePhase() < EAthenaGamePhase::SafeZones || Now < Bot->NextStormTick)
			return;

		Bot->NextStormTick = Now + 1.f; // storm ticks once per second

		auto Zone = GetSafeZone();

		if (!Zone.bValid || BotMath::Dist2D(Bot->Location(), Zone.Center) <= Zone.Radius)
			return;

		static const float DamagePerPhase[] = { 1.f, 1.f, 2.f, 5.f, 8.f, 10.f, 10.f, 10.f, 10.f, 10.f };
		PlayerBot::ApplyDamageToPawn(Bot->Pawn, DamagePerPhase[(std::min)((std::max)(Zone.Phase, 0), 9)], nullptr, nullptr);
	}

	inline void TickBot(PlayerBot* Bot, float Now, float DeltaTime)
	{
		if (Bot->State == EBotState::Dead)
			return;

		if (!Bot->ControllerRef.IsValid() || !Bot->PlayerStateRef.IsValid())
		{
			LOG_WARN(LogBots, "Bot {} lost its controller/playerstate, removing it.", Bot->Name);
			Bot->bBroken = true;
			return;
		}

		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto Phase = GameState ? GameState->GetGamePhase() : EAthenaGamePhase::None;

		// Respawn in warmup like real players do
		if (Bot->State == EBotState::WaitingToRespawn)
		{
			if (Now < Bot->RespawnTime)
				return;

			auto Starts = GetWarmupStarts();

			if (Starts.empty() || Phase > EAthenaGamePhase::Warmup)
			{
				EnterBus(Bot, Now); // the match moved on while we were dead
				return;
			}

			FTransform Transform = Starts.at(BotMath::RandI(0, (int)Starts.size() - 1))->GetTransform();
			Transform.Translation.Z += 60.f;

			if (!Bot->SpawnPawnAndController(Transform))
			{
				Bot->bBroken = true;
				return;
			}

			Bot->SetupNewPawn();
			Bot->HomeLocation = Bot->Location();
			Bot->State = EBotState::Warmup;
			return;
		}

		// Bus phase started while we were on spawn island
		if (Bot->State == EBotState::Warmup && Phase == EAthenaGamePhase::Aircraft)
		{
			EnterBus(Bot, Now);
			return;
		}

		if (Bot->State == EBotState::Bus)
		{
			TickBus(Bot, Now, DeltaTime);
			return;
		}

		// From here on we need a living pawn
		bool bPawnValid = Bot->PawnRef.IsValid() && IsActorUsable(Bot->Pawn);

		if (!bPawnValid || Bot->Pawn->GetHealth() <= 0.f)
		{
			if (Phase <= EAthenaGamePhase::Warmup)
			{
				Bot->State = EBotState::WaitingToRespawn;
				Bot->RespawnTime = Now + 3.f;
				Bot->StopFiring();
				return;
			}

			HandleBotEliminated(Bot, Now);
			return;
		}

		// Stop Fortnite from putting the bot to sleep, and log whether it actually moves
		if (Now >= Bot->NextKeepAwake)
		{
			Bot->NextKeepAwake = Now + 2.f;
			Bot->EnsurePossessed(false);
			BotEngine::KeepAwakeAndMoving(Bot->Controller, Bot->Pawn, false);
		}

		bool bDiagnoseThisBot = false;

		for (size_t i = 0; i < AllPlayerBotsToTick.size() && i < 3; ++i)
			if (AllPlayerBotsToTick[i] == Bot)
				bDiagnoseThisBot = true;

		if (bDiagnoseThisBot && Bot->DiagnosticsLogged < 3)
		{
			if (Bot->DiagnosticTime < 0.f)
			{
				Bot->DiagnosticTime = Now + 4.f;
				Bot->DiagnosticLocation = Bot->Location();
			}
			else if (Now >= Bot->DiagnosticTime)
			{
				auto Here = Bot->Location();
				auto PawnController = (UObject*)Bot->Pawn->GetController();

				LOG_INFO(LogBots, "[Diag] {} state {} moved {:.0f} in 4s (z {:.0f} -> {:.0f}) movementmode {} possessed-by-us {} skydiving {}{}",
					Bot->Name, (int)Bot->State, BotMath::Dist(Here, Bot->DiagnosticLocation), (float)Bot->DiagnosticLocation.Z, (float)Here.Z,
					BotEngine::GetMovementMode(Bot->Pawn), PawnController == (UObject*)Bot->Controller, BotEngine::IsSkydiving(Bot->Pawn),
					BotEngine::DescribeMovement(Bot->Pawn, Bot->Controller));

				Bot->DiagnosticsLogged++;
				Bot->DiagnosticTime = Now + 4.f;
				Bot->DiagnosticLocation = Here;
			}
		}

		if (BotSettings::bUseEpicAI)
		{
			// Let Fortnite's own brain drive. We still handle the bus and landing.
			if (Bot->State == EBotState::Skydive || Bot->State == EBotState::Glide)
				TickAirborne(Bot, Now, DeltaTime);
			return;
		}

		switch (Bot->State)
		{
		case EBotState::Warmup:
			TickWarmup(Bot, Now, DeltaTime);
			break;
		case EBotState::Skydive:
		case EBotState::Glide:
			TickAirborne(Bot, Now, DeltaTime);
			break;
		case EBotState::Ground:
			TickGround(Bot, Now, DeltaTime);
			ApplyBotStormDamage(Bot, Now);
			break;
		default:
			break;
		}
	}

	// SEH guard: if anything inside a bot's tick crashes, drop that bot instead of the server.
	// (No C++ objects with destructors may live in this function.)
	inline bool TickBotGuarded(PlayerBot* Bot, float Now, float DeltaTime)
	{
		__try
		{
			TickBot(Bot, Now, DeltaTime);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	inline void ResetForNewMatch()
	{
		// The old bots belong to the old world; just forget them (the world cleans up its actors)
		for (auto Bot : AllPlayerBotsToTick)
			delete Bot;

		AllPlayerBotsToTick.clear();
		BotWorld::Pawns.clear();
		BotWorld::Pickups.clear();
		BotWorld::Chests.clear();
		BotWorld::bBuiltChestCache = false;
		BotWorld::LastPhase = EAthenaGamePhase::None;
		BotWorld::AircraftPhaseStartTime = -1.f;
		BotWorld::bWonAnnounced = false;
		BotSettings::AliveBots = 0;
		InitBotNames();
	}

	inline void Tick()
	{
		static float LastTime = -1.f;

		auto World = GetWorld();

		if (!World)
			return;

		if (BotWorld::LastWorld != World)
		{
			if (BotWorld::LastWorld)
				ResetForNewMatch();

			BotWorld::LastWorld = World;
		}

		if (BotSettings::bPendingRemoveAllBots.exchange(false))
			RemoveAllBots();

		if (AllPlayerBotsToTick.empty())
			return;

		auto GameState = Cast<AFortGameStateAthena>(World->GetGameState());

		if (!GameState)
			return;

		float Now = UGameplayStatics::GetTimeSeconds(World);
		float DeltaTime = LastTime < 0.f ? 0.033f : BotMath::Clamp(Now - LastTime, 0.001f, 0.25f);
		LastTime = Now;

		auto Phase = GameState->GetGamePhase();

		if (Phase != BotWorld::LastPhase)
		{
			if (Phase == EAthenaGamePhase::Aircraft)
			{
				BotWorld::AircraftPhaseStartTime = Now;
				BuildChestCache();
			}

			if (Phase >= EAthenaGamePhase::SafeZones && !BotWorld::bBuiltChestCache)
				BuildChestCache();

			BotWorld::LastPhase = Phase;
		}

		RefreshPawnCache(Now);

		if (Phase >= EAthenaGamePhase::Aircraft)
			RefreshPickupCache(Now);

		for (size_t i = 0; i < AllPlayerBotsToTick.size(); ++i)
		{
			auto Bot = AllPlayerBotsToTick[i];

			if (!Bot || Bot->bBroken)
				continue;

			if (!TickBotGuarded(Bot, Now, DeltaTime))
			{
				LOG_ERROR(LogBots, "Bot {} crashed while thinking and was removed (the server is fine).", Bot->Name);
				Bot->bBroken = true;
			}

			if (Bot->bBroken && Bot->State != EBotState::Dead)
			{
				Bot->State = EBotState::Dead;
				BotSettings::AliveBots--;
			}
		}
	}
}

namespace Bosses
{

}
