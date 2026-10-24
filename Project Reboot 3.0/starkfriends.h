#pragma once

// Project Stark: play with friends without Radmin VPN.
// When you host, we ask your router (UPnP) to forward the game port to this PC, then show your
// public address in the server window. Friends paste it into
// Play -> Multiplayer -> Join a custom server -> Enter IP.
// (Networking code is in starkfriends.cpp so Windows' socket headers don't clash with the rest.)

#include <atomic>
#include <string>

namespace StarkFriends
{
	extern int GamePort;
	extern std::atomic<bool> bWorking;
	extern std::atomic<bool> bOpen;

	std::string GetStatus();
	std::string GetPublicIP();
	void OpenPort();
}

#ifdef IMGUI_VERSION
// Drawn in the Project Stark server window
inline void StarkFriendsUI()
{
	static bool bStarted = false;

	if (!bStarted)
	{
		bStarted = true;
		StarkFriends::OpenPort();
	}

	ImGui::TextWrapped("%s", StarkFriends::GetStatus().c_str());

	auto PublicIP = StarkFriends::GetPublicIP();

	if (StarkFriends::bOpen && !PublicIP.empty())
	{
		std::string Address = StarkFriends::GamePort == 7777 ? PublicIP : PublicIP + ":" + std::to_string(StarkFriends::GamePort);

		ImGui::Text("Your address: %s", Address.c_str());
		ImGui::SameLine();

		if (ImGui::Button("Copy"))
			ImGui::SetClipboardText(Address.c_str());

		ImGui::TextWrapped("Send it to your friends. In Reboot Launcher they go to Play -> Multiplayer -> Join a custom server -> Enter IP, paste it, then Launch.");
	}

	if (!StarkFriends::bWorking && ImGui::Button("Try again"))
		StarkFriends::OpenPort();
}
#endif
