#include "NetLobby.h"

#include "ActivityMan.h"
#include "GameActivity.h"
#include "InputScheme.h"
#include "UInputMan.h"
#include "LuaMan.h"
#include "NetLockstep.h"
#include "PresetMan.h"
#include "Scene.h"
#include "SceneMan.h"

#include <chrono>
#include <sstream>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace RTE;

NetLobby::State NetLobby::s_State = NetLobby::State::Idle;
std::vector<NetLobby::Game> NetLobby::s_Games;
NetLobby::Setup NetLobby::s_Setup;
bool NetLobby::s_HasPeer = false;
std::string NetLobby::s_PeerName;
bool NetLobby::s_PeerReady = false;
bool NetLobby::s_Ready = false;
bool NetLobby::s_Starting = false;
bool NetLobby::s_GameIsToStart = false;
std::string NetLobby::s_Error;

namespace {
	using Clock = std::chrono::steady_clock;

	constexpr uint16_t c_AnnouncePort = 47530; //!< The port that hosted games are announced on.
	constexpr std::string_view c_AnnounceMark = "CCCPLAN1"; //!< What an announcement starts with. The number is for telling apart ones that newer versions may come to make differently.
	constexpr auto c_AnnounceInterval = std::chrono::milliseconds(1000); //!< How often a hosted game is announced.
	constexpr auto c_SendInterval = std::chrono::milliseconds(200); //!< How often the machines of a lobby tell each other how things are. Everything gets repeated rather than confirmed, so lost messages don't matter.
	constexpr auto c_Timeout = std::chrono::seconds(3); //!< How long a game or the other machine can go without being heard from before it is taken to be gone.

	int s_AnnounceSocket = -1; //!< The socket that announcements are listened for on when browsing, or sent from when hosting.
	int s_GameSocket = -1; //!< The socket that the lobby talks over, and that the game then gets played over.
	int s_GamePort = 0; //!< The port that the hosted game is on.
	sockaddr_in s_PeerAddress{}; //!< Where the machine that joined is, when hosting.
	Clock::time_point s_LastSend; //!< When the other machine was last told how things are.
	Clock::time_point s_LastAnnouncement; //!< When the hosted game was last announced.
	Clock::time_point s_PeerLastHeardFrom; //!< When the other machine was last heard from.
	std::vector<Clock::time_point> s_GamesLastHeardFrom; //!< When each of the games on the network was last heard from.

	void CloseSocket(int& socketToClose) {
		if (socketToClose >= 0) {
			close(socketToClose);
			socketToClose = -1;
		}
	}

	std::string GetLocalName() {
		char name[256] = {};
		gethostname(name, sizeof(name) - 1);
		std::string localName(name);
		return localName.substr(0, localName.find('.')).substr(0, 32);
	}

	std::vector<std::string> SplitLines(const std::string& text) {
		std::vector<std::string> lines;
		std::istringstream stream(text);
		for (std::string line; std::getline(stream, line);) {
			lines.push_back(line);
		}
		return lines;
	}

	constexpr char c_ControlSchemeMark = '\x1e'; //!< What comes before the sender's control scheme at the end of a message, which no text has in it.
	std::string s_PeerControlScheme; //!< The control scheme that the other machine's player plays with, as text.

	/// Takes the sender's control scheme off the end of a message, and returns it.
	std::string TakeControlScheme(std::string& message) {
		size_t mark = message.find(c_ControlSchemeMark);
		if (mark == std::string::npos) {
			return "";
		}
		std::string controlScheme = message.substr(mark + 1);
		message.erase(mark);
		return controlScheme;
	}

	/// Sends an announcement to every network that this machine is on, and to this machine itself, for trying things out with two games on one machine.
	void Announce(const std::string& text) {
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(c_AnnouncePort);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sendto(s_AnnounceSocket, text.data(), text.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));

		ifaddrs* interfaces = nullptr;
		if (getifaddrs(&interfaces) != 0) {
			return;
		}
		for (ifaddrs* interface = interfaces; interface; interface = interface->ifa_next) {
			if (interface->ifa_addr && interface->ifa_addr->sa_family == AF_INET && (interface->ifa_flags & IFF_BROADCAST) && interface->ifa_broadaddr) {
				address.sin_addr = reinterpret_cast<sockaddr_in*>(interface->ifa_broadaddr)->sin_addr;
				sendto(s_AnnounceSocket, text.data(), text.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));
			}
		}
		freeifaddrs(interfaces);
	}
} // namespace

std::string NetLobby::Setup::ToText() const {
	std::ostringstream text;
	text << ActivityClass << "\n" << ActivityPreset << "\n" << ScenePreset << "\n";
	text << Difficulty << " " << StartingGold << " " << FogOfWar << " " << RequireClearPathToOrbit << " " << DeployUnits << " " << PlayerTeams[0] << " " << PlayerTeams[1] << " " << CPUTeam;
	for (int teamAISkill: TeamAISkills) {
		text << " " << teamAISkill;
	}
	for (const std::string& teamTech: TeamTechs) {
		text << "\n" << teamTech;
	}
	return text.str();
}

bool NetLobby::Setup::FromText(const std::string& text) {
	std::vector<std::string> lines = SplitLines(text);
	// A team without a tech makes for an empty line, and the last of those doesn't come out as a line at all.
	lines.resize(std::max<size_t>(lines.size(), 8));
	ActivityClass = lines[0];
	ActivityPreset = lines[1];
	ScenePreset = lines[2];
	std::istringstream numbers(lines[3]);
	numbers >> Difficulty >> StartingGold >> FogOfWar >> RequireClearPathToOrbit >> DeployUnits >> PlayerTeams[0] >> PlayerTeams[1] >> CPUTeam;
	for (int& teamAISkill: TeamAISkills) {
		numbers >> teamAISkill;
	}
	for (int team = 0; team < 4; ++team) {
		TeamTechs[team] = lines[4 + team];
	}
	return !numbers.fail() && !ActivityPreset.empty() && !ScenePreset.empty();
}

bool NetLobby::Setup::HasPlayersOnTeams() const {
	// The other machine is not to be trusted with which teams there are.
	auto teamIsValid = [](int team) { return team >= Activity::Teams::TeamOne && team < Activity::Teams::MaxTeamCount; };
	return teamIsValid(PlayerTeams[0]) && teamIsValid(PlayerTeams[1]) && (CPUTeam == Activity::Teams::NoTeam || teamIsValid(CPUTeam));
}

void NetLobby::StartBrowsing() {
	if (s_State != State::Idle) {
		return;
	}
	s_Games.clear();
	s_GamesLastHeardFrom.clear();
	s_AnnounceSocket = socket(AF_INET, SOCK_DGRAM, 0);
	// Several games on one machine can all listen for announcements this way.
	int enable = 1;
	setsockopt(s_AnnounceSocket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
	setsockopt(s_AnnounceSocket, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(c_AnnouncePort);
	if (s_AnnounceSocket < 0 || bind(s_AnnounceSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
		CloseSocket(s_AnnounceSocket);
		s_Error = "Can't listen for games on this network.";
		return;
	}
	s_State = State::Browsing;
}

bool NetLobby::StartHosting() {
	Leave();
	s_AnnounceSocket = socket(AF_INET, SOCK_DGRAM, 0);
	int enable = 1;
	setsockopt(s_AnnounceSocket, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));

	// Any port that is free will do for the game, since the announcements say which one it is.
	s_GameSocket = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	socklen_t addressSize = sizeof(address);
	if (s_AnnounceSocket < 0 || s_GameSocket < 0 || bind(s_GameSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || getsockname(s_GameSocket, reinterpret_cast<sockaddr*>(&address), &addressSize) != 0) {
		CloseSocket(s_AnnounceSocket);
		CloseSocket(s_GameSocket);
		s_Error = "Can't host a game on this network.";
		return false;
	}
	s_GamePort = ntohs(address.sin_port);
	s_LastAnnouncement = {};
	s_State = State::Hosting;
	return true;
}

void NetLobby::SetHostedSetup(const Setup& setup) {
	if (!s_Starting) {
		s_Setup = setup;
	}
}

bool NetLobby::Join(const Game& game) {
	Leave();
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<uint16_t>(game.Port));
	s_GameSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (s_GameSocket < 0 || inet_pton(AF_INET, game.Address.c_str(), &address.sin_addr) != 1 || connect(s_GameSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
		CloseSocket(s_GameSocket);
		s_Error = "Can't connect to " + game.HostName + ".";
		return false;
	}
	s_PeerName = game.HostName;
	s_PeerLastHeardFrom = Clock::now();
	s_LastSend = {};
	s_State = State::Joined;
	return true;
}

void NetLobby::Leave() {
	if (s_GameSocket >= 0 && (s_State == State::Joined || s_HasPeer)) {
		std::string_view leave = "L leave";
		if (s_State == State::Joined) {
			send(s_GameSocket, leave.data(), leave.size(), 0);
		} else {
			sendto(s_GameSocket, leave.data(), leave.size(), 0, reinterpret_cast<sockaddr*>(&s_PeerAddress), sizeof(s_PeerAddress));
		}
	}
	CloseSocket(s_AnnounceSocket);
	CloseSocket(s_GameSocket);
	s_State = State::Idle;
	s_Games.clear();
	s_GamesLastHeardFrom.clear();
	s_HasPeer = false;
	s_PeerReady = false;
	s_Ready = false;
	s_Starting = false;
	s_GameIsToStart = false;
	s_Error.clear();
}

void NetLobby::StartGame(const Setup& setup) {
	if (s_State == State::Hosting && IsPeerReady()) {
		s_Setup = setup;
		s_Starting = true;
		s_LastSend = {};
	}
}

void NetLobby::Update() {
	auto now = Clock::now();
	char buffer[16384];

	if (s_State == State::Browsing) {
		sockaddr_in sender{};
		socklen_t senderSize = sizeof(sender);
		for (ssize_t received; (received = recvfrom(s_AnnounceSocket, buffer, sizeof(buffer), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&sender), &senderSize)) > 0; senderSize = sizeof(sender)) {
			std::vector<std::string> lines = SplitLines(std::string(buffer, static_cast<size_t>(received)));
			if (lines.size() < 6 || lines[0] != c_AnnounceMark) {
				continue;
			}
			char address[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address));
			Game game{address, std::atoi(lines[1].c_str()), lines[3].substr(0, 32), lines[4].substr(0, 64), lines[5].substr(0, 64), lines[2] == "1"};
			// A host on this same machine is heard both directly and over the network, and only has to be listed once.
			size_t index = 0;
			while (index < s_Games.size() && (s_Games[index].Port != game.Port || s_Games[index].HostName != game.HostName)) {
				++index;
			}
			if (index == s_Games.size() && s_Games.size() < 32) {
				s_Games.emplace_back();
				s_GamesLastHeardFrom.emplace_back();
			}
			if (index < s_Games.size()) {
				// The direct way to a host on this machine is the one that is sure to work.
				if (s_Games[index].Address == "127.0.0.1") {
					game.Address = s_Games[index].Address;
				}
				s_Games[index] = game;
				s_GamesLastHeardFrom[index] = now;
			}
		}
		for (size_t index = s_Games.size(); index-- > 0;) {
			if (now - s_GamesLastHeardFrom[index] > c_Timeout) {
				s_Games.erase(s_Games.begin() + static_cast<std::ptrdiff_t>(index));
				s_GamesLastHeardFrom.erase(s_GamesLastHeardFrom.begin() + static_cast<std::ptrdiff_t>(index));
			}
		}
	} else if (s_State == State::Hosting) {
		sockaddr_in sender{};
		socklen_t senderSize = sizeof(sender);
		for (ssize_t received; (received = recvfrom(s_GameSocket, buffer, sizeof(buffer), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&sender), &senderSize)) > 0; senderSize = sizeof(sender)) {
			std::string message(buffer, static_cast<size_t>(received));
			std::string controlScheme = TakeControlScheme(message);
			std::vector<std::string> lines = SplitLines(message);
			bool fromPeer = s_HasPeer && sender.sin_addr.s_addr == s_PeerAddress.sin_addr.s_addr && sender.sin_port == s_PeerAddress.sin_port;
			if (lines.empty()) {
				continue;
			} else if (lines[0] == "L join" && lines.size() >= 3 && (fromPeer || !s_HasPeer)) {
				s_HasPeer = true;
				s_PeerAddress = sender;
				s_PeerLastHeardFrom = now;
				s_PeerReady = lines[1] == "1";
				s_PeerName = lines[2].substr(0, 32);
				s_PeerControlScheme = controlScheme;
			} else if (lines[0] == "L join") {
				std::string_view full = "L full";
				sendto(s_GameSocket, full.data(), full.size(), 0, reinterpret_cast<sockaddr*>(&sender), senderSize);
			} else if (fromPeer && lines[0] == "L leave") {
				s_HasPeer = false;
				s_Starting = false;
			} else if (fromPeer && lines[0] == "L started" && s_Starting) {
				s_GameIsToStart = true;
			}
		}
		if (s_HasPeer && now - s_PeerLastHeardFrom > c_Timeout && !s_GameIsToStart) {
			s_HasPeer = false;
			s_Starting = false;
		}
		if (s_HasPeer && now - s_LastSend > c_SendInterval) {
			s_LastSend = now;
			std::string text = (s_Starting ? "L start\n" : "L lobby\n") + GetLocalName() + "\n" + s_Setup.ToText() + c_ControlSchemeMark + NetLockstep::GetOwnControlSchemeText();
			sendto(s_GameSocket, text.data(), text.size(), 0, reinterpret_cast<sockaddr*>(&s_PeerAddress), sizeof(s_PeerAddress));
		}
		if (now - s_LastAnnouncement > c_AnnounceInterval) {
			s_LastAnnouncement = now;
			Announce(std::string(c_AnnounceMark) + "\n" + std::to_string(s_GamePort) + "\n" + (s_HasPeer ? "1" : "0") + "\n" + GetLocalName() + "\n" + s_Setup.ActivityPreset.substr(s_Setup.ActivityPreset.find('/') + 1) + "\n" + s_Setup.ScenePreset.substr(s_Setup.ScenePreset.find('/') + 1));
		}
	} else if (s_State == State::Joined) {
		for (ssize_t received; (received = recv(s_GameSocket, buffer, sizeof(buffer), MSG_DONTWAIT)) > 0;) {
			std::string text(buffer, static_cast<size_t>(received));
			std::string controlScheme = TakeControlScheme(text);
			size_t nameStart = text.find('\n');
			size_t setupStart = nameStart == std::string::npos ? nameStart : text.find('\n', nameStart + 1);
			bool isStart = text.starts_with("L start\n");
			if (text == "L full" || text == "L leave") {
				s_Error = text == "L full" ? s_PeerName + "'s game is full." : s_PeerName + " closed the game.";
				std::string error = s_Error;
				Leave();
				s_Error = error;
				return;
			} else if ((isStart || text.starts_with("L lobby\n")) && setupStart != std::string::npos && !s_GameIsToStart) {
				Setup setup;
				if (setup.FromText(text.substr(setupStart + 1))) {
					s_HasPeer = true;
					s_PeerLastHeardFrom = now;
					s_PeerName = text.substr(nameStart + 1, setupStart - nameStart - 1).substr(0, 32);
					s_Setup = setup;
					s_PeerControlScheme = controlScheme;
					// The host only starts a game that this machine said it was ready for, but it may have changed its mind and that not have got there yet.
					s_GameIsToStart = isStart && s_Ready && setup.HasPlayersOnTeams() && HasContentFor(setup);
				}
			}
		}
		if (now - s_PeerLastHeardFrom > c_Timeout && !s_GameIsToStart) {
			std::string error = s_HasPeer ? "Lost the connection to " + s_PeerName + "." : s_PeerName + " didn't answer.";
			Leave();
			s_Error = error;
			return;
		}
		if (now - s_LastSend > c_SendInterval) {
			s_LastSend = now;
			std::string text = "L join\n" + std::string(s_Ready ? "1" : "0") + "\n" + GetLocalName() + c_ControlSchemeMark + NetLockstep::GetOwnControlSchemeText();
			send(s_GameSocket, text.data(), text.size(), 0);
		}
	}
}

std::string NetLobby::GetOwnControlsName() {
	InputDevice device = g_UInputMan.GetControlScheme(Players::PlayerOne)->GetDevice();
	return device == InputDevice::DEVICE_KEYB_ONLY ? "Keyboard" : (device == InputDevice::DEVICE_MOUSE_KEYB ? "Mouse + Keys" : "Gamepad");
}

void NetLobby::CycleOwnControls() {
	InputScheme* controlScheme = g_UInputMan.GetControlScheme(Players::PlayerOne);
	if (controlScheme->GetDevice() == InputDevice::DEVICE_KEYB_ONLY) {
		controlScheme->SetDevice(InputDevice::DEVICE_MOUSE_KEYB);
		controlScheme->SetPreset(InputScheme::InputPreset::PresetMouseWASDKeys);
	} else if (controlScheme->GetDevice() == InputDevice::DEVICE_MOUSE_KEYB) {
		controlScheme->SetDevice(InputDevice::DEVICE_GAMEPAD_1);
		controlScheme->SetPreset(InputScheme::InputPreset::PresetGenericDualAnalog);
	} else {
		controlScheme->SetDevice(InputDevice::DEVICE_KEYB_ONLY);
		controlScheme->SetPreset(InputScheme::InputPreset::PresetWASDKeys);
	}
}

bool NetLobby::HasContentFor(const Setup& setup) {
	return dynamic_cast<const GameActivity*>(g_PresetMan.GetEntityPreset(setup.ActivityClass, setup.ActivityPreset)) && g_PresetMan.GetEntityPreset("Scene", setup.ScenePreset);
}

bool NetLobby::LaunchGame() {
	const GameActivity* activityPreset = dynamic_cast<const GameActivity*>(g_PresetMan.GetEntityPreset(s_Setup.ActivityClass, s_Setup.ActivityPreset));
	const Scene* scene = dynamic_cast<const Scene*>(g_PresetMan.GetEntityPreset("Scene", s_Setup.ScenePreset));
	if (!s_GameIsToStart || !activityPreset || !scene || !s_Setup.HasPlayersOnTeams()) {
		return false;
	}

	// Both machines make the game from the setup alone, so that it comes out the same on both.
	GameActivity* gameActivity = dynamic_cast<GameActivity*>(activityPreset->Clone());
	gameActivity->SetDifficulty(s_Setup.Difficulty);
	gameActivity->SetStartingGold(s_Setup.StartingGold);
	gameActivity->SetRequireClearPathToOrbit(s_Setup.RequireClearPathToOrbit);
	gameActivity->SetFogOfWarEnabled(s_Setup.FogOfWar);
	g_SceneMan.SetSceneToLoad(scene, true, s_Setup.DeployUnits);
	gameActivity->ClearPlayers(false);
	for (int player = Players::PlayerOne; player <= Players::PlayerTwo; ++player) {
		gameActivity->AddPlayer(player, true, s_Setup.PlayerTeams[player], 0);
	}
	if (s_Setup.CPUTeam != Activity::Teams::NoTeam) {
		gameActivity->SetCPUTeam(s_Setup.CPUTeam);
	}
	for (int team = Activity::Teams::TeamOne; team < Activity::Teams::MaxTeamCount; ++team) {
		gameActivity->SetTeamTech(team, s_Setup.TeamTechs[team]);
		gameActivity->SetTeamAISkill(team, s_Setup.TeamAISkills[team]);
	}
	g_LuaMan.FileCloseAll();
	g_ActivityMan.SetStartActivity(gameActivity);

	if (s_State == State::Hosting) {
		connect(s_GameSocket, reinterpret_cast<sockaddr*>(&s_PeerAddress), sizeof(s_PeerAddress));
	} else {
		// The host keeps saying to start until it hears that this machine is, and NetLockstep answers it from here on.
		std::string_view started = "L started";
		send(s_GameSocket, started.data(), started.size(), 0);
	}
	// Each player plays with the controls that their own machine has for its first player.
	std::string ownControlScheme = NetLockstep::GetOwnControlSchemeText();
	NetLockstep::SetPlayerControlSchemeTexts(s_State == State::Hosting ? ownControlScheme : s_PeerControlScheme, s_State == State::Hosting ? s_PeerControlScheme : ownControlScheme);
	NetLockstep::StartWhenGameStarts(s_GameSocket, s_State == State::Hosting ? 0 : 1);
	// The connection is NetLockstep's now, and isn't to be closed or said goodbye over.
	s_GameSocket = -1;
	s_HasPeer = false;
	Leave();
	return true;
}
