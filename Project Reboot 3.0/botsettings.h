#pragma once

// Settings shared between the Reboot GUI (which runs on its own thread) and the bot code
// (which runs on the game thread). Anything that changes the game world must be *requested*
// here by the GUI and then actually done by the game thread in UNetDriver::TickFlushHook.

#include <atomic>
#include <mutex>
#include <string>
#include <fstream>

namespace BotSettings
{
	// Requests from the GUI, consumed on the game thread.
	inline std::atomic<int> PendingBotsToSpawn = 0;
	inline std::atomic<bool> bPendingStartAircraft = false;
	inline std::atomic<bool> bPendingRemoveAllBots = false;

	// How good the bots are, 0 (potato) to 100 (sweaty). Affects aim, reaction time and how often they hit.
	inline int Skill = 50;

	inline bool bFight = true;   // bots shoot at players and other bots
	inline bool bLoot = true;    // bots open chests and pick up weapons/heals
	inline bool bEmote = true;   // bots emote on spawn island

	// Experimental: let Fortnite's own bot AI ("Phoebe") control the bots instead of Reboot's.
	// Needs navigation data that Reboot doesn't set up on most versions, so the bots usually just stand still.
	inline bool bUseEpicAI = false;

	// "Your skin": the skin human players wear in game, by name (e.g. "Travis Scott").
	// Reboot Launcher's login server always sends the default skin, so this overrides it.
	// Saved to stark_skin.txt next to the game so it's remembered.
	inline std::mutex SkinMutex;
	inline std::string PlayerSkinName;
	inline std::atomic<bool> bPendingApplySkin = false;

	inline std::string GetPlayerSkinName()
	{
		std::lock_guard<std::mutex> Lock(SkinMutex);
		return PlayerSkinName;
	}

	inline void SetPlayerSkinName(const std::string& Name)
	{
		{
			std::lock_guard<std::mutex> Lock(SkinMutex);
			PlayerSkinName = Name;
		}

		std::ofstream("stark_skin.txt") << Name;
	}

	inline void LoadPlayerSkinName()
	{
		std::ifstream File("stark_skin.txt");
		std::string Name;
		std::getline(File, Name);
		std::lock_guard<std::mutex> Lock(SkinMutex);
		PlayerSkinName = Name;
	}

	// Read-only stats for the GUI.
	inline std::atomic<int> AliveBots = 0;
}
