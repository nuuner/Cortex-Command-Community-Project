#include "InputRecorder.h"

#include "UInputMan.h"

#include "NetLockstep.h"

#include "RTEError.h"

#include <cstdlib>
#include <format>
#include <sstream>

using namespace RTE;

bool InputRecorder::s_Recording = false;
bool InputRecorder::s_Replaying = false;
std::ofstream InputRecorder::s_RecordFile;
std::ifstream InputRecorder::s_ReplayFile;
bool InputRecorder::s_ReplayIsOver = false;
long long InputRecorder::s_SimUpdate = 0;
std::vector<std::string> InputRecorder::s_PendingLines;
long long InputRecorder::s_NextReplayUpdate = -1;
std::string InputRecorder::s_NextReplayLine;

void InputRecorder::StartRecording(const std::string& filePath) {
	s_RecordFile.open(filePath, std::ios::out | std::ios::trunc);
	if (!s_RecordFile.is_open()) {
		// Carrying on without recording would look just like a run that had no input.
		RTEError::ShowMessageBox("Failed to open the input recording file \"" + filePath + "\"!");
		std::exit(EXIT_FAILURE);
	}
	s_Recording = true;
}

void InputRecorder::StartReplaying(const std::string& filePath) {
	s_ReplayFile.open(filePath);
	if (!s_ReplayFile.is_open()) {
		RTEError::ShowMessageBox("Failed to open the input replay file \"" + filePath + "\"!");
		std::exit(EXIT_FAILURE);
	}
	s_Replaying = true;
	ReadNextReplayLine();
}

bool InputRecorder::IsActive() {
	return s_Recording || s_Replaying || NetLockstep::IsActive();
}

std::string InputRecorder::EventToLine(const SDL_Event& inputEvent) {
	switch (inputEvent.type) {
		case SDL_EVENT_KEY_UP:
		case SDL_EVENT_KEY_DOWN:
			return std::format("k {} {} {}", inputEvent.key.which, static_cast<int>(inputEvent.key.scancode), static_cast<int>(inputEvent.key.down));
		case SDL_EVENT_MOUSE_MOTION:
			return std::format("m {} {} {} {} {}", inputEvent.motion.which, inputEvent.motion.x, inputEvent.motion.y, inputEvent.motion.xrel, inputEvent.motion.yrel);
		case SDL_EVENT_MOUSE_BUTTON_UP:
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			return std::format("b {} {} {}", inputEvent.button.which, static_cast<int>(inputEvent.button.button), static_cast<int>(inputEvent.button.down));
		case SDL_EVENT_MOUSE_WHEEL:
			return std::format("w {} {}", inputEvent.wheel.which, inputEvent.wheel.direction == SDL_MOUSEWHEEL_NORMAL ? inputEvent.wheel.y : -inputEvent.wheel.y);
		case SDL_EVENT_TEXT_INPUT:
			return std::format("t {}", inputEvent.text.text);
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			return std::format("j a {} {} {}", inputEvent.gaxis.which, static_cast<int>(inputEvent.gaxis.axis), static_cast<int>(inputEvent.gaxis.value));
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
			return std::format("j b {} {} {}", inputEvent.gbutton.which, static_cast<int>(inputEvent.gbutton.button), static_cast<int>(inputEvent.gbutton.down));
		default:
			return "";
	}
}

void InputRecorder::Record(const SDL_Event& inputEvent) {
	if (!s_Recording) {
		return;
	}
	if (std::string line = EventToLine(inputEvent); !line.empty()) {
		s_PendingLines.emplace_back(std::move(line));
	}
}

void InputRecorder::HandleFocusChange(bool focused) {
	// Only input that every machine gets can reach the simulation when it runs in lockstep, and the focus of this machine's window isn't.
	if (!s_Replaying && !NetLockstep::IsActive()) {
		ApplyFocusChange(focused);
	}
}

void InputRecorder::ApplyFocusChange(bool focused) {
	if (s_Recording) {
		s_PendingLines.emplace_back(std::format("f {}", static_cast<int>(focused)));
	}
	g_UInputMan.DisableMouseMoving(!focused);
	g_UInputMan.DisableKeys(!focused);
}

void InputRecorder::Stop() {
	if (s_Recording) {
		s_RecordFile << s_SimUpdate << " q\n";
		s_RecordFile.close();
		s_Recording = false;
	}
}

void InputRecorder::BeginSimUpdate() {
	++s_SimUpdate;

	while (s_Replaying && s_NextReplayUpdate >= 0 && s_NextReplayUpdate <= s_SimUpdate) {
		ApplyLine(s_NextReplayLine);
		ReadNextReplayLine();
	}

	if (s_Recording && !s_PendingLines.empty()) {
		for (const std::string& line: s_PendingLines) {
			s_RecordFile << s_SimUpdate << ' ' << line << '\n';
		}
		// The game doesn't always get to quit in a way that would close the file.
		s_RecordFile.flush();
		s_PendingLines.clear();
	}
}

void InputRecorder::ReadNextReplayLine() {
	s_NextReplayUpdate = -1;
	std::string line;
	if (std::getline(s_ReplayFile, line)) {
		size_t updateEnd = line.find(' ');
		if (updateEnd != std::string::npos) {
			s_NextReplayUpdate = std::atoll(line.c_str());
			s_NextReplayLine = line.substr(updateEnd + 1);
		}
	}
}

void InputRecorder::ApplyLine(const std::string& line) {
	std::istringstream values(line);
	char kind = 0;
	values >> kind;

	SDL_Event inputEvent{};
	int down = 0;
	switch (kind) {
		case 'k': {
			int scancode = 0;
			values >> inputEvent.key.which >> scancode >> down;
			if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT) {
				return;
			}
			inputEvent.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
			inputEvent.key.scancode = static_cast<SDL_Scancode>(scancode);
			inputEvent.key.down = down;
			break;
		}
		case 'm':
			inputEvent.type = SDL_EVENT_MOUSE_MOTION;
			values >> inputEvent.motion.which >> inputEvent.motion.x >> inputEvent.motion.y >> inputEvent.motion.xrel >> inputEvent.motion.yrel;
			break;
		case 'b': {
			int button = 0;
			values >> inputEvent.button.which >> button >> down;
			if (button < SDL_BUTTON_LEFT || button > SDL_BUTTON_RIGHT) {
				return;
			}
			inputEvent.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			inputEvent.button.button = static_cast<Uint8>(button);
			inputEvent.button.down = down;
			break;
		}
		case 'w':
			inputEvent.type = SDL_EVENT_MOUSE_WHEEL;
			inputEvent.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
			values >> inputEvent.wheel.which >> inputEvent.wheel.y;
			break;
		case 't':
			inputEvent.type = SDL_EVENT_TEXT_INPUT;
			// The text is everything after the kind and the space that follows it, and this line outlives the handling of the event.
			inputEvent.text.text = line.c_str() + std::min<size_t>(2, line.size());
			break;
		case 'j': {
			// A gamepad's stick or trigger moving, or one of its buttons going down or up.
			char part = 0;
			SDL_JoystickID which = 0;
			int index = 0;
			int value = 0;
			values >> part >> which >> index >> value;
			if (part == 'a' && index >= 0 && index < SDL_GAMEPAD_AXIS_COUNT) {
				inputEvent.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
				inputEvent.gaxis.which = which;
				inputEvent.gaxis.axis = static_cast<Uint8>(index);
				inputEvent.gaxis.value = static_cast<Sint16>(std::clamp(value, -32768, 32767));
			} else if (part == 'b' && index >= 0 && index < SDL_GAMEPAD_BUTTON_COUNT) {
				inputEvent.type = value ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
				inputEvent.gbutton.which = which;
				inputEvent.gbutton.button = static_cast<Uint8>(index);
				inputEvent.gbutton.down = value != 0;
			} else {
				return;
			}
			break;
		}
		case 'f':
			values >> down;
			ApplyFocusChange(down);
			return;
		case 'q':
			s_ReplayIsOver = true;
			return;
		default:
			return;
	}
	if (values.fail()) {
		return;
	}
	g_UInputMan.HandleInputEvent(inputEvent);
}
