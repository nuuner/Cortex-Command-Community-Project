#pragma once

#include "SDL3/SDL_events.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace RTE {

	/// Runs the simulation in lockstep with another instance of the game over the network. Both instances run the whole simulation, and only send each other their player's input.
	/// The input of each machine is held back for a few simulation updates, so that it gets to the other one in time for both to apply it in the same update.
	/// An update can't begin until the other machine's input for it has arrived, so the simulation stalls when the connection does.
	/// The two machines also compare hashes of their simulation state now and then, to find out if they have stopped matching.
	/// shortcut: one remote player over a TCP connection, on POSIX sockets. Move to RakNet, which is already linked, once more players, Windows or NAT punch-through are needed.
	/// shortcut: both machines have to be started with the same Activity, Scene, seed, game data and settings, nothing checks it. Add a handshake that compares them along with the lobby.
	class NetLockstep {

	public:
		/// Waits for the other machine to connect, and plays as player one.
		/// @param port The port to listen on.
		static void Host(const std::string& port);

		/// Connects to a hosting machine, and plays as player two.
		/// @param address The address and port of the host, as "address:port".
		static void Join(const std::string& address);

		/// Gets whether the simulation is running in lockstep with another machine.
		/// @return Whether lockstep is on.
		static bool IsActive() { return s_Active; }

		/// Gets whether the two machines were found to have diverged.
		/// @return Whether the simulation states stopped matching.
		static bool HasDesynced() { return s_Desynced; }

		/// Gets which player this machine is.
		/// @return The player.
		static int GetLocalPlayer() { return s_LocalPlayer; }

		/// Gets the ID of the made up mouse and keyboard that a player's input arrives on. It is the same on both machines.
		/// @param player The player.
		/// @return The device ID.
		static Uint32 GetPlayerDeviceID(int player) { return c_VirtualDeviceBase + player; }

		/// Takes an input event from this machine's devices away from UInputMan, to hand it back at the simulation update that both machines apply it in.
		/// @param inputEvent The input event.
		/// @return Whether the event was taken, and so shouldn't be handled now.
		static bool CaptureLocalInput(const SDL_Event& inputEvent);

		/// Has to be called at the start of each simulation update, before UInputMan updates.
		/// Sends this machine's input, waits for the other machine's input for this update, and applies the input of both.
		static void BeginSimUpdate();

		/// Closes the connection, after giving the other machine a moment to finish.
		static void Stop();

	private:
		static constexpr int c_PlayerCount = 2; //!< How many machines, and so players, there are.
		static constexpr long long c_InputDelay = 3; //!< How many simulation updates input is held back for.
		static constexpr long long c_HashInterval = 30; //!< How many simulation updates there are between the state hashes that get compared.
		static constexpr int c_WaitTimeoutS = 120; //!< How long to wait for the other machine's input before giving up on it. It has to cover the other machine loading the game.
		static constexpr Uint32 c_VirtualDeviceBase = 1000; //!< The mouse and keyboard ID of the first player. Each player gets a made up device, which is the same on both machines.

		static bool s_Active; //!< Whether lockstep is on.
		static bool s_Desynced; //!< Whether the simulation states stopped matching.
		static bool s_ApplyingInput; //!< Whether input is being handed back to UInputMan, and so shouldn't be captured.
		static int s_Socket; //!< The connection to the other machine, or -1 once it is gone.
		static int s_LocalPlayer; //!< Which player this machine is.
		static long long s_SimUpdate; //!< How many simulation updates have begun.
		static long long s_RemoteCompleteThrough; //!< The last simulation update that all of the other machine's input has arrived for.
		static long long s_MatchedHashCount; //!< How many state hashes have been compared and matched.
		static std::vector<std::string> s_PendingLines; //!< This machine's input since the last simulation update began.
		static std::map<long long, std::vector<std::string>> s_InputLines[c_PlayerCount]; //!< The input of each player, by the simulation update it gets applied in.
		static std::map<long long, uint64_t> s_StateHashes[c_PlayerCount]; //!< The state hashes of each machine that haven't been compared yet, by simulation update.
		static std::string s_ReceiveBuffer; //!< What has arrived from the other machine but isn't a whole line yet.

		/// Sets up the connection once it is made.
		/// @param socket The connected socket.
		/// @param localPlayer Which player this machine is.
		static void Start(int socket, int localPlayer);

		/// Sends text to the other machine. Drops the connection if that fails.
		/// @param text The text to send.
		static void Send(const std::string& text);

		/// Reads what has arrived from the other machine. Drops the connection if it has closed.
		/// @param timeoutMS How long to wait for something to arrive, in milliseconds.
		static void Receive(int timeoutMS);

		/// Handles a line from the other machine.
		/// @param line The line.
		static void HandleRemoteLine(const std::string& line);

		/// Compares the state hashes of the two machines for a simulation update, if both are known.
		/// @param simUpdate The simulation update.
		static void CompareStateHashes(long long simUpdate);

		/// Closes the connection.
		static void Disconnect();
	};
} // namespace RTE
