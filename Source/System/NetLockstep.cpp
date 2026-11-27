#include "NetLockstep.h"

#include "MovableMan.h"

#include "DeterminismChecker.h"
#include "InputRecorder.h"
#include "System.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <thread>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace RTE;

bool NetLockstep::s_Active = false;
bool NetLockstep::s_Desynced = false;
bool NetLockstep::s_ApplyingInput = false;
int NetLockstep::s_Socket = -1;
int NetLockstep::s_LocalPlayer = 0;
long long NetLockstep::s_SimUpdate = 0;
long long NetLockstep::s_RemoteCompleteThrough = NetLockstep::c_InputDelay;
long long NetLockstep::s_MatchedHashCount = 0;
std::vector<std::string> NetLockstep::s_PendingLines;
std::map<long long, std::vector<std::string>> NetLockstep::s_InputLines[NetLockstep::c_PlayerCount];
std::map<long long, uint64_t> NetLockstep::s_StateHashes[NetLockstep::c_PlayerCount];
std::string NetLockstep::s_ReceiveBuffer;

namespace {
	/// Reports a failure to set up the connection, and quits. There are no managers around yet to report through.
	[[noreturn]] void FailSetup(const std::string& message) {
		std::cerr << "Lockstep: " << message << std::endl;
		std::exit(EXIT_FAILURE);
	}
} // namespace

void NetLockstep::Host(const std::string& port) {
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	int enable = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(static_cast<uint16_t>(std::atoi(port.c_str())));
	if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0) {
		FailSetup("Failed to listen on port " + port + "!");
	}

	std::cerr << "Lockstep: waiting for the other player to connect to port " << port << "..." << std::endl;
	int connection = accept(listener, nullptr, nullptr);
	close(listener);
	if (connection < 0) {
		FailSetup("Failed to accept the connection of the other player!");
	}
	Start(connection, 0);
}

void NetLockstep::Join(const std::string& address) {
	size_t portStart = address.rfind(':');
	if (portStart == std::string::npos) {
		FailSetup("The address to join has to be given as address:port!");
	}

	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* hostInfo = nullptr;
	if (getaddrinfo(address.substr(0, portStart).c_str(), address.substr(portStart + 1).c_str(), &hints, &hostInfo) != 0 || !hostInfo) {
		FailSetup("Failed to look up the address " + address + "!");
	}

	// The host may not be listening quite yet when both are started at the same time, so keep trying for a while.
	int connection = -1;
	for (int attempt = 0; attempt < 150 && connection < 0; ++attempt) {
		connection = socket(hostInfo->ai_family, hostInfo->ai_socktype, hostInfo->ai_protocol);
		if (connection >= 0 && connect(connection, hostInfo->ai_addr, hostInfo->ai_addrlen) != 0) {
			close(connection);
			connection = -1;
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
	}
	freeaddrinfo(hostInfo);
	if (connection < 0) {
		FailSetup("Failed to connect to " + address + "!");
	}
	Start(connection, 1);
}

void NetLockstep::Start(int socket, int localPlayer) {
	// Sending to a closed connection has to fail instead of killing the game.
	std::signal(SIGPIPE, SIG_IGN);
	// Each update's input has to go out right away, instead of waiting to be bundled with later input.
	int enable = 1;
	setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

	s_Socket = socket;
	s_LocalPlayer = localPlayer;
	s_Active = true;
	std::cerr << "Lockstep: connected, playing as player " << localPlayer + 1 << "." << std::endl;
}

bool NetLockstep::CaptureLocalInput(const SDL_Event& inputEvent) {
	if (!s_Active || s_ApplyingInput) {
		return false;
	}

	// Each player's input goes to a made up device of their own, so that both machines can tell the players apart the same way.
	SDL_Event playerEvent = inputEvent;
	Uint32 deviceID = c_VirtualDeviceBase + s_LocalPlayer;
	switch (inputEvent.type) {
		case SDL_EVENT_KEY_UP:
		case SDL_EVENT_KEY_DOWN:
			// shortcut: Escape pauses into the menus, which the two machines would then get through on their own. Let it through once pausing is synced.
			if (inputEvent.key.scancode == SDL_SCANCODE_ESCAPE) {
				return true;
			}
			playerEvent.key.which = deviceID;
			break;
		case SDL_EVENT_MOUSE_MOTION:
			playerEvent.motion.which = deviceID;
			break;
		case SDL_EVENT_MOUSE_BUTTON_UP:
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			playerEvent.button.which = deviceID;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			playerEvent.wheel.which = deviceID;
			break;
		default:
			// Anything else would only reach this machine's simulation, so it is dropped.
			return true;
	}
	s_PendingLines.emplace_back(InputRecorder::EventToLine(playerEvent));
	return true;
}

void NetLockstep::BeginSimUpdate() {
	if (!s_Active) {
		return;
	}
	++s_SimUpdate;

	// This machine's input since the last update goes to a later update, to give it time to get to the other machine.
	long long inputUpdate = s_SimUpdate + c_InputDelay;
	std::string message;
	for (std::string& line: s_PendingLines) {
		message += std::format("{} {}\n", inputUpdate, line);
		s_InputLines[s_LocalPlayer][inputUpdate].emplace_back(std::move(line));
	}
	s_PendingLines.clear();

	if (s_SimUpdate % c_HashInterval == 0) {
		StateHasher hasher;
		g_MovableMan.HashState(hasher);
		s_StateHashes[s_LocalPlayer][s_SimUpdate] = hasher.GetHash();
		message += std::format("{} h {:016x}\n", s_SimUpdate, hasher.GetHash());
		CompareStateHashes(s_SimUpdate);
	}
	// This tells the other machine that it has all of this machine's input for that update.
	message += std::format("{} .\n", inputUpdate);
	Send(message);

	auto waitStart = std::chrono::steady_clock::now();
	while (s_RemoteCompleteThrough < s_SimUpdate && !s_Desynced) {
		if (s_Socket < 0 || std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(c_WaitTimeoutS)) {
			std::cerr << "Lockstep: the other player is gone, ending at update " << s_SimUpdate << "." << std::endl;
			Disconnect();
			System::SetQuit();
			return;
		}
		Receive(100);
	}
	// Pick up anything else that has arrived, so that state hashes get compared without waiting for the next stall.
	Receive(0);

	s_ApplyingInput = true;
	for (std::map<long long, std::vector<std::string>>& playerLines: s_InputLines) {
		if (auto lines = playerLines.find(s_SimUpdate); lines != playerLines.end()) {
			for (const std::string& line: lines->second) {
				InputRecorder::ApplyLine(line);
			}
			playerLines.erase(lines);
		}
	}
	s_ApplyingInput = false;
}

void NetLockstep::Stop() {
	if (!s_Active) {
		return;
	}
	if (s_Socket >= 0) {
		// Closing with input from the other machine left unread could throw away what this machine sent last, so tell the other machine this one is done and read until it is too.
		shutdown(s_Socket, SHUT_WR);
		auto waitStart = std::chrono::steady_clock::now();
		while (s_Socket >= 0 && std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(5)) {
			Receive(100);
		}
		Disconnect();
	}
	std::cerr << "Lockstep: ran " << s_SimUpdate << " updates, " << s_MatchedHashCount << " state hashes matched" << (s_Desynced ? ", and then the machines DESYNCED." : ".") << std::endl;
	s_Active = false;
}

void NetLockstep::Send(const std::string& text) {
	size_t sent = 0;
	while (s_Socket >= 0 && sent < text.size()) {
		ssize_t result = send(s_Socket, text.data() + sent, text.size() - sent, 0);
		if (result <= 0) {
			Disconnect();
			return;
		}
		sent += static_cast<size_t>(result);
	}
}

void NetLockstep::Receive(int timeoutMS) {
	if (s_Socket < 0) {
		return;
	}
	pollfd pollInfo{s_Socket, POLLIN, 0};
	if (poll(&pollInfo, 1, timeoutMS) <= 0) {
		return;
	}
	char buffer[4096];
	ssize_t received = recv(s_Socket, buffer, sizeof(buffer), 0);
	if (received <= 0) {
		Disconnect();
		return;
	}
	s_ReceiveBuffer.append(buffer, static_cast<size_t>(received));

	size_t lineStart = 0;
	for (size_t lineEnd = s_ReceiveBuffer.find('\n'); lineEnd != std::string::npos; lineEnd = s_ReceiveBuffer.find('\n', lineStart)) {
		HandleRemoteLine(s_ReceiveBuffer.substr(lineStart, lineEnd - lineStart));
		lineStart = lineEnd + 1;
	}
	s_ReceiveBuffer.erase(0, lineStart);
}

void NetLockstep::HandleRemoteLine(const std::string& line) {
	size_t updateEnd = line.find(' ');
	if (updateEnd == std::string::npos || updateEnd + 1 >= line.size()) {
		return;
	}
	long long simUpdate = std::atoll(line.c_str());
	std::string content = line.substr(updateEnd + 1);
	int remotePlayer = 1 - s_LocalPlayer;

	// The other machine is not to be trusted with how much gets kept around, or with which kinds of input get applied.
	bool updateIsNear = simUpdate > s_SimUpdate - c_HashInterval * 4 && simUpdate < s_SimUpdate + c_HashInterval * 4;
	if (content == ".") {
		if (updateIsNear) {
			s_RemoteCompleteThrough = std::max(s_RemoteCompleteThrough, simUpdate);
		}
	} else if (content.starts_with("h ")) {
		if (updateIsNear) {
			s_StateHashes[remotePlayer][simUpdate] = std::strtoull(content.c_str() + 2, nullptr, 16);
			CompareStateHashes(simUpdate);
		}
	} else if (updateIsNear && simUpdate >= s_SimUpdate && std::string_view("kmbw").find(content[0]) != std::string_view::npos && s_InputLines[remotePlayer][simUpdate].size() < 1000) {
		s_InputLines[remotePlayer][simUpdate].emplace_back(std::move(content));
	}
}

void NetLockstep::CompareStateHashes(long long simUpdate) {
	auto localHash = s_StateHashes[s_LocalPlayer].find(simUpdate);
	auto remoteHash = s_StateHashes[1 - s_LocalPlayer].find(simUpdate);
	if (localHash == s_StateHashes[s_LocalPlayer].end() || remoteHash == s_StateHashes[1 - s_LocalPlayer].end()) {
		return;
	}
	if (localHash->second == remoteHash->second) {
		++s_MatchedHashCount;
	} else if (!s_Desynced) {
		s_Desynced = true;
		std::cerr << std::format("Lockstep: DESYNC! The machines stopped matching by update {}. This one has {:016x}, the other one has {:016x}.", simUpdate, localHash->second, remoteHash->second) << std::endl;
		System::SetQuit();
	}
	s_StateHashes[s_LocalPlayer].erase(localHash);
	s_StateHashes[1 - s_LocalPlayer].erase(remoteHash);
}

void NetLockstep::Disconnect() {
	if (s_Socket >= 0) {
		close(s_Socket);
		s_Socket = -1;
	}
}
