#pragma once

#include <string>
#include <vector>

namespace RTE {

	/// Gets two machines on the same local network together for a game of NetLockstep, from the menus.
	/// The machine that hosts keeps announcing its game to the network, and the other one picks it from the games that it hears of. The host then shows the other machine how the game is set up, and starts it once the other player is ready.
	/// shortcut: local network only, and two players. Finding games further away takes a server that both machines can reach, see the punch-through item of the todo.
	/// shortcut: nothing checks that both machines have the same version, mods and settings, other than that the joiner has the Activity and Scene. Compare them here once that bites.
	class NetLobby {

	public:
		/// A game that a machine on the network is hosting.
		struct Game {
			std::string Address; //!< The address of the host.
			int Port = 0; //!< The port that the host plays on.
			std::string HostName; //!< What the host calls itself.
			std::string Activity; //!< The name of the Activity that is set up.
			std::string Scene; //!< The name of the Scene that is set up.
			bool Full = false; //!< Whether someone has already joined it.
		};

		/// How a game is set up, which is all that the two machines need to agree on to start the same one.
		struct Setup {
			std::string ActivityClass;
			std::string ActivityPreset;
			std::string ScenePreset;
			int Difficulty = 0;
			int StartingGold = 0;
			bool FogOfWar = false;
			bool RequireClearPathToOrbit = false;
			bool DeployUnits = true;
			int PlayerTeams[2] = {0, 1}; //!< The team of the host and of the joiner.
			int CPUTeam = -1;
			std::string TeamTechs[4];
			int TeamAISkills[4] = {};

			bool operator==(const Setup&) const = default;

			/// Writes the setup out for sending to the other machine.
			std::string ToText() const;

			/// Reads a setup that the other machine sent.
			/// @return Whether it was all there.
			bool FromText(const std::string& text);

			/// Gets whether both players are on a team, which the host may still be getting to.
			bool HasPlayersOnTeams() const;
		};

		/// Starts listening for the games that are being hosted on the network. Does nothing if already listening.
		static void StartBrowsing();

		/// Gets the games that are being hosted on the network right now.
		static const std::vector<Game>& GetGames() { return s_Games; }

		/// Starts hosting a game, and announcing it to the network.
		/// @return Whether that worked.
		static bool StartHosting();

		/// Sets how the hosted game is set up, for the other player to see.
		static void SetHostedSetup(const Setup& setup);

		/// Joins a game that is being hosted.
		/// @return Whether that could be tried. It takes hearing from the host for it to have worked, see HasPeer.
		static bool Join(const Game& game);

		/// Sets whether this machine's player is ready to start, when it joined a game.
		static void SetReady(bool ready) { s_Ready = ready; }

		/// Gets whether this machine's player said to be ready to start.
		static bool IsReady() { return s_Ready; }

		/// Stops browsing, hosting or being joined to a game, and lets the other machine know.
		static void Leave();

		/// Sends and receives what there is to. Has to be called every frame while the menus that use this are showing.
		static void Update();

		static bool IsHosting() { return s_State == State::Hosting; }
		static bool IsJoined() { return s_State == State::Joined; }

		/// Gets whether the other machine is there: the one that joined when hosting, or the host when joined.
		static bool HasPeer() { return s_HasPeer; }

		/// Gets what the other machine calls itself.
		static const std::string& GetPeerName() { return s_PeerName; }

		/// Gets whether the player that joined is ready to start.
		static bool IsPeerReady() { return s_HasPeer && s_PeerReady; }

		/// Gets how the game is set up: by this machine when hosting, or by the host when joined.
		static const Setup& GetSetup() { return s_Setup; }

		/// Has the host start the game, as it is set up, once the other machine has confirmed that it is starting it too.
		static void StartGame(const Setup& setup);

		/// Gets whether the host is waiting for the other machine to confirm that it is starting the game.
		static bool IsStarting() { return s_Starting; }

		/// Gets whether both machines are starting the game, so that it is time for LaunchGame.
		static bool GameIsToStart() { return s_GameIsToStart; }

		/// Sets the game up to be started, as ActivityMan::SetStartActivity does, and hands the connection over to NetLockstep.
		/// @return Whether that worked. It doesn't if this machine doesn't have the Activity or Scene.
		static bool LaunchGame();

		/// Gets whether this machine has the Activity and Scene of a setup.
		static bool HasContentFor(const Setup& setup);

		/// Gets what this machine's player plays a game with: the device of player 1's control scheme, as a few words to show.
		static std::string GetOwnControlsName();

		/// Changes what this machine's player plays with to the next kind of device: keyboard, mouse and keyboard, or gamepad.
		/// shortcut: this sets player 1's control scheme to that device's usual keys or buttons, replacing mappings of their own. Leave those be if someone minds.
		static void CycleOwnControls();

		/// Gets what the simulation depends on that this machine could have differently from another one: the game version, the mods, the resolution and some settings.
		/// @return A line for each of those, as what it is called, a tab, and what this machine has for it.
		static std::string GetAgreementText();

		/// Finds what two machines have differently, out of what their simulations depend on.
		/// @param ownText What this machine has, as GetAgreementText gives it.
		/// @param peerText What the other machine has.
		/// @param peerName What the other machine calls itself.
		/// @return The first difference as a sentence to show to the player, or nothing if a game between the two can stay in sync.
		static std::string DescribeDisagreement(const std::string& ownText, const std::string& peerText, const std::string& peerName);

		/// Gets why this machine and the other one can't play together, if they can't.
		/// @return The reason as a sentence to show to the player, or nothing if they can, or if the other machine isn't there.
		static std::string GetDisagreement();

		/// Gets what went wrong last, to show to the player.
		static const std::string& GetError() { return s_Error; }

	private:
		enum class State { Idle, Browsing, Hosting, Joined };

		static State s_State;
		static std::vector<Game> s_Games;
		static Setup s_Setup;
		static bool s_HasPeer;
		static std::string s_PeerName;
		static bool s_PeerReady;
		static bool s_Ready;
		static bool s_Starting;
		static bool s_GameIsToStart;
		static std::string s_Error;
	};
} // namespace RTE
