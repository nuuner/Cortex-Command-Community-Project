#include "NetLockstep.h"

#include "Actor.h"
#include "ActivityMan.h"
#include "LuaMan.h"
#include "MovableMan.h"
#include "PresetMan.h"
#include "SceneMan.h"
#include "SLTerrain.h"
#include "TimerMan.h"

#include "DeterminismChecker.h"
#include "ConsoleMan.h"
#include "InputRecorder.h"
#include "InputScheme.h"
#include "UInputMan.h"
#include "RTETools.h"
#include "System.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
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
std::string NetLockstep::s_LastGameEnding;
long long NetLockstep::s_ForcedResyncUpdate = -1;
long long NetLockstep::s_ForcedDesyncUpdate = -1;

namespace {
	using Clock = std::chrono::steady_clock;

	constexpr size_t c_MaxMessageSize = 1000; //!< How long a message can get before the rest of the input is left for the next one, in bytes. Longer ones wouldn't fit in a single packet.
	constexpr int c_PlayerCount = 2; //!< How many machines, and so players, there are.
	constexpr long long c_HashInterval = 30; //!< How many simulation updates there are between the state hashes that get compared.
	constexpr size_t c_ResentHashCount = 3; //!< How many of the latest state hashes each message repeats.
	constexpr long long c_PingInterval = 30; //!< How many simulation updates there are between measurements of how long the connection takes.
	constexpr size_t c_PingSampleCount = 10; //!< How many of the latest measurements the input delay goes by. It goes by the worst of them.
	constexpr long long c_ShrinkInterval = 60; //!< The fewest simulation updates there can be between changes of the input delay when it is shrinking.
	constexpr long long c_StallAllowanceDuration = 300; //!< How many simulation updates each extra update of input delay that the other machine asked for is kept for.
	constexpr long long c_RepeatedStallWindow = 120; //!< How many simulation updates the other machine's waits are counted over, for telling whether they keep coming.
	constexpr size_t c_RepeatedStallCount = 3; //!< How many times the other machine has to say that it waited within that many updates for the input delay to get longer. It says so once every c_PingInterval updates at most.
	constexpr long long c_MaxStallAllowance = 2; //!< The most simulation updates of input delay that the other machine can ask to have added.
	constexpr long long c_MaxUpdatesAhead = 120; //!< How many simulation updates ahead of this machine the other one is believed about.
	constexpr int c_ResendIntervalMS = 20; //!< How long to wait for the other machine's input before repeating what this machine last sent, in case that got lost.
	constexpr int c_WaitTimeoutS = 120; //!< How long to wait for the other machine's first input before giving up on it. It has to cover the other machine loading the game.
	constexpr int c_SilenceTimeoutS = 10; //!< How long the other machine can go without being heard from, once it has been, before it is taken to be gone.

	constexpr size_t c_BlobPartSize = 1000; //!< How much of something big each message carries, in bytes.
	constexpr size_t c_MaxBlobSize = 64 * 1024 * 1024; //!< The biggest thing that the other machine is believed about, in bytes.
	constexpr int c_MaxReloadCount = 10; //!< How many times the machines get rebuilt from the host's game before giving up on keeping them matching.
	constexpr int c_CorrectionTimeoutS = 60; //!< How long correcting the other machine can take before it is given up on.
	constexpr char c_DigestBlob = 'D'; //!< What the host's digest of its objects, for the other machine to find its differences by, starts with.
	constexpr char c_ObjectsBlob = 'O'; //!< What the host's copies of the objects that the other machine asked for start with.
	constexpr int c_TerrainTileSize = 64; //!< How wide and tall the tiles are that the terrain is compared and copied in, in pixels.
	constexpr long long c_ObjectCorrectionHoldUpdates = 90; //!< How many simulation updates the machines have to keep matching for after objects were recreated, for the next correction to be tried that way too. Sooner than that, what is off isn't something that recreating objects gets at.
	constexpr size_t c_MaxWantedSize = 800; //!< How long the other machine's answer about which objects it wants can get, in bytes, since it has to fit in a message. It takes a byte for every four objects and terrain tiles there are.
	constexpr char c_ReloadBlob = 'L'; //!< What the host's game for both machines to rebuild theirs from starts with.

	int s_Epoch = 0; //!< How many times the machines have been rebuilt from the host's game. Messages say which time they are from, so that ones from before can be told apart.
	bool s_CorrectionIsWanted = false; //!< Whether the machines have stopped matching, and the other one is to be corrected from the host's state.
	bool s_ReloadIsForced = false; //!< Whether the next correction is to rebuild the machines from the host's game, without first trying to do with less.
	long long s_CorrectionUpdate = 0; //!< The simulation update that the host corrects the other machine at, or 0 if it isn't about to. The host holds its input for it back until then, so that the other machine can't get past it.
	int s_PatchCount = 0; //!< How many times the other machine's state was set to the host's.

	int s_LastBlobID = 0; //!< The number of the last thing the host sent, or that arrived here in full.
	int s_IncomingBlobID = 0; //!< The number of the thing that is arriving.
	std::string s_Blob; //!< What is being sent or is arriving.
	std::vector<bool> s_BlobPartsDone; //!< Which parts of it the other machine has confirmed having, or which have arrived.
	size_t s_BlobPartsLeft = 0; //!< How many parts of it are still to be confirmed, or to arrive.
	std::string s_ReceivedBlob; //!< What has arrived in full and is yet to be used, which has to wait for the simulation update it is for.
	int s_VerdictBlobID = 0; //!< The number of the host's state that the other machine has said how it went with, or that this machine has to say it about.
	bool s_Verdict = false; //!< Whether setting this machine's state to the host's got the machines matching.
	int s_WantedBlobID = 0; //!< The number of the host's digest that the other machine has said which objects it wants for, or that this machine has to say it about.
	std::string s_Wanted; //!< Which of the host's objects the other machine wants copies of, as hexadecimal digits of four objects each, in the order of the host's digest.
	std::string s_SyncDigest; //!< The host's digest that this machine is getting the objects for.
	long long s_LastObjectCorrectionUpdate = -c_ObjectCorrectionHoldUpdates; //!< The simulation update that the other machine last had objects recreated at.

	int s_Socket = -1; //!< The UDP socket that is connected to the other machine.
	int s_PendingSocket = -1; //!< The socket of a connection that was made in the menus, for the game that is about to start.
	int s_PendingLocalPlayer = 0; //!< Which player this machine is in the game that is about to start.
	bool s_StartedFromMenus = false; //!< Whether the game was set up in the menus, which it then goes back to when it is over, rather than the program quitting.
	bool s_LeaveIsWanted = false; //!< Whether this machine's player asked to leave the game.
	InputScheme s_OwnControlSchemes[c_PlayerCount]; //!< The control schemes that the players of this machine had before the game took them over.
	std::string s_ControlSchemeTexts[c_PlayerCount]; //!< The control scheme that each player's machine asked for, as text. Empty for mouse and keyboard.
	bool s_ControlSchemesAreTakenOver = false; //!< Whether the control schemes are the game's, and the players' own are to be put back.
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
	std::deque<long long> s_StallNoticesReceived; //!< The simulation updates that the other machine lately said it had to wait in.
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
	std::mt19937_64 s_LocalRandom{std::random_device{}()}; //!< Random numbers for what has nothing to do with the simulation: which messages the artificial loss throws away, and new seeds for the simulation's random numbers.

	/// Gets the name of the save that the machines get rebuilt from. Each running game has its own, since several can be running from the same folder.
	// shortcut: the host's game is passed along as a saved game, written to a file on both ends, since that is the only form the game can be rebuilt from. Give it a form of its own that stays in memory and leaves nothing out.
	std::string GetReloadSaveName() {
		return std::format("LockstepReload{}_{}", NetLockstep::GetLocalPlayer() + 1, getpid());
	}

	std::string GetReloadSavePath() {
		return g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + GetReloadSaveName() + ".ccsave";
	}

	/// Adds a value to the end of something to send, as its bytes.
	template <typename ValueType>
	void AppendValue(std::string& blob, const ValueType& value) {
		blob.append(reinterpret_cast<const char*>(&value), sizeof(value));
	}

	/// Takes a value off the front of something that arrived. Returns whether there was enough left for it.
	template <typename ValueType>
	bool TakeValue(std::string_view& blob, ValueType& value) {
		if (blob.size() < sizeof(value)) {
			return false;
		}
		std::memcpy(&value, blob.data(), sizeof(value));
		blob.remove_prefix(sizeof(value));
		return true;
	}

	/// Goes through the tiles of the terrain, handing over the rows of each one's material and foreground color pixels.
	// shortcut: the background color and the unseen layers are left out, since they don't change how the simulation goes. Add them if a difference in them shows.
	template <typename RowFunction>
	void ForEachTerrainTile(RowFunction&& onRow) {
		SLTerrain* terrain = g_SceneMan.GetTerrain();
		uint32_t tile = 0;
		for (int tileY = 0; tileY < terrain->GetMaterialBitmap()->h; tileY += c_TerrainTileSize) {
			for (int tileX = 0; tileX < terrain->GetMaterialBitmap()->w; tileX += c_TerrainTileSize, ++tile) {
				for (BITMAP* bitmap: {terrain->GetMaterialBitmap(), terrain->GetFGColorBitmap()}) {
					for (int y = tileY; y < std::min(tileY + c_TerrainTileSize, bitmap->h); ++y) {
						onRow(tile, bitmap->line[y] + tileX, static_cast<size_t>(std::min(c_TerrainTileSize, bitmap->w - tileX)));
					}
				}
			}
		}
	}

	/// Gets a hash of each tile of the terrain.
	std::vector<uint64_t> GetTerrainTileHashes() {
		std::vector<StateHasher> hashers;
		ForEachTerrainTile([&hashers](uint32_t tile, unsigned char* row, size_t size) {
			hashers.resize(tile + 1);
			hashers[tile].AddBytes(row, size);
		});
		std::vector<uint64_t> hashes;
		for (const StateHasher& hasher: hashers) {
			hashes.push_back(hasher.GetHash());
		}
		return hashes;
	}

	/// Gets the hash of the simulation state that the machines compare.
	uint64_t GetStateHash() {
		StateHasher hasher;
		g_MovableMan.HashState(hasher);
		// The objects can stay the same for a good while after the random numbers have stopped matching, so those are compared too.
		hasher.Add(g_RandomGenerator.PeekNext());
		return hasher.GetHash();
	}

	/// Starts all the random number generators that the simulation uses over from a seed. Doing this with the same seed on both machines gets their random numbers matching, whatever they were up to.
	void SeedSimRandomGenerators(uint64_t seed) {
		// Every time the machines are made the same, what input each has seen so far has to be made the same along with the rest.
		g_UInputMan.ResetSharedInputState();
		g_RandomGenerator.Seed(seed);
		// Which script state the next scripted object goes to decides which random numbers its scripts get.
		g_LuaMan.RestartScriptStateAssignment();
		g_LuaMan.GetMasterScriptState().SeedRandomGenerators(seed + 1);
		uint64_t luaStateSeed = seed + 2;
		for (LuaStateWrapper& luaState: g_LuaMan.GetThreadedScriptStates()) {
			luaState.SeedRandomGenerators(luaStateSeed++);
		}
	}

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
			// shortcut: Escape pauses into the menus, which the two machines would then get through on their own. It leaves the game instead, until pausing is synced.
			if (inputEvent.key.scancode == SDL_SCANCODE_ESCAPE) {
				s_LeaveIsWanted = s_LeaveIsWanted || (s_StartedFromMenus && inputEvent.type == SDL_EVENT_KEY_DOWN);
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
		// shortcut: every gamepad of this machine counts as its player's, and joysticks that SDL doesn't know as gamepads are left out.
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			playerEvent.gaxis.which = deviceID;
			break;
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
			playerEvent.gbutton.which = deviceID;
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
	if (s_LeaveIsWanted) {
		EndGame("You left the game.");
		return;
	}
	++s_SimUpdate;
	if (s_Epoch == 0 && s_SimUpdate == s_ForcedDesyncUpdate) {
		// Using up a random number is enough to send this machine's simulation its own way, and shoving an actor over makes for objects that are off as well.
		RandomNum();
		Vector distance;
		if (Actor* actor = g_MovableMan.GetClosestActor(Vector(), 1000000, distance)) {
			actor->SetPos(actor->GetPos() + Vector(4.0F, -4.0F));
			actor->SetVel(actor->GetVel() + Vector(2.0F, -6.0F));
		}
	}
	if (s_LocalPlayer == 0 && s_Epoch == 0 && s_SimUpdate == s_ForcedResyncUpdate) {
		s_CorrectionIsWanted = true;
		s_ReloadIsForced = true;
	}

	if (s_LocalPlayer == 0) {
		if (s_CorrectionIsWanted && s_CorrectionUpdate == 0) {
			// The other machine can't begin an update that it doesn't have all of this machine's input for, so the first one that it hasn't been told that about is one that both machines will be at the start of.
			s_CorrectionUpdate = std::max(s_LocalCompleteThrough + 1, s_SimUpdate);
		}
		if (s_CorrectionUpdate != 0 && s_SimUpdate == s_CorrectionUpdate) {
			if (!CorrectOtherMachine()) {
				std::cerr << "Lockstep: failed to get the other player's game matching again, ending at update " << s_SimUpdate << "." << std::endl;
				s_Desynced = true;
				EndGame("The two games stopped matching.");
				return;
			}
			if (s_SimUpdate == 0) {
				// The game was rebuilt, and the simulation updates are counted over from here.
				return;
			}
		}
	}

	std::string extraLines = AdaptInputDelay();
	if (s_LocalPlayer != 0 && s_CorrectionIsWanted) {
		// Only the host can do something about the machines not matching, so the other one keeps asking it to.
		extraLines += std::format("{} r\n", s_SimUpdate);
	}

	// This machine's input since the last update goes to a later update, to give it time to get to the other machine. It can't go to an update that was already said to be complete.
	long long inputUpdate = std::max(s_SimUpdate + s_InputDelay, s_LocalCompleteThrough + 1);
	for (std::string& line: s_PendingLines) {
		s_UnconfirmedLines[inputUpdate].emplace_back(line);
		s_InputLines[s_LocalPlayer][inputUpdate].emplace_back(std::move(line));
	}
	s_PendingLines.clear();

	if (s_SimUpdate % c_HashInterval == 0) {
		uint64_t stateHash = GetStateHash();
		s_StateHashes[s_LocalPlayer][s_SimUpdate] = stateHash;
		s_LatestStateHashes.emplace_back(s_SimUpdate, stateHash);
		if (s_LatestStateHashes.size() > c_ResentHashCount) {
			s_LatestStateHashes.pop_front();
		}
		CompareStateHashes(s_SimUpdate);
	}

	if (s_CorrectionUpdate != 0) {
		// Input for the update that the other machine gets corrected at isn't said to be complete until that is done, which is what keeps the other machine from getting past it.
		s_LocalCompleteThrough = std::max(s_LocalCompleteThrough, std::min(inputUpdate, s_CorrectionUpdate - 1));
	} else if (s_LeaveInputUpdateOpen) {
		// The next update's input goes to this same update now, which is what makes the delay one shorter.
		s_LeaveInputUpdateOpen = false;
		--s_InputDelay;
		s_LastInputDelayChange = s_SimUpdate;
	} else {
		s_LocalCompleteThrough = inputUpdate;
	}
	SendState(extraLines);

	auto waitStart = Clock::now();
	long long remoteCompleteAtWaitStart = s_RemoteCompleteThrough;
	auto lastSend = waitStart;
	Receive();
	if (UseReceivedBlob()) {
		return;
	}
	while (s_RemoteCompleteThrough < s_SimUpdate && !s_Desynced) {
		// The other machine can't be heard from while it loads the game, which is why it gets longer before it has been.
		bool silentForTooLong = Clock::now() - std::max(waitStart, s_LastHeardFrom) > std::chrono::seconds(s_RemoteCompleteThrough > 0 ? c_SilenceTimeoutS : c_WaitTimeoutS);
		if (s_PeerIsGone || silentForTooLong) {
			std::cerr << "Lockstep: the other player is gone, ending at update " << s_SimUpdate << "." << std::endl;
			s_PeerIsGone = true;
			EndGame("The other player left the game.");
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
		if (UseReceivedBlob()) {
			return;
		}
	}
	// A wait of less than an update is just this machine keeping in step with the other one, and doesn't show.
	// For tuning the input delay: set CCCP_NET_LOG to get a line a second about how evenly this machine gets to its updates, and how long it waits for the other one's input.
	static const bool logTimings = std::getenv("CCCP_NET_LOG") != nullptr;
	if (logTimings) {
		static Clock::time_point lastUpdateStart = waitStart;
		static double longestIntervalMS = 0.0, longestWaitMS = 0.0, totalWaitMS = 0.0;
		double waitMS = std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count();
		longestIntervalMS = std::max(longestIntervalMS, std::chrono::duration<double, std::milli>(waitStart - lastUpdateStart).count());
		static double lastWaitMS = 0.0;
		if (waitMS > 25.0) {
			std::cerr << std::format("Lockstep wait: update {}, waited {:.0f} ms for input through it, had it through {} when the wait began. This machine took {:.0f} ms over the update before, apart from waiting. Delay {}, own input complete through {}.", s_SimUpdate, waitMS, remoteCompleteAtWaitStart, std::chrono::duration<double, std::milli>(waitStart - lastUpdateStart).count() - lastWaitMS, s_InputDelay, s_LocalCompleteThrough) << std::endl;
		}
		lastWaitMS = waitMS;
		longestWaitMS = std::max(longestWaitMS, waitMS);
		totalWaitMS += waitMS;
		lastUpdateStart = waitStart;
		if (s_SimUpdate % 60 == 0) {
			std::cerr << std::format("Lockstep timing: update {}, delay {} (allowance {}), worst ping {:.0f} ms, longest gap between updates {:.0f} ms, waited {:.0f} ms in all and {:.0f} ms at most.", s_SimUpdate, s_InputDelay, s_StallAllowance, s_PingSamplesMS.empty() ? 0.0 : *std::max_element(s_PingSamplesMS.begin(), s_PingSamplesMS.end()), longestIntervalMS, totalWaitMS, longestWaitMS) << std::endl;
			longestIntervalMS = longestWaitMS = totalWaitMS = 0.0;
		}
	}
	if (double waitedMS = std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count(); waitedMS > g_TimerMan.GetDeltaTimeMS()) {
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
	std::cerr << "Lockstep: the other machine's state was set to the host's " << s_PatchCount << " times, and the machines were rebuilt from the host's game " << s_Epoch << " times." << std::endl;
	std::cerr << std::format("Lockstep: {} updates waited for the other player's input, for {:.0f} ms in total.", s_StalledUpdateCount, s_StalledMS) << std::endl;
	std::cerr << std::format("Lockstep: input delay ended at {} updates, with a ping of about {:.0f} ms. {} of {} messages were thrown away.", s_InputDelay, s_PingSamplesMS.empty() ? 0.0 : s_PingSamplesMS.back(), s_LostMessageCount, s_SentMessageCount) << std::endl;
	s_Active = false;
	if (s_ControlSchemesAreTakenOver) {
		for (int player = 0; player < c_PlayerCount; ++player) {
			g_UInputMan.GetControlScheme(player)->Create(s_OwnControlSchemes[player]);
			s_ControlSchemeTexts[player].clear();
		}
		g_UInputMan.GiveBackGamepads();
		s_ControlSchemesAreTakenOver = false;
		g_UInputMan.CheckMultiMouseKeyboardEnabled();
	}
}

void NetLockstep::EndGame(const std::string& reason) {
	if (!s_StartedFromMenus) {
		System::SetQuit();
		return;
	}
	Stop();
	s_LastGameEnding = reason;
	g_ConsoleMan.PrintString("SYSTEM: " + reason);
	// The game can't be carried on with from here, so it is ended rather than left for the menus to offer resuming.
	g_ActivityMan.EndActivity();
	g_ActivityMan.PauseActivity(true, true);
}

std::string NetLockstep::GetOwnControlSchemeText() {
	auto stream = std::make_unique<std::ostringstream>();
	std::ostringstream* text = stream.get();
	Writer writer(std::move(stream));
	writer << (s_ControlSchemesAreTakenOver ? s_OwnControlSchemes[0] : *g_UInputMan.GetControlScheme(0));
	return text->str();
}

bool NetLockstep::HasTakenOverControlSchemes() {
	return s_ControlSchemesAreTakenOver;
}

void NetLockstep::SetPlayerControlSchemeTexts(const std::string& hostScheme, const std::string& joinerScheme) {
	s_ControlSchemeTexts[0] = hostScheme;
	s_ControlSchemeTexts[1] = joinerScheme;
}

void NetLockstep::SetUpControlSchemes() {
	std::array<SDL_JoystickID, Players::MaxPlayerCount> gamepadIDs{};
	for (int player = 0; player < c_PlayerCount; ++player) {
		InputScheme* controlScheme = g_UInputMan.GetControlScheme(player);
		if (!s_ControlSchemesAreTakenOver) {
			s_OwnControlSchemes[player].Create(*controlScheme);
		}
		if (s_ControlSchemeTexts[player].empty()) {
			controlScheme->SetDevice(InputDevice::DEVICE_MOUSE_KEYB);
			controlScheme->SetPreset(InputScheme::InputPreset::PresetMouseWASDKeys);
		} else {
			// Both machines read the same text, so the player's controls work the same on both, whatever each has in its own settings.
			Reader reader(std::make_unique<std::istringstream>(s_ControlSchemeTexts[player]), "the other machine's control scheme", true, nullptr, false);
			controlScheme->Reset();
			reader >> *controlScheme;
		}
		// Whichever device it is for, the player's input arrives on a made up one, which is the same on both machines.
		if (controlScheme->GetDevice() >= InputDevice::DEVICE_GAMEPAD_1) {
			controlScheme->SetDevice(static_cast<InputDevice>(InputDevice::DEVICE_GAMEPAD_1 + player));
			gamepadIDs[player] = static_cast<SDL_JoystickID>(GetPlayerDeviceID(player));
		}
		DeviceID deviceID;
		deviceID.mouseKeyboard = {GetPlayerDeviceID(player), GetPlayerDeviceID(player)};
		controlScheme->SetDeviceID(deviceID);
	}
	g_UInputMan.TakeOverGamepads(gamepadIDs);
	s_ControlSchemesAreTakenOver = true;
	g_UInputMan.CheckMultiMouseKeyboardEnabled();
	s_ControlSchemesAreTakenOver = true;
}

void NetLockstep::StartWhenGameStarts(int connectedSocket, int localPlayer) {
	s_PendingSocket = connectedSocket;
	s_PendingLocalPlayer = localPlayer;
}

void NetLockstep::PrepareGameStart() {
	if (s_PendingSocket >= 0) {
		Stop();
		s_Socket = s_PendingSocket;
		s_PendingSocket = -1;
		s_LocalPlayer = s_PendingLocalPlayer;
		s_StartedFromMenus = true;
		s_LeaveIsWanted = false;
		s_LastGameEnding.clear();

		// Whatever is left of a game before this one is forgotten.
		StartEpoch(0);
		s_PeerIsGone = false;
		s_Desynced = false;
		s_PatchCount = 0;
		s_LastBlobID = 0;
		s_IncomingBlobID = 0;
		s_VerdictBlobID = 0;
		s_WantedBlobID = 0;
		s_Blob.clear();
		s_ReceivedBlob.clear();
		s_PingSamplesMS.clear();
		s_StallAllowance = 0;
		s_StallNoticesReceived.clear();
		s_MatchedHashCount = 0;
		if (!s_InputDelayIsFixed) {
			s_InputDelay = 3;
		}
		s_LastHeardFrom = Clock::now();
		s_Active = true;
		std::cerr << "Lockstep: starting a game from the menus, playing as player " << s_LocalPlayer + 1 << "." << std::endl;
	}
	if (!s_Active) {
		return;
	}
	// Everything that the simulation goes by and that the game doesn't set up itself has to start out the same on both machines.
	// shortcut: the script states carry over what earlier games left in them, which can differ between the machines. Make them anew here once that shows.
	MovableObject::SetUniqueIDCounters(std::vector<int64_t>(MovableObject::GetUniqueIDCounters().size(), 0));
	SeedSimRandomGenerators(0);
	SetUpControlSchemes();
}

void NetLockstep::SendState(const std::string& extraLines) {
	// How far this machine has the other one's input, so that the other one can stop repeating it.
	// Which count of the simulation updates this is about comes first, so that a message from before the machines last loaded the same game can be told apart.
	std::string message = std::format("{} e\n{} a\n", s_Epoch, s_RemoteCompleteThrough);

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
	if (s_LocalPlayer != 0 && s_VerdictBlobID > 0) {
		// The host waits to hear how setting this machine's state to its own went, and keeps being told in case that gets lost.
		message += std::format("{} v {}\n", s_VerdictBlobID, static_cast<int>(s_Verdict));
	}
	if (s_LocalPlayer != 0 && s_WantedBlobID > 0 && s_WantedBlobID == s_LastBlobID) {
		message += std::format("{} w x{}\n", s_WantedBlobID, s_Wanted);
	}
	SendToPeer(message + extraLines);
}

void NetLockstep::SendToPeer(const std::string& text) {
	if (s_Socket < 0 || s_PeerIsGone) {
		return;
	}
	++s_SentMessageCount;
	if (s_ArtificialLossPercent > 0 && static_cast<int>(s_LocalRandom() % 100) < s_ArtificialLossPercent) {
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
		} else if (text.starts_with("L ")) {
			// The host keeps saying to start the game until it hears that this machine has, and that got lost.
			if (text.starts_with("L start") && s_LocalPlayer != 0) {
				SendNow("L started");
			}
		} else if (text.starts_with("S ") || text.starts_with("K ")) {
			HandleBlobPart(text);
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

		if (content == "e") {
			if (simUpdate != s_Epoch) {
				return;
			}
		} else if (content == "r") {
			if (s_LocalPlayer == 0) {
				s_CorrectionIsWanted = true;
			}
		} else if (content.starts_with("v ")) {
			if (s_LocalPlayer == 0 && simUpdate == s_LastBlobID) {
				s_VerdictBlobID = static_cast<int>(simUpdate);
				s_Verdict = content[2] == '1';
			}
		} else if (content.starts_with("w x")) {
			if (s_LocalPlayer == 0 && simUpdate == s_LastBlobID) {
				s_WantedBlobID = static_cast<int>(simUpdate);
				s_Wanted = content.substr(3);
			}
		} else if (content == "a") {
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
			// A machine that takes long over a frame makes the other one wait once, which shows less than holding all input back for longer to avoid it. So the delay only grows when the waits keep coming, which is the connection's doing.
			s_StallNoticesReceived.push_back(s_SimUpdate);
			while (s_SimUpdate - s_StallNoticesReceived.front() > c_RepeatedStallWindow) {
				s_StallNoticesReceived.pop_front();
			}
			if (!s_InputDelayIsFixed && s_StallAllowance < c_MaxStallAllowance && s_StallNoticesReceived.size() >= c_RepeatedStallCount) {
				++s_StallAllowance;
				s_LastStallAllowanceChange = s_SimUpdate;
				s_StallNoticesReceived.clear();
			}
		} else if (updateIsNear && std::string_view("kmbwj").find(content[0]) != std::string_view::npos && messageInput.size() < 200 && messageInput[simUpdate].size() < 1000) {
			messageInput[simUpdate].emplace_back(std::move(content));
		}
	}
}

void NetLockstep::ForgetStateHashes() {
	for (std::map<long long, uint64_t>& stateHashes: s_StateHashes) {
		stateHashes.clear();
	}
	s_LatestStateHashes.clear();
	s_LastComparedHashUpdate = s_SimUpdate;
}

void NetLockstep::StartEpoch(int epoch) {
	s_Epoch = epoch;
	s_CorrectionIsWanted = false;
	s_ReloadIsForced = false;
	s_CorrectionUpdate = 0;

	s_SimUpdate = 0;
	s_LocalCompleteThrough = 0;
	s_RemoteAckedThrough = 0;
	s_RemoteCompleteThrough = 0;
	s_PendingLines.clear();
	s_UnconfirmedLines.clear();
	for (std::map<long long, std::vector<std::string>>& playerLines: s_InputLines) {
		playerLines.clear();
	}
	ForgetStateHashes();

	s_LastObjectCorrectionUpdate = -c_ObjectCorrectionHoldUpdates;
	s_LeaveInputUpdateOpen = false;
	s_LastInputDelayChange = 0;
	s_LastStallAllowanceChange = 0;
	s_LastStallNotice = 0;
}

bool NetLockstep::CorrectOtherMachine() {
	auto correctionStart = Clock::now();
	bool matching = false;

	// The other machine answers each thing it is sent, with which objects it wants or with how it went.
	auto waitForAnswer = [&correctionStart]() {
		while (s_WantedBlobID != s_LastBlobID && s_VerdictBlobID != s_LastBlobID) {
			if (s_PeerIsGone || Clock::now() - correctionStart > std::chrono::seconds(c_CorrectionTimeoutS)) {
				return false;
			}
			pollfd pollInfo{s_Socket, POLLIN, 0};
			poll(&pollInfo, 1, 2);
			Receive();
		}
		return true;
	};

	if (!s_ReloadIsForced && s_SimUpdate - s_LastObjectCorrectionUpdate >= c_ObjectCorrectionHoldUpdates) {
		s_LastObjectCorrectionUpdate = s_SimUpdate;
		// First the other machine gets to find out which of its objects aren't as this machine has them, and to recreate just those from this machine's copies of them.
		std::string digest;
		g_MovableMan.WriteSyncDigest(digest);
		std::string digestBlob(1, c_DigestBlob);
		AppendValue(digestBlob, s_SimUpdate);
		AppendValue(digestBlob, static_cast<uint32_t>(digest.size()));
		digestBlob += digest;
		std::vector<uint64_t> tileHashes = GetTerrainTileHashes();
		for (uint64_t tileHash: tileHashes) {
			AppendValue(digestBlob, tileHash);
		}
		if (!SendBlob(digestBlob) || !waitForAnswer()) {
			return false;
		}
		if (s_WantedBlobID == s_LastBlobID) {
			std::vector<bool> wanted;
			for (char digit: s_Wanted) {
				int bits = std::isxdigit(static_cast<unsigned char>(digit)) ? std::stoi(std::string(1, digit), nullptr, 16) : 0;
				for (int bit = 0; bit < 4; ++bit) {
					wanted.push_back((bits >> bit) & 1);
				}
			}
			// The terrain tiles come before the objects in what the other machine wants. Those are the same bytes on both ends, so this machine's own stay as they are.
			wanted.resize(std::max(wanted.size(), tileHashes.size()), false);
			std::string objects;
			g_MovableMan.WriteSyncObjects(std::vector<bool>(wanted.begin() + static_cast<std::ptrdiff_t>(tileHashes.size()), wanted.end()), objects);
			std::string tiles;
			uint32_t lastTile = UINT32_MAX;
			ForEachTerrainTile([&](uint32_t tile, unsigned char* row, size_t size) {
				if (wanted[tile]) {
					if (tile != lastTile) {
						AppendValue(tiles, tile);
						lastTile = tile;
					}
					tiles.append(reinterpret_cast<const char*>(row), size);
				}
			});

			// This machine recreates those objects from its copies too, since an object recreated from a copy isn't quite the one that the copy was made of.
			// The random numbers are started over on both machines rather than passed along, which gets them matching just the same.
			std::vector<int64_t> uniqueIDCounters = MovableObject::GetUniqueIDCounters();
			uint64_t seed = s_LocalRandom();
			int recreatedCount = 0;
			int removedCount = 0;
			// Creating objects takes random numbers, so those have to match before rather than after.
			SeedSimRandomGenerators(seed);
			if (g_MovableMan.ApplySyncObjects(digest, objects, recreatedCount, removedCount)) {
				std::string objectsBlob(1, c_ObjectsBlob);
				AppendValue(objectsBlob, s_SimUpdate);
				AppendValue(objectsBlob, seed);
				AppendValue(objectsBlob, static_cast<uint32_t>(uniqueIDCounters.size()));
				for (int64_t uniqueIDCounter: uniqueIDCounters) {
					AppendValue(objectsBlob, uniqueIDCounter);
				}
				AppendValue(objectsBlob, GetStateHash());
				AppendValue(objectsBlob, static_cast<uint32_t>(objects.size()));
				if (!SendBlob(objectsBlob + objects + tiles) || !waitForAnswer()) {
					return false;
				}
				matching = s_VerdictBlobID == s_LastBlobID && s_Verdict;
			}
		}
	}

	if (matching) {
		++s_PatchCount;
		s_CorrectionIsWanted = false;
		s_CorrectionUpdate = 0;
		ForgetStateHashes();
		// The traces of the two machines can't match up to here, so this marks where they are to match from again.
		DeterminismCheck("Lockstep correction", s_LastBlobID);
		std::cerr << std::format("Lockstep: recreated the objects that the other player had differently at update {}, in {:.0f} ms.", s_SimUpdate, std::chrono::duration<double, std::milli>(Clock::now() - correctionStart).count()) << std::endl;
		return true;
	}

	// The other machine doesn't have the same objects anymore, so both machines get their game rebuilt from this one's.
	if (s_Epoch >= c_MaxReloadCount || !g_ActivityMan.SaveCurrentGame(GetReloadSaveName())) {
		return false;
	}
	// The save gets written in the background, and has to be all there before it can be read back.
	if (g_ActivityMan.GetSaveGameTask().valid()) {
		g_ActivityMan.GetSaveGameTask().wait();
	}
	std::string reload(1, c_ReloadBlob);
	AppendValue(reload, s_SimUpdate);
	{
		std::ifstream saveFile(GetReloadSavePath(), std::ios::binary);
		reload.append(std::istreambuf_iterator<char>(saveFile), std::istreambuf_iterator<char>());
	}
	long long correctedUpdate = s_SimUpdate;
	if (reload.size() <= 1 + sizeof(s_SimUpdate) || !SendBlob(reload)) {
		return false;
	}
	if (!RebuildGame()) {
		return false;
	}
	std::cerr << std::format("Lockstep: rebuilt both games from this one at update {}, in {:.0f} ms.", correctedUpdate, std::chrono::duration<double, std::milli>(Clock::now() - correctionStart).count()) << std::endl;
	return true;
}

bool NetLockstep::UseReceivedBlob() {
	std::string_view blob = s_ReceivedBlob;
	char kind = 0;
	long long simUpdate = 0;
	// What arrived is for the start of one simulation update, which the host keeps this machine from getting past, though it can arrive before this machine gets there.
	if (!TakeValue(blob, kind) || !TakeValue(blob, simUpdate) || simUpdate != s_SimUpdate) {
		return false;
	}

	if (kind == c_ReloadBlob) {
		{
			std::ofstream saveFile(GetReloadSavePath(), std::ios::binary | std::ios::trunc);
			saveFile.write(blob.data(), static_cast<std::streamsize>(blob.size()));
		}
		s_ReceivedBlob.clear();
		if (!RebuildGame()) {
			s_Desynced = true;
			EndGame("The two games stopped matching.");
		} else {
			std::cerr << "Lockstep: rebuilt this game from the host's." << std::endl;
		}
		return true;
	}

	if (kind == c_DigestBlob) {
		// The host is told which of its objects this machine doesn't have the way it does, or that there is no matching it by recreating objects.
		uint32_t digestSize = 0;
		bool readable = TakeValue(blob, digestSize) && digestSize <= blob.size();
		s_SyncDigest = readable ? std::string(blob.substr(0, digestSize)) : "";
		std::vector<bool> wanted;
		std::vector<bool> wantedObjects;
		readable = readable && g_MovableMan.FindSyncDifferences(s_SyncDigest, wantedObjects);
		if (readable) {
			// The tiles go first, since the host knows how many of those there are, and so where the objects start.
			blob.remove_prefix(digestSize);
			for (uint64_t ownTileHash: GetTerrainTileHashes()) {
				uint64_t hostTileHash = 0;
				readable = readable && TakeValue(blob, hostTileHash);
				wanted.push_back(hostTileHash != ownTileHash);
			}
			wanted.insert(wanted.end(), wantedObjects.begin(), wantedObjects.end());
		}
		s_ReceivedBlob.clear();
		if (readable && wanted.size() <= c_MaxWantedSize * 4) {
			s_Wanted.clear();
			for (size_t i = 0; i < wanted.size(); i += 4) {
				int bits = 0;
				for (size_t bit = 0; bit < 4 && i + bit < wanted.size(); ++bit) {
					bits |= static_cast<int>(wanted[i + bit]) << bit;
				}
				s_Wanted += "0123456789abcdef"[bits];
			}
			s_WantedBlobID = s_LastBlobID;
		} else {
			std::cerr << "Lockstep: this game is too far from the host's at update " << s_SimUpdate << " to be matched by recreating objects." << std::endl;
			s_VerdictBlobID = s_LastBlobID;
			s_Verdict = false;
		}
		return false;
	}

	bool matching = false;
	int recreatedCount = 0;
	int removedCount = 0;
	int tileCount = 0;
	uint32_t objectsSize = 0;
	uint64_t seed = 0;
	uint32_t uniqueIDCounterCount = 0;
	std::vector<int64_t> uniqueIDCounters;
	uint64_t hostStateHash = 0;
	bool readable = kind == c_ObjectsBlob && TakeValue(blob, seed) && TakeValue(blob, uniqueIDCounterCount) && uniqueIDCounterCount <= 1024;
	for (uint32_t i = 0; readable && i < uniqueIDCounterCount; ++i) {
		readable = TakeValue(blob, uniqueIDCounters.emplace_back());
	}
	if (readable && TakeValue(blob, hostStateHash) && TakeValue(blob, objectsSize) && objectsSize <= blob.size()) {
		// The objects that get recreated get new unique IDs, which have to come out the same as they did on the host.
		MovableObject::SetUniqueIDCounters(uniqueIDCounters);
		SeedSimRandomGenerators(seed);
		matching = g_MovableMan.ApplySyncObjects(s_SyncDigest, blob.substr(0, objectsSize), recreatedCount, removedCount) && GetStateHash() == hostStateHash;
		// What is left are the terrain tiles that this machine asked for, each as its number and then its pixels.
		blob.remove_prefix(objectsSize);
		uint32_t hostTile = 0;
		bool moreTiles = TakeValue(blob, hostTile);
		ForEachTerrainTile([&](uint32_t tile, unsigned char* row, size_t size) {
			while (moreTiles && hostTile < tile) {
				// That was all of that tile, so the number of the next one follows.
				++tileCount;
				moreTiles = TakeValue(blob, hostTile);
			}
			if (moreTiles && tile == hostTile && blob.size() >= size) {
				std::memcpy(row, blob.data(), size);
				blob.remove_prefix(size);
			}
		});
		tileCount += moreTiles;
	}
	s_ReceivedBlob.clear();
	s_VerdictBlobID = s_LastBlobID;
	s_Verdict = matching;
	if (matching) {
		++s_PatchCount;
		s_CorrectionIsWanted = false;
		ForgetStateHashes();
		DeterminismCheck("Lockstep correction", s_LastBlobID);
		std::cerr << std::format("Lockstep: recreated {} objects from the host's, removed {} and copied {} terrain tiles at update {}.", recreatedCount, removedCount, tileCount, s_SimUpdate) << std::endl;
	} else {
		std::cerr << std::format("Lockstep: recreating {} objects from the host's and removing {} at update {} didn't get the games matching.", recreatedCount, removedCount, s_SimUpdate) << std::endl;
	}
	return false;
}

bool NetLockstep::RebuildGame() {
	int epoch = s_Epoch + 1;
	// Everything that the simulation goes by and that isn't in the save has to end up the same on both machines: how much time has passed, which unique IDs get handed out next, and the random numbers.
	// Each rebuild hands out unique IDs from a range of its own, so that none of them can be one that an object from before still has.
	static_assert((int64_t(c_MaxReloadCount) + 1) << 32 < int64_t(1) << MovableObject::c_UniqueIDCountBits, "The last rebuild has to have unique IDs left to hand out.");
	g_TimerMan.ResetTime();
	MovableObject::SetUniqueIDCounters(std::vector<int64_t>(MovableObject::GetUniqueIDCounters().size(), static_cast<int64_t>(epoch) << 32));
	SeedSimRandomGenerators(static_cast<uint64_t>(epoch));
	bool loaded = g_ActivityMan.LoadAndLaunchGame(GetReloadSaveName());
	std::remove(GetReloadSavePath().c_str());
	StartEpoch(epoch);
	DeterminismCheck("Lockstep correction", epoch);
	return loaded;
}

bool NetLockstep::SendBlob(const std::string& blob) {
	++s_LastBlobID;
	s_Blob = blob;
	size_t partCount = (s_Blob.size() + c_BlobPartSize - 1) / c_BlobPartSize;
	s_BlobPartsDone.assign(partCount, false);
	s_BlobPartsLeft = partCount;
	std::vector<Clock::time_point> partLastSent(partCount);

	// shortcut: the parts go out as fast as the other machine confirms them, without regard for how much the connection can take. Pace them once this takes long over real connections.
	auto sendStart = Clock::now();
	while (s_BlobPartsLeft > 0) {
		if (s_PeerIsGone || Clock::now() - sendStart > std::chrono::seconds(c_CorrectionTimeoutS)) {
			return false;
		}
		// A part is sent again when the other machine has had the time to confirm it and hasn't.
		double pingMS = s_PingSamplesMS.empty() ? 50.0 : *std::max_element(s_PingSamplesMS.begin(), s_PingSamplesMS.end());
		auto resendAfter = std::chrono::milliseconds(static_cast<int>(pingMS * 2.0) + 50);
		int sentCount = 0;
		for (size_t part = 0; part < partCount && sentCount < 64; ++part) {
			if (!s_BlobPartsDone[part] && Clock::now() - partLastSent[part] > resendAfter) {
				SendToPeer(std::format("S {} {} {}\n", s_LastBlobID, part, s_Blob.size()) + s_Blob.substr(part * c_BlobPartSize, c_BlobPartSize));
				partLastSent[part] = Clock::now();
				++sentCount;
			}
		}
		pollfd pollInfo{s_Socket, POLLIN, 0};
		poll(&pollInfo, 1, 2);
		Receive();
	}
	s_Blob.clear();
	return true;
}

void NetLockstep::HandleBlobPart(const std::string& text) {
	size_t headerEnd = text.find('\n');
	std::istringstream header(text.substr(0, headerEnd));
	char kind = 0;
	int blobID = -1;
	long long part = -1;
	header >> kind >> blobID >> part;
	if (header.fail() || part < 0) {
		return;
	}

	if (kind == 'K') {
		// The other machine has this part.
		if (s_LocalPlayer == 0 && blobID == s_LastBlobID && static_cast<size_t>(part) < s_BlobPartsDone.size() && !s_BlobPartsDone[part]) {
			s_BlobPartsDone[part] = true;
			--s_BlobPartsLeft;
		}
		return;
	}
	if (s_LocalPlayer == 0 || headerEnd == std::string::npos) {
		return;
	}
	if (blobID <= s_LastBlobID) {
		// This machine already has all of this, but its confirmation of this part got lost.
		SendToPeer(std::format("K {} {}\n", blobID, part));
		return;
	}
	size_t blobSize = 0;
	header >> blobSize;
	std::string_view data = std::string_view(text).substr(headerEnd + 1);
	size_t partCount = (blobSize + c_BlobPartSize - 1) / c_BlobPartSize;
	size_t partStart = static_cast<size_t>(part) * c_BlobPartSize;
	// The other machine is not to be trusted with how much memory gets used, or with where in it things get put.
	if (header.fail() || blobSize == 0 || blobSize > c_MaxBlobSize || static_cast<size_t>(part) >= partCount || data.size() != std::min(c_BlobPartSize, blobSize - partStart)) {
		return;
	}

	if (blobID != s_IncomingBlobID || s_Blob.size() != blobSize) {
		s_IncomingBlobID = blobID;
		s_Blob.assign(blobSize, '\0');
		s_BlobPartsDone.assign(partCount, false);
		s_BlobPartsLeft = partCount;
	}
	if (!s_BlobPartsDone[part]) {
		s_Blob.replace(partStart, data.size(), data);
		s_BlobPartsDone[part] = true;
		--s_BlobPartsLeft;
	}
	SendToPeer(std::format("K {} {}\n", blobID, part));

	if (s_BlobPartsLeft == 0) {
		s_LastBlobID = blobID;
		s_ReceivedBlob = std::move(s_Blob);
		s_Blob.clear();
		s_BlobPartsDone.clear();
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
	} else if (!s_Desynced && !s_CorrectionIsWanted) {
		std::cerr << std::format("Lockstep: DESYNC! The machines stopped matching by update {}. This one has {:016x}, the other one has {:016x}.", simUpdate, localHash->second, remoteHash->second) << std::endl;
		s_CorrectionIsWanted = true;
	}
	// Hashes get repeated, and ones from before this update could still turn up, so those are done with as well.
	s_LastComparedHashUpdate = std::max(s_LastComparedHashUpdate, simUpdate);
	localHashes.erase(localHashes.begin(), localHashes.upper_bound(s_LastComparedHashUpdate));
	remoteHashes.erase(remoteHashes.begin(), remoteHashes.upper_bound(s_LastComparedHashUpdate));
}
