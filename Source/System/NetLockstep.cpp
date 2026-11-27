#include "NetLockstep.h"

#include "MovableMan.h"
#include "TimerMan.h"

#include "DeterminismChecker.h"
#include "InputRecorder.h"
#include "System.h"

#include <chrono>
#include <cmath>
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
long long NetLockstep::s_RemoteCompleteThrough = 0;
std::string NetLockstep::s_HostPort;
std::string NetLockstep::s_JoinAddress;
long long NetLockstep::s_InputDelay = 3;
bool NetLockstep::s_InputDelayIsKnown = false;
bool NetLockstep::s_InputDelayIsFixed = false;
bool NetLockstep::s_LeaveInputUpdateOpen = false;
long long NetLockstep::s_LastInputDelayChange = 0;
long long NetLockstep::s_StallAllowance = 0;
long long NetLockstep::s_LastStallAllowanceChange = 0;
long long NetLockstep::s_LastStallNotice = 0;
std::deque<double> NetLockstep::s_PingSamplesMS;
int NetLockstep::s_ArtificialLatencyMS = 0;
std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>> NetLockstep::s_HeldBackMessages;
std::recursive_mutex NetLockstep::s_SendMutex;
std::thread NetLockstep::s_SenderThread;
std::atomic<bool> NetLockstep::s_SenderThreadRunning = false;
long long NetLockstep::s_StalledUpdateCount = 0;
double NetLockstep::s_StalledMS = 0;
long long NetLockstep::s_MatchedHashCount = 0;
std::vector<std::string> NetLockstep::s_PendingLines;
std::map<long long, std::vector<std::string>> NetLockstep::s_InputLines[NetLockstep::c_PlayerCount];
std::map<long long, uint64_t> NetLockstep::s_StateHashes[NetLockstep::c_PlayerCount];
std::string NetLockstep::s_ReceiveBuffer;

namespace {
	/// Gets the time on this machine's clock, in microseconds.
	long long GetClockUS() {
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	/// Reports a failure to set up the connection, and quits. There are no managers around yet to report through.
	[[noreturn]] void FailSetup(const std::string& message) {
		std::cerr << "Lockstep: " << message << std::endl;
		std::exit(EXIT_FAILURE);
	}
} // namespace

void NetLockstep::Connect() {
	if (!s_HostPort.empty()) {
		Host();
		// The first update that input can be for is the one after the delay, so there is none to wait for before that.
		s_RemoteCompleteThrough = s_InputDelay;
		SendNow(std::format("0 d {}\n", s_InputDelay));
	} else if (!s_JoinAddress.empty()) {
		Join();
		auto waitStart = std::chrono::steady_clock::now();
		while (!s_InputDelayIsKnown) {
			if (s_Socket < 0 || std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(10)) {
				FailSetup("The host didn't say which input delay to use!");
			}
			Receive(100);
		}
	} else {
		return;
	}
	std::cerr << "Lockstep: input is delayed by " << s_InputDelay << " updates" << (s_InputDelayIsFixed ? "" : " to begin with") << ", and what this machine sends by another " << s_ArtificialLatencyMS << " ms." << std::endl;
}

void NetLockstep::Host() {
	const std::string& port = s_HostPort;
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

void NetLockstep::Join() {
	const std::string& address = s_JoinAddress;
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
	if (s_ArtificialLatencyMS > 0) {
		s_SenderThreadRunning = true;
		s_SenderThread = std::thread([]() {
			while (s_SenderThreadRunning) {
				SendHeldBackMessages();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
	}
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
	SendHeldBackMessages();

	AdaptInputDelay();

	// This machine's input since the last update goes to a later update, to give it time to get to the other machine.
	long long inputUpdate = s_SimUpdate + s_InputDelay;
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
	if (s_LeaveInputUpdateOpen) {
		// The next update's input goes to this same update now, which is what makes the delay one shorter.
		s_LeaveInputUpdateOpen = false;
		--s_InputDelay;
		s_LastInputDelayChange = s_SimUpdate;
	} else {
		// This tells the other machine that it has all of this machine's input for that update.
		message += std::format("{} .\n", inputUpdate);
	}
	Send(message);

	auto waitStart = std::chrono::steady_clock::now();
	while (s_RemoteCompleteThrough < s_SimUpdate && !s_Desynced) {
		if (s_Socket < 0 || std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(c_WaitTimeoutS)) {
			std::cerr << "Lockstep: the other player is gone, ending at update " << s_SimUpdate << "." << std::endl;
			Disconnect();
			System::SetQuit();
			return;
		}
		SendHeldBackMessages();
		// The other machine may be waiting on what is being held back here, so that has to keep going out on time.
		Receive(s_HeldBackMessages.empty() ? 100 : 2);
	}
	// A wait of less than half an update is just this machine keeping in step with the other one, and doesn't hold anything up.
	if (double waitedMS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count(); waitedMS > g_TimerMan.GetDeltaTimeMS() * 0.5) {
		++s_StalledUpdateCount;
		s_StalledMS += waitedMS;
		// The other machine's input is getting here too late, so ask it to hold its input back for longer. The start is left out, since the machines are still falling into step then.
		if (s_SimUpdate > c_ShrinkInterval && s_SimUpdate - s_LastStallNotice >= c_PingInterval) {
			s_LastStallNotice = s_SimUpdate;
			Send(std::format("{} g\n", s_SimUpdate));
		}
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

std::string NetLockstep::GetStatusText() {
	if (s_PingSamplesMS.empty()) {
		return std::format("Input delay {:.0f} ms", static_cast<double>(s_InputDelay) * g_TimerMan.GetDeltaTimeMS());
	}
	// Each machine only reads what has arrived once per update, which adds an update to the measurements on average that isn't the connection's doing.
	double averagePingMS = 0.0;
	for (double pingSampleMS: s_PingSamplesMS) {
		averagePingMS += pingSampleMS / static_cast<double>(s_PingSamplesMS.size());
	}
	return std::format("Ping {:.0f} ms, input delay {:.0f} ms", std::max(0.0, averagePingMS - g_TimerMan.GetDeltaTimeMS()), static_cast<double>(s_InputDelay) * g_TimerMan.GetDeltaTimeMS());
}

void NetLockstep::AdaptInputDelay() {
	// The ping is measured even when the delay is fixed, since it is shown to the player.
	if (s_SimUpdate > c_PingInterval && s_SimUpdate % c_PingInterval == 0) {
		Send(std::format("{} p {}\n", s_SimUpdate, GetClockUS()));
	}
	if (s_InputDelayIsFixed) {
		return;
	}
	if (s_StallAllowance > 0 && s_SimUpdate - s_LastStallAllowanceChange > c_StallAllowanceDuration) {
		--s_StallAllowance;
		s_LastStallAllowanceChange = s_SimUpdate;
	}
	if (s_PingSamplesMS.empty()) {
		return;
	}

	// Input has to get there in half the time a message takes to go there and back, with a little to spare.
	// Each machine only reads what has arrived once per update, unless it is waiting for it, so the worst measurement has up to an update on either end that waiting input wouldn't have.
	double worstPingMS = *std::max_element(s_PingSamplesMS.begin(), s_PingSamplesMS.end());
	double connectionPingMS = std::max(0.0, worstPingMS - 2.0 * g_TimerMan.GetDeltaTimeMS());
	long long wantedDelay = static_cast<long long>(std::ceil((connectionPingMS * 0.5 + 4.0) / g_TimerMan.GetDeltaTimeMS())) + s_StallAllowance;
	wantedDelay = std::clamp(wantedDelay, 1LL, c_MaxInputDelay);

	if (wantedDelay > s_InputDelay) {
		// Growing leaves some updates without input from this machine, which is fine, so it can be done right away.
		s_InputDelay = wantedDelay;
		s_LastInputDelayChange = s_SimUpdate;
	} else if (wantedDelay < s_InputDelay && s_SimUpdate - s_LastInputDelayChange >= c_ShrinkInterval) {
		s_LeaveInputUpdateOpen = true;
	}
}

void NetLockstep::Stop() {
	if (!s_Active) {
		return;
	}
	// The thread has to be joined whether or not there is still a connection, or the program can't exit properly.
	if (s_SenderThread.joinable()) {
		s_SenderThreadRunning = false;
		s_SenderThread.join();
	}
	if (s_Socket >= 0) {
		SendHeldBackMessages(true);
		// Closing with input from the other machine left unread could throw away what this machine sent last, so tell the other machine this one is done and read until it is too.
		shutdown(s_Socket, SHUT_WR);
		auto waitStart = std::chrono::steady_clock::now();
		while (s_Socket >= 0 && std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(5)) {
			Receive(100);
		}
		Disconnect();
	}
	std::cerr << "Lockstep: ran " << s_SimUpdate << " updates, " << s_MatchedHashCount << " state hashes matched" << (s_Desynced ? ", and then the machines DESYNCED." : ".") << std::endl;
	std::cerr << std::format("Lockstep: {} updates waited for the other player's input, for {:.0f} ms in total.", s_StalledUpdateCount, s_StalledMS) << std::endl;
	std::cerr << std::format("Lockstep: input delay ended at {} updates, with a ping of about {:.0f} ms.", s_InputDelay, s_PingSamplesMS.empty() ? 0.0 : s_PingSamplesMS.back()) << std::endl;
	s_Active = false;
}

void NetLockstep::Send(const std::string& text) {
	if (s_ArtificialLatencyMS > 0) {
		std::lock_guard<std::recursive_mutex> lock(s_SendMutex);
		s_HeldBackMessages.emplace_back(std::chrono::steady_clock::now() + std::chrono::milliseconds(s_ArtificialLatencyMS), text);
	} else {
		SendNow(text);
	}
}

void NetLockstep::SendHeldBackMessages(bool everything) {
	std::lock_guard<std::recursive_mutex> lock(s_SendMutex);
	while (!s_HeldBackMessages.empty() && (everything || s_HeldBackMessages.front().first <= std::chrono::steady_clock::now())) {
		SendNow(s_HeldBackMessages.front().second);
		s_HeldBackMessages.pop_front();
	}
}

void NetLockstep::SendNow(const std::string& text) {
	std::lock_guard<std::recursive_mutex> lock(s_SendMutex);
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
	if (content.starts_with("d ")) {
		// Only the joiner takes the input delay, and only before the first update.
		if (s_LocalPlayer == 1 && s_SimUpdate == 0 && !s_InputDelayIsKnown) {
			s_InputDelay = std::clamp(std::atoll(content.c_str() + 2), 1LL, 60LL);
			s_RemoteCompleteThrough = s_InputDelay;
			s_InputDelayIsKnown = true;
		}
	} else if (content.starts_with("p ")) {
		// Send the other machine's clock reading straight back, for it to tell how long that took.
		if (updateIsNear) {
			Send(std::format("{} P {}\n", s_SimUpdate, std::atoll(content.c_str() + 2)));
		}
	} else if (content.starts_with("P ")) {
		double pingMS = static_cast<double>(GetClockUS() - std::atoll(content.c_str() + 2)) / 1000.0;
		// A measurement this far off is the other machine having been busy with something like loading, and says nothing about the connection.
		if (pingMS >= 0.0 && pingMS < 1000.0) {
			s_PingSamplesMS.push_back(pingMS);
			if (s_PingSamplesMS.size() > c_PingSampleCount) {
				s_PingSamplesMS.pop_front();
			}
		}
	} else if (content == "g") {
		if (updateIsNear && !s_InputDelayIsFixed && s_StallAllowance < 6) {
			++s_StallAllowance;
			s_LastStallAllowanceChange = s_SimUpdate;
		}
	} else if (content == ".") {
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
	std::lock_guard<std::recursive_mutex> lock(s_SendMutex);
	if (s_Socket >= 0) {
		close(s_Socket);
		s_Socket = -1;
	}
}
