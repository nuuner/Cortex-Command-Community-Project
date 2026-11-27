#include "NetLockstep.h"

#include "MovableMan.h"
#include "TimerMan.h"

#include "DeterminismChecker.h"
#include "InputRecorder.h"
#include "RTETools.h"
#include "System.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <format>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace RTE;

std::string NetLockstep::s_HostPort;
std::string NetLockstep::s_JoinAddress;
long long NetLockstep::s_InputDelay = 3;
bool NetLockstep::s_InputDelayIsFixed = false;
int NetLockstep::s_ArtificialLatencyMS = 0;
int NetLockstep::s_ArtificialLossPercent = 0;
bool NetLockstep::s_Active = false;
bool NetLockstep::s_Desynced = false;
int NetLockstep::s_LocalPlayer = 0;

namespace {
	using Clock = std::chrono::steady_clock;

	constexpr size_t c_MaxMessageSize = 1000; //!< How long a message can get before the rest of the input is left for the next one, in bytes. Longer ones wouldn't fit in a single packet.
	constexpr int c_PlayerCount = 2; //!< How many machines, and so players, there are.
	constexpr long long c_HashInterval = 30; //!< How many simulation updates there are between the state hashes that get compared.
	constexpr size_t c_ResentHashCount = 3; //!< How many of the latest state hashes each message repeats.
	constexpr long long c_PingInterval = 30; //!< How many simulation updates there are between measurements of how long the connection takes.
	constexpr size_t c_PingSampleCount = 10; //!< How many of the latest measurements the input delay goes by. It goes by the worst of them.
	constexpr long long c_ShrinkInterval = 60; //!< The fewest simulation updates there can be between changes of the input delay when it is shrinking.
	constexpr long long c_StallAllowanceDuration = 600; //!< How many simulation updates each extra update of input delay that the other machine asked for is kept for.
	constexpr long long c_MaxStallAllowance = 6; //!< The most simulation updates of input delay that the other machine can ask to have added.
	constexpr long long c_MaxUpdatesAhead = 120; //!< How many simulation updates ahead of this machine the other one is believed about.
	constexpr int c_ResendIntervalMS = 20; //!< How long to wait for the other machine's input before repeating what this machine last sent, in case that got lost.
	constexpr int c_WaitTimeoutS = 120; //!< How long to wait for the other machine's first input before giving up on it. It has to cover the other machine loading the game.
	constexpr int c_SilenceTimeoutS = 10; //!< How long the other machine can go without being heard from, once it has been, before it is taken to be gone.

	int s_Socket = -1; //!< The UDP socket that is connected to the other machine.
	Clock::time_point s_LastHeardFrom; //!< When something last arrived from the other machine.
	bool s_PeerIsGone = false; //!< Whether the other machine has disconnected.
	bool s_ApplyingInput = false; //!< Whether input is being handed back to UInputMan, and so shouldn't be captured.

	long long s_SimUpdate = 0; //!< How many simulation updates have begun.
	long long s_LocalCompleteThrough = 0; //!< The last simulation update that all of this machine's input has been decided for.
	long long s_RemoteAckedThrough = 0; //!< The last simulation update that the other machine has confirmed having all of this machine's input for.
	long long s_RemoteCompleteThrough = 0; //!< The last simulation update that all of the other machine's input has arrived for.
	std::vector<std::string> s_PendingLines; //!< This machine's input since the last simulation update began.
	std::map<long long, std::vector<std::string>> s_UnconfirmedLines; //!< This machine's input that the other machine hasn't confirmed having, by the simulation update it gets applied in.
	std::map<long long, std::vector<std::string>> s_InputLines[c_PlayerCount]; //!< The input of each player that is yet to be applied, by the simulation update it gets applied in.

	std::map<long long, uint64_t> s_StateHashes[c_PlayerCount]; //!< The state hashes of each machine that haven't been compared yet, by simulation update.
	std::deque<std::pair<long long, uint64_t>> s_LatestStateHashes; //!< The latest state hashes of this machine, which each message repeats.
	long long s_LastComparedHashUpdate = 0; //!< The latest simulation update that state hashes have been compared for.
	long long s_MatchedHashCount = 0; //!< How many state hashes have been compared and matched.

	bool s_LeaveInputUpdateOpen = false; //!< Whether the input delay shrinks by one after this simulation update. The next update's input then goes to the same update as this one's, so that update isn't complete yet.
	long long s_LastInputDelayChange = 0; //!< The simulation update that the input delay last changed in.
	long long s_StallAllowance = 0; //!< How many simulation updates of input delay are added because the other machine had to wait for this one's input.
	long long s_LastStallAllowanceChange = 0; //!< The simulation update that the stall allowance last changed in.
	long long s_LastStallNotice = 0; //!< The simulation update that this machine last told the other one that it had to wait in.
	std::deque<double> s_PingSamplesMS; //!< The latest measurements of how long a message takes to get to the other machine and back, in milliseconds.

	long long s_StalledUpdateCount = 0; //!< How many simulation updates had to wait for the other machine's input.
	double s_StalledMS = 0; //!< How long simulation updates have waited for the other machine's input in total, in milliseconds.
	long long s_SentMessageCount = 0; //!< How many messages this machine has sent.
	long long s_LostMessageCount = 0; //!< How many messages the artificial loss has thrown away.

	std::mutex s_HeldBackMutex; //!< Guards what is being held back, which the sender thread shares with the main thread.
	std::deque<std::pair<Clock::time_point, std::string>> s_HeldBackMessages; //!< What is being held back for the artificial latency, with when each is to be sent.
	std::thread s_SenderThread; //!< Sends what is being held back when its time comes, so that the artificial latency doesn't also wait for the next simulation update.
	std::atomic<bool> s_SenderThreadRunning = false; //!< Whether the sender thread is to keep going.
	std::mt19937 s_LossRandom{std::random_device{}()}; //!< Decides which messages the artificial loss throws away. It has nothing to do with the simulation's random numbers.

	/// Gets the time on this machine's clock, in microseconds.
	long long GetClockUS() {
		return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
	}

	/// Reports a failure to set up the connection, and quits. There are no managers around yet to report through.
	[[noreturn]] void FailSetup(const std::string& message) {
		std::cerr << "Lockstep: " << message << std::endl;
		std::exit(EXIT_FAILURE);
	}

	/// Sends a message right away. A lost message is made up for by the next, so whether it got there isn't checked.
	void SendNow(const std::string& text) {
		if (s_Socket >= 0) {
			send(s_Socket, text.data(), text.size(), 0);
		}
	}

	/// Sends what has been held back for long enough, or all of it.
	void SendHeldBackMessages(bool everything = false) {
		std::lock_guard<std::mutex> lock(s_HeldBackMutex);
		while (!s_HeldBackMessages.empty() && (everything || s_HeldBackMessages.front().first <= Clock::now())) {
			SendNow(s_HeldBackMessages.front().second);
			s_HeldBackMessages.pop_front();
		}
	}
} // namespace

void NetLockstep::Connect() {
	if (s_HostPort.empty() && s_JoinAddress.empty()) {
		return;
	}
	s_Socket = socket(AF_INET, SOCK_DGRAM, 0);
	char buffer[2048];

	if (!s_HostPort.empty()) {
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		address.sin_port = htons(static_cast<uint16_t>(std::atoi(s_HostPort.c_str())));
		if (s_Socket < 0 || bind(s_Socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
			FailSetup("Failed to listen on port " + s_HostPort + "!");
		}
		s_LocalPlayer = 0;
		std::cerr << "Lockstep: waiting for the other player to connect to port " << s_HostPort << "..." << std::endl;

		// Whoever says hello first is the other player, and nothing from anywhere else gets through after that.
		sockaddr_storage joinerAddress{};
		socklen_t joinerAddressSize = sizeof(joinerAddress);
		for (bool greeted = false; !greeted;) {
			joinerAddressSize = sizeof(joinerAddress);
			ssize_t received = recvfrom(s_Socket, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&joinerAddress), &joinerAddressSize);
			greeted = received > 0 && std::string_view(buffer, static_cast<size_t>(received)) == "hello";
		}
		if (connect(s_Socket, reinterpret_cast<sockaddr*>(&joinerAddress), joinerAddressSize) != 0) {
			FailSetup("Failed to connect back to the other player!");
		}
		SendNow("welcome");
	} else {
		size_t portStart = s_JoinAddress.rfind(':');
		if (portStart == std::string::npos) {
			FailSetup("The address to join has to be given as address:port!");
		}
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		addrinfo* hostInfo = nullptr;
		if (s_Socket < 0 || getaddrinfo(s_JoinAddress.substr(0, portStart).c_str(), s_JoinAddress.substr(portStart + 1).c_str(), &hints, &hostInfo) != 0 || !hostInfo || connect(s_Socket, hostInfo->ai_addr, hostInfo->ai_addrlen) != 0) {
			FailSetup("Failed to look up the address " + s_JoinAddress + "!");
		}
		freeaddrinfo(hostInfo);
		s_LocalPlayer = 1;

		// The host may not be listening quite yet when both are started at the same time, and the hello or its answer can get lost, so keep saying it for a while.
		auto connectStart = Clock::now();
		for (bool welcomed = false; !welcomed;) {
			if (Clock::now() - connectStart > std::chrono::seconds(30)) {
				FailSetup("Failed to connect to " + s_JoinAddress + "!");
			}
			SendNow("hello");
			pollfd pollInfo{s_Socket, POLLIN, 0};
			if (poll(&pollInfo, 1, 200) > 0) {
				ssize_t received = recv(s_Socket, buffer, sizeof(buffer), 0);
				welcomed = received > 0 && std::string_view(buffer, static_cast<size_t>(received)) == "welcome";
			}
		}
	}
	s_LastHeardFrom = Clock::now();

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
	std::cerr << "Lockstep: connected, playing as player " << s_LocalPlayer + 1 << "." << std::endl;
	std::cerr << "Lockstep: input is delayed by " << s_InputDelay << " updates" << (s_InputDelayIsFixed ? "" : " to begin with") << ". What this machine sends is held back by " << s_ArtificialLatencyMS << " ms, and " << s_ArtificialLossPercent << "% of it is thrown away." << std::endl;
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

bool NetLockstep::CaptureLocalInput(const SDL_Event& inputEvent) {
	if (!s_Active || s_ApplyingInput) {
		return false;
	}

	// Each player's input goes to a made up device of their own, so that both machines can tell the players apart the same way.
	SDL_Event playerEvent = inputEvent;
	Uint32 deviceID = GetPlayerDeviceID(s_LocalPlayer);
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
	std::string extraLines = AdaptInputDelay();

	// This machine's input since the last update goes to a later update, to give it time to get to the other machine. It can't go to an update that was already said to be complete.
	long long inputUpdate = std::max(s_SimUpdate + s_InputDelay, s_LocalCompleteThrough + 1);
	for (std::string& line: s_PendingLines) {
		s_UnconfirmedLines[inputUpdate].emplace_back(line);
		s_InputLines[s_LocalPlayer][inputUpdate].emplace_back(std::move(line));
	}
	s_PendingLines.clear();

	if (s_SimUpdate % c_HashInterval == 0) {
		StateHasher hasher;
		g_MovableMan.HashState(hasher);
		// The objects can stay the same for a good while after the random numbers have stopped matching, so those are compared too.
		hasher.Add(g_RandomGenerator.PeekNext());
		s_StateHashes[s_LocalPlayer][s_SimUpdate] = hasher.GetHash();
		s_LatestStateHashes.emplace_back(s_SimUpdate, hasher.GetHash());
		if (s_LatestStateHashes.size() > c_ResentHashCount) {
			s_LatestStateHashes.pop_front();
		}
		CompareStateHashes(s_SimUpdate);
	}

	if (s_LeaveInputUpdateOpen) {
		// The next update's input goes to this same update now, which is what makes the delay one shorter.
		s_LeaveInputUpdateOpen = false;
		--s_InputDelay;
		s_LastInputDelayChange = s_SimUpdate;
	} else {
		s_LocalCompleteThrough = inputUpdate;
	}
	SendState(extraLines);

	auto waitStart = Clock::now();
	auto lastSend = waitStart;
	Receive();
	while (s_RemoteCompleteThrough < s_SimUpdate && !s_Desynced) {
		// The other machine can't be heard from while it loads the game, which is why it gets longer before it has been.
		bool silentForTooLong = Clock::now() - std::max(waitStart, s_LastHeardFrom) > std::chrono::seconds(s_RemoteCompleteThrough > 0 ? c_SilenceTimeoutS : c_WaitTimeoutS);
		if (s_PeerIsGone || silentForTooLong) {
			std::cerr << "Lockstep: the other player is gone, ending at update " << s_SimUpdate << "." << std::endl;
			s_PeerIsGone = true;
			System::SetQuit();
			return;
		}
		// Both machines could be waiting on a message of the other's that got lost, so what the other one may be missing is sent again every so often.
		if (Clock::now() - lastSend > std::chrono::milliseconds(c_ResendIntervalMS)) {
			SendState();
			lastSend = Clock::now();
		}
		pollfd pollInfo{s_Socket, POLLIN, 0};
		poll(&pollInfo, 1, 1);
		Receive();
	}
	// A wait of less than half an update is just this machine keeping in step with the other one, and doesn't hold anything up.
	if (double waitedMS = std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count(); waitedMS > g_TimerMan.GetDeltaTimeMS() * 0.5) {
		++s_StalledUpdateCount;
		s_StalledMS += waitedMS;
		// The other machine's input is getting here too late, so ask it to hold its input back for longer. The start is left out, since the machines are still falling into step then.
		if (s_SimUpdate > c_ShrinkInterval && s_SimUpdate - s_LastStallNotice >= c_PingInterval) {
			s_LastStallNotice = s_SimUpdate;
			SendToPeer(std::format("{} g\n", s_SimUpdate));
		}
	}

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

std::string NetLockstep::AdaptInputDelay() {
	// The ping is measured even when the delay is fixed, since it is shown to the player.
	std::string pingLine;
	if (s_SimUpdate > c_PingInterval && s_SimUpdate % c_PingInterval == 0) {
		pingLine = std::format("{} p {}\n", s_SimUpdate, GetClockUS());
	}
	if (s_InputDelayIsFixed) {
		return pingLine;
	}
	if (s_StallAllowance > 0 && s_SimUpdate - s_LastStallAllowanceChange > c_StallAllowanceDuration) {
		--s_StallAllowance;
		s_LastStallAllowanceChange = s_SimUpdate;
	}
	if (s_PingSamplesMS.empty()) {
		return pingLine;
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
	return pingLine;
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
		// The other machine may still need this machine's last input to get as far as this one did, so it is sent a few more times in case it gets lost, and so is the goodbye.
		for (int repeat = 0; repeat < 3 && !s_PeerIsGone; ++repeat) {
			SendState();
			SendHeldBackMessages(true);
			SendNow("bye");
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		close(s_Socket);
		s_Socket = -1;
	}
	std::cerr << "Lockstep: ran " << s_SimUpdate << " updates, " << s_MatchedHashCount << " state hashes matched" << (s_Desynced ? ", and then the machines DESYNCED." : ".") << std::endl;
	std::cerr << std::format("Lockstep: {} updates waited for the other player's input, for {:.0f} ms in total.", s_StalledUpdateCount, s_StalledMS) << std::endl;
	std::cerr << std::format("Lockstep: input delay ended at {} updates, with a ping of about {:.0f} ms. {} of {} messages were thrown away.", s_InputDelay, s_PingSamplesMS.empty() ? 0.0 : s_PingSamplesMS.back(), s_LostMessageCount, s_SentMessageCount) << std::endl;
	s_Active = false;
}

void NetLockstep::SendState(const std::string& extraLines) {
	// How far this machine has the other one's input, so that the other one can stop repeating it.
	std::string message = std::format("{} a\n", s_RemoteCompleteThrough);

	// All of this machine's input that the other one may not have, oldest first. What doesn't fit goes in a later message, once the other machine has confirmed having some of this.
	long long sentThrough = s_RemoteAckedThrough;
	for (long long simUpdate = s_RemoteAckedThrough + 1; simUpdate <= s_LocalCompleteThrough; ++simUpdate) {
		std::string updateLines;
		if (auto lines = s_UnconfirmedLines.find(simUpdate); lines != s_UnconfirmedLines.end()) {
			for (const std::string& line: lines->second) {
				updateLines += std::format("{} {}\n", simUpdate, line);
			}
		}
		if (message.size() + updateLines.size() > c_MaxMessageSize && sentThrough > s_RemoteAckedThrough) {
			break;
		}
		message += updateLines;
		sentThrough = simUpdate;
	}
	if (sentThrough > s_RemoteAckedThrough) {
		// This tells the other machine that it now has all of this machine's input through that update.
		message += std::format("{} .\n", sentThrough);
	}

	for (const auto& [simUpdate, hash]: s_LatestStateHashes) {
		message += std::format("{} h {:016x}\n", simUpdate, hash);
	}
	SendToPeer(message + extraLines);
}

void NetLockstep::SendToPeer(const std::string& text) {
	if (s_Socket < 0 || s_PeerIsGone) {
		return;
	}
	++s_SentMessageCount;
	if (s_ArtificialLossPercent > 0 && static_cast<int>(s_LossRandom() % 100) < s_ArtificialLossPercent) {
		++s_LostMessageCount;
		return;
	}
	if (s_ArtificialLatencyMS > 0) {
		std::lock_guard<std::mutex> lock(s_HeldBackMutex);
		s_HeldBackMessages.emplace_back(Clock::now() + std::chrono::milliseconds(s_ArtificialLatencyMS), text);
	} else {
		SendNow(text);
	}
}

void NetLockstep::Receive() {
	char buffer[2048];
	while (s_Socket >= 0) {
		ssize_t received = recv(s_Socket, buffer, sizeof(buffer), MSG_DONTWAIT);
		if (received <= 0) {
			break;
		}
		s_LastHeardFrom = Clock::now();
		std::string text(buffer, static_cast<size_t>(received));
		if (text == "hello") {
			// The answer to the joiner's hello got lost, so it is still asking.
			SendNow("welcome");
		} else if (text == "bye") {
			s_PeerIsGone = true;
		} else if (text != "welcome") {
			HandleMessage(text);
		}
	}
}

void NetLockstep::HandleMessage(const std::string& text) {
	int remotePlayer = 1 - s_LocalPlayer;
	// The input lines of this message, which only count once the message says that they are all there is for their updates.
	std::map<long long, std::vector<std::string>> messageInput;

	size_t lineStart = 0;
	for (size_t lineEnd = text.find('\n'); lineEnd != std::string::npos; lineStart = lineEnd + 1, lineEnd = text.find('\n', lineStart)) {
		std::string line = text.substr(lineStart, lineEnd - lineStart);
		size_t updateEnd = line.find(' ');
		if (updateEnd == std::string::npos || updateEnd + 1 >= line.size()) {
			continue;
		}
		long long simUpdate = std::atoll(line.c_str());
		std::string content = line.substr(updateEnd + 1);
		// The other machine is not to be trusted with how much gets kept around, or with which kinds of input get applied.
		bool updateIsNear = simUpdate > s_SimUpdate - c_MaxUpdatesAhead && simUpdate < s_SimUpdate + c_MaxUpdatesAhead;

		if (content == "a") {
			if (simUpdate > s_RemoteAckedThrough && simUpdate <= s_LocalCompleteThrough) {
				s_RemoteAckedThrough = simUpdate;
				s_UnconfirmedLines.erase(s_UnconfirmedLines.begin(), s_UnconfirmedLines.upper_bound(simUpdate));
			}
		} else if (content == ".") {
			// Messages can arrive late or twice, so only what is new in this one is taken from it.
			if (updateIsNear && simUpdate > s_RemoteCompleteThrough) {
				for (auto& [inputUpdate, lines]: messageInput) {
					if (inputUpdate > s_RemoteCompleteThrough && inputUpdate <= simUpdate) {
						s_InputLines[remotePlayer][inputUpdate] = std::move(lines);
					}
				}
				s_RemoteCompleteThrough = simUpdate;
			}
		} else if (content.starts_with("h ")) {
			if (updateIsNear && simUpdate > s_LastComparedHashUpdate) {
				s_StateHashes[remotePlayer][simUpdate] = std::strtoull(content.c_str() + 2, nullptr, 16);
				CompareStateHashes(simUpdate);
			}
		} else if (content.starts_with("p ")) {
			// Send the other machine's clock reading straight back, for it to tell how long that took.
			SendToPeer(std::format("{} P {}\n", s_SimUpdate, std::atoll(content.c_str() + 2)));
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
			if (!s_InputDelayIsFixed && s_StallAllowance < c_MaxStallAllowance) {
				++s_StallAllowance;
				s_LastStallAllowanceChange = s_SimUpdate;
			}
		} else if (updateIsNear && std::string_view("kmbw").find(content[0]) != std::string_view::npos && messageInput.size() < 200 && messageInput[simUpdate].size() < 1000) {
			messageInput[simUpdate].emplace_back(std::move(content));
		}
	}
}

void NetLockstep::CompareStateHashes(long long simUpdate) {
	std::map<long long, uint64_t>& localHashes = s_StateHashes[s_LocalPlayer];
	std::map<long long, uint64_t>& remoteHashes = s_StateHashes[1 - s_LocalPlayer];
	auto localHash = localHashes.find(simUpdate);
	auto remoteHash = remoteHashes.find(simUpdate);
	if (localHash == localHashes.end() || remoteHash == remoteHashes.end()) {
		return;
	}
	if (localHash->second == remoteHash->second) {
		++s_MatchedHashCount;
	} else if (!s_Desynced) {
		s_Desynced = true;
		std::cerr << std::format("Lockstep: DESYNC! The machines stopped matching by update {}. This one has {:016x}, the other one has {:016x}.", simUpdate, localHash->second, remoteHash->second) << std::endl;
		System::SetQuit();
	}
	// Hashes get repeated, and ones from before this update could still turn up, so those are done with as well.
	s_LastComparedHashUpdate = std::max(s_LastComparedHashUpdate, simUpdate);
	localHashes.erase(localHashes.begin(), localHashes.upper_bound(s_LastComparedHashUpdate));
	remoteHashes.erase(remoteHashes.begin(), remoteHashes.upper_bound(s_LastComparedHashUpdate));
}
