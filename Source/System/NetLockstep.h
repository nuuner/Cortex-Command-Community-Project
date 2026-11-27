#pragma once

#include "SDL3/SDL_events.h"

#include <algorithm>
#include <string>

namespace RTE {

	/// Runs the simulation in lockstep with another instance of the game over the network. Both instances run the whole simulation, and only send each other their player's input.
	/// The input of each machine is held back for a few simulation updates, so that it gets to the other one in time for both to apply it in the same update.
	/// An update can't begin until the other machine's input for it has arrived, so the simulation stalls when the connection does.
	/// Messages can get lost on the way, so each one repeats all the input that the other machine hasn't confirmed having, and a lost one is made up for by the next.
	/// The two machines also compare hashes of their simulation state now and then, to find out if they have stopped matching.
	/// When they have, the host has both machines stop at the start of the same simulation update, and gets the other machine matching it again there, changing no more than it has to.
	/// shortcut: one remote player. Give each player their own connection and input once more are needed.
	/// shortcut: plain UDP on POSIX sockets, so no Windows and no way through home routers. The linked RakNet would cover both, but it crashes in its own network thread on arm64 macOS as soon as two peers connect.
	/// shortcut: both machines have to be started with the same Activity, Scene, seed, game data and settings, nothing checks it. Add a handshake that compares them along with the lobby.
	class NetLockstep {

	public:
		/// Sets this machine to wait for the other one to connect, and play as player one.
		/// @param port The port to listen on.
		static void SetHostPort(const std::string& port) { s_HostPort = port; }

		/// Sets this machine to connect to a hosting machine, and play as player two.
		/// @param address The address and port of the host, as "address:port".
		static void SetJoinAddress(const std::string& address) { s_JoinAddress = address; }

		/// Sets how many simulation updates this machine's input is held back for, instead of having it adapt to the connection as it goes.
		/// @param inputDelay The number of simulation updates.
		static void SetInputDelay(long long inputDelay) {
			s_InputDelay = std::clamp(inputDelay, 1LL, c_MaxInputDelay);
			s_InputDelayIsFixed = true;
		}

		/// Sets how long everything this machine sends is held back for, to try out how a slower connection plays.
		/// @param latencyMS The time to hold it back for, in milliseconds. A ping is twice this, when both machines are set to it.
		static void SetArtificialLatency(int latencyMS) { s_ArtificialLatencyMS = std::max(0, latencyMS); }

		/// Sets how much of what this machine sends is thrown away, to try out how a worse connection plays.
		/// @param lossPercent How many of each hundred messages to throw away.
		static void SetArtificialLoss(int lossPercent) { s_ArtificialLossPercent = std::clamp(lossPercent, 0, 90); }

		/// Sets the host to rebuild both machines from its own game at a simulation update, as it does when the other machine can't be corrected with less. This is for testing that.
		/// @param simUpdate The simulation update to do it at.
		static void SetForcedResyncUpdate(long long simUpdate) { s_ForcedResyncUpdate = simUpdate; }

		/// Sets this machine to throw its simulation off at a simulation update, so that the machines stop matching. This is for testing what happens then.
		/// @param simUpdate The simulation update to do it at.
		static void SetForcedDesyncUpdate(long long simUpdate) { s_ForcedDesyncUpdate = simUpdate; }

		/// Makes the connection that was asked for with SetHostPort or SetJoinAddress, if any. This doesn't return until the other machine is there.
		static void Connect();

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

		/// Gets a line about how the connection is doing, to show to the player.
		/// @return The ping and the input delay, as text.
		static std::string GetStatusText();

		/// Takes an input event from this machine's devices away from UInputMan, to hand it back at the simulation update that both machines apply it in.
		/// @param inputEvent The input event.
		/// @return Whether the event was taken, and so shouldn't be handled now.
		static bool CaptureLocalInput(const SDL_Event& inputEvent);

		/// Has to be called at the start of each simulation update, before UInputMan updates.
		/// Sends this machine's input, waits for the other machine's input for this update, and applies the input of both.
		static void BeginSimUpdate();

		/// Closes the connection.
		static void Stop();

	private:
		static constexpr long long c_MaxInputDelay = 30; //!< The most simulation updates that input can be held back for.
		static constexpr Uint32 c_VirtualDeviceBase = 1000; //!< The mouse and keyboard ID of the first player. Each player gets a made up device, which is the same on both machines.

		static std::string s_HostPort; //!< The port to host on, if this machine is to host.
		static std::string s_JoinAddress; //!< The address to join, if this machine is to join.
		static long long s_InputDelay; //!< How many simulation updates this machine's input is held back for.
		static bool s_InputDelayIsFixed; //!< Whether the input delay was set, and so isn't adapted to the connection.
		static int s_ArtificialLatencyMS; //!< How long everything this machine sends is held back for, in milliseconds.
		static int s_ArtificialLossPercent; //!< How many of each hundred messages this machine sends are thrown away.
		static bool s_Active; //!< Whether lockstep is on.
		static bool s_Desynced; //!< Whether the simulation states stopped matching.
		static int s_LocalPlayer; //!< Which player this machine is.
		static long long s_ForcedResyncUpdate; //!< The simulation update at which the host rebuilds the machines from its own game for testing, or -1.
		static long long s_ForcedDesyncUpdate; //!< The simulation update at which this machine throws its simulation off for testing, or -1.

		/// Measures how long the connection takes, and fits the input delay of this machine to it. Each machine does this for its own input, without the other having to agree.
		/// @return The line that asks the other machine to answer for the measurement, when it is time for one.
		static std::string AdaptInputDelay();

		/// Sends the other machine everything it may still be missing: the input it hasn't confirmed having, how far this machine has its input, and the latest state hashes.
		/// @param extraLines Lines that only get sent this once, since nothing depends on them getting there.
		static void SendState(const std::string& extraLines = "");

		/// Sends a message to the other machine, after the artificial latency and loss if there are any.
		/// @param text The message.
		static void SendToPeer(const std::string& text);

		/// Reads the messages that have arrived from the other machine.
		static void Receive();

		/// Handles a message from the other machine.
		/// @param text The message.
		static void HandleMessage(const std::string& text);

		/// Has the host get the other machine matching it again, at the start of a simulation update that both machines are at.
		/// The other machine first gets to set the state of its objects to the host's, which changes no more than what had gone off. If it no longer has the same objects, both machines rebuild their game from the host's.
		/// @return Whether the machines match again.
		static bool CorrectOtherMachine();

		/// Uses what has arrived in full from the host, if it is for the simulation update that this machine is at the start of.
		/// @return Whether this machine's game was rebuilt, and the simulation updates are being counted over.
		static bool UseReceivedBlob();

		/// Rebuilds this machine's game from the host's, which both machines do at the start of the same simulation update. The simulation updates are counted over from there.
		/// @return Whether that worked.
		static bool RebuildGame();

		/// Starts counting the simulation updates over, forgetting all input and hashes from before.
		/// @param epoch Which time this is.
		static void StartEpoch(int epoch);

		/// Forgets the state hashes so far, which are no longer of use once the state has been set to the host's.
		static void ForgetStateHashes();

		/// Sends the other machine something too big for one message, in parts, and sends each part again until the other machine has confirmed it. This doesn't return until it has all of them.
		/// @param blob What to send.
		/// @return Whether the other machine got all of it.
		static bool SendBlob(const std::string& blob);

		/// Handles a part of something big arriving, or the other machine confirming one.
		/// @param text The message.
		static void HandleBlobPart(const std::string& text);

		/// Compares the state hashes of the two machines for a simulation update, if both are known.
		/// @param simUpdate The simulation update.
		static void CompareStateHashes(long long simUpdate);
	};
} // namespace RTE
