#pragma once

// Settings shared between the Reboot GUI (which runs on its own thread) and the bot code
// (which runs on the game thread). Anything that changes the game world must be *requested*
// here by the GUI and then actually done by the game thread in UNetDriver::TickFlushHook.

#include <atomic>

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

	// Read-only stats for the GUI.
	inline std::atomic<int> AliveBots = 0;
}
