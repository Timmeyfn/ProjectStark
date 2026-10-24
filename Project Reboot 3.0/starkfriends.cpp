// Project Stark: play with friends (see starkfriends.h)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <natupnp.h>
#include <comutil.h>
#include <atomic>
#include <mutex>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comsuppw.lib")

namespace StarkFriends
{
	// definitions of the variables declared in starkfriends.h
	int GamePort = 7777;

	std::mutex Mutex;
	std::string Status = "Not started";
	std::string PublicIP;
	std::string LocalIP;
	std::atomic<bool> bWorking = false;
	std::atomic<bool> bOpen = false;

	inline void SetStatus(const std::string& NewStatus)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Status = NewStatus;
	}

	std::string GetStatus() { std::lock_guard<std::mutex> Lock(Mutex); return Status; }
	std::string GetPublicIP() { std::lock_guard<std::mutex> Lock(Mutex); return PublicIP; }

	// The address of this PC on your home network (the one the router should forward to)
	inline std::string FindLocalIP()
	{
		WSADATA Data;
		WSAStartup(MAKEWORD(2, 2), &Data);

		std::string Result;
		SOCKET Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

		if (Socket != INVALID_SOCKET)
		{
			sockaddr_in Remote{};
			Remote.sin_family = AF_INET;
			Remote.sin_port = htons(53);
			inet_pton(AF_INET, "8.8.8.8", &Remote.sin_addr); // no packet is sent, this just picks the right network card

			if (connect(Socket, (sockaddr*)&Remote, sizeof(Remote)) == 0)
			{
				sockaddr_in Local{};
				int Length = sizeof(Local);

				if (getsockname(Socket, (sockaddr*)&Local, &Length) == 0)
				{
					char Buffer[INET_ADDRSTRLEN] = {};
					inet_ntop(AF_INET, &Local.sin_addr, Buffer, sizeof(Buffer));
					Result = Buffer;
				}
			}

			closesocket(Socket);
		}

		return Result;
	}

	inline bool IsPrivateOrShared(const std::string& IP)
	{
		return IP.starts_with("10.") || IP.starts_with("192.168.") || IP.starts_with("100.") || IP.starts_with("127.")
			|| (IP.starts_with("172.") && atoi(IP.c_str() + 4) >= 16 && atoi(IP.c_str() + 4) <= 31);
	}

	inline DWORD WINAPI OpenPortThread(LPVOID)
	{
		bWorking = true;
		bOpen = false;
		SetStatus("Asking your router to open the game port...");

		CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		auto Local = FindLocalIP();
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			LocalIP = Local;
		}

		IUPnPNAT* Nat = nullptr;
		IStaticPortMappingCollection* Mappings = nullptr;

		if (FAILED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), (void**)&Nat)) || !Nat)
		{
			SetStatus("UPnP isn't available on this PC.");
			bWorking = false;
			return 0;
		}

		// The router can take a few seconds to answer
		for (int Attempt = 0; Attempt < 10 && !Mappings; ++Attempt)
		{
			Nat->get_StaticPortMappingCollection(&Mappings);
			if (!Mappings) Sleep(1000);
		}

		if (!Mappings)
		{
			SetStatus("Your router didn't answer. Turn on UPnP in your router's settings, or forward UDP port " + std::to_string(GamePort) + " to " + Local + " yourself.");
			Nat->Release();
			bWorking = false;
			return 0;
		}

		bool bAnyOpened = false;
		std::string External;

		for (const wchar_t* Protocol : { L"UDP", L"TCP" })
		{
			_bstr_t ProtocolStr(Protocol);
			Mappings->Remove(GamePort, ProtocolStr); // clear an old mapping (maybe to another PC)

			IStaticPortMapping* Mapping = nullptr;
			_bstr_t LocalStr(Local.c_str());
			_bstr_t Description(L"Project Stark");

			if (SUCCEEDED(Mappings->Add(GamePort, ProtocolStr, GamePort, LocalStr, VARIANT_TRUE, Description, &Mapping)) && Mapping)
			{
				bAnyOpened = true;

				BSTR ExternalIP = nullptr;
				if (External.empty() && SUCCEEDED(Mapping->get_ExternalIPAddress(&ExternalIP)) && ExternalIP)
				{
					External = (const char*)_bstr_t(ExternalIP, false);
				}

				Mapping->Release();
			}
		}

		Mappings->Release();
		Nat->Release();

		if (!bAnyOpened)
		{
			SetStatus("Your router said no. Forward UDP port " + std::to_string(GamePort) + " to " + Local + " in your router's settings.");
			bWorking = false;
			return 0;
		}

		{
			std::lock_guard<std::mutex> Lock(Mutex);
			PublicIP = External;
		}

		if (External.empty())
			SetStatus("Port opened, but the router didn't say your public IP. Search \"what is my ip\" and send friends that.");
		else if (IsPrivateOrShared(External))
			SetStatus("Port opened, but your internet provider shares your IP (" + External + "), so friends can't reach you directly. Ask your provider for a public IP, or we'll need the tunnel option.");
		else
		{
			SetStatus("Ready! Friends can join you.");
			bOpen = true;
		}

		bWorking = false;
		return 0;
	}

	void OpenPort()
	{
		if (!bWorking)
			CreateThread(nullptr, 0, OpenPortThread, nullptr, 0, nullptr);
	}
}

