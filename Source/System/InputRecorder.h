#pragma once

#include "SDL3/SDL_events.h"

#include <fstream>
#include <string>
#include <vector>

namespace RTE {

	/// Records the input that each simulation update gets to a file, or plays it back from one in place of the real input devices.
	/// Each line of the file is the number of the simulation update the input goes to, counted from the first one of the run, then a letter for what kind of input it is, then its values.
	/// shortcut: gamepads aren't recorded, and the menu loop isn't either so pausing ends a replay. Add both when players other than mouse and keyboard ones or pausing are needed.
	/// shortcut: mouse positions are recorded in window pixels, so a replay needs the same resolution and window setup. Record them in scene units once two machines have to agree.
	class InputRecorder {

	public:
		/// Starts recording input to a file.
		/// @param filePath The path of the file to write.
		static void StartRecording(const std::string& filePath);

		/// Starts playing back input from a previously recorded file. Input from the real devices is ignored from then on.
		/// @param filePath The path of the file to read.
		static void StartReplaying(const std::string& filePath);

		/// Gets whether input is being recorded or played back. The simulation can't depend on anything that the input file doesn't hold while it is, such as how much real time has passed.
		/// @return Whether input is being recorded or played back.
		static bool IsActive() { return s_Recording || s_Replaying; }

		/// Gets whether input is being played back from a file.
		/// @return Whether input is being played back.
		static bool IsReplaying() { return s_Replaying; }

		/// Records an input event that UInputMan is about to handle. Does nothing unless recording.
		/// @param inputEvent The input event.
		static void Record(const SDL_Event& inputEvent);

		/// Passes on the game window gaining or losing input focus to UInputMan, and records it. Does nothing while replaying, since the recorded focus changes are used then.
		/// @param focused Whether the window now has focus.
		static void HandleFocusChange(bool focused);

		/// Gets whether the recorded run ended with the simulation update that was just played back, so that this run has to end here too.
		/// @return Whether the recording being played back is over.
		static bool ReplayIsOver() { return s_ReplayIsOver; }

		/// Marks where the run ended in the recording, so that playing it back ends at the same simulation update.
		static void Stop();

		/// Has to be called at the start of each simulation update, before UInputMan updates. Plays back the input recorded for this update, and writes out the input recorded since the last one.
		static void BeginSimUpdate();

	private:
		static bool s_Recording; //!< Whether input is being recorded.
		static bool s_Replaying; //!< Whether input is being played back.
		static std::ofstream s_RecordFile; //!< The file being recorded to.
		static std::ifstream s_ReplayFile; //!< The file being played back from.
		static bool s_ReplayIsOver; //!< Whether the recorded run ended with the simulation update being played back.
		static long long s_SimUpdate; //!< How many simulation updates have begun.
		static std::vector<std::string> s_PendingLines; //!< What has been recorded since the last simulation update began, without the update numbers.
		static long long s_NextReplayUpdate; //!< The simulation update that the next line to play back is for, or -1 if there are none left.
		static std::string s_NextReplayLine; //!< The next line to play back, without its update number.

		/// Reads the next line to play back from the replay file.
		static void ReadNextReplayLine();

		/// Applies a recorded line, as if the input had just come from the real devices.
		/// @param line The line, without its update number.
		static void ApplyLine(const std::string& line);

		/// Applies a change of input focus to UInputMan, and records it.
		/// @param focused Whether the window now has focus.
		static void ApplyFocusChange(bool focused);
	};
} // namespace RTE
