#include "LANGamesGUI.h"

#include "GameActivity.h"
#include "NetLobby.h"
#include "NetLockstep.h"
#include "PresetMan.h"
#include "WindowMan.h"

#include "GUI.h"
#include "AllegroScreen.h"
#include "GUIInputWrapper.h"
#include "GUICollectionBox.h"
#include "GUILabel.h"
#include "GUIButton.h"
#include "GUIListBox.h"

using namespace RTE;

LANGamesGUI::LANGamesGUI(AllegroScreen* guiScreen, GUIInputWrapper* guiInput) {
	m_GUIControlManager = std::make_unique<GUIControlManager>();
	RTEAssert(m_GUIControlManager->Create(guiScreen, guiInput, "Base.rte/GUIs/Skins/Menus", "MainMenuSubMenuSkin.ini"), "Failed to create GUI Control Manager and load it from Base.rte/GUIs/Skins/Menus/MainMenuSubMenuSkin.ini");
	m_GUIControlManager->Load("Base.rte/GUIs/LANGamesGUI.ini");

	int rootBoxMaxWidth = g_WindowMan.FullyCoversAllDisplays() ? g_WindowMan.GetPrimaryWindowDisplayWidth() / g_WindowMan.GetResMultiplier() : g_WindowMan.GetResX();

	GUICollectionBox* rootBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("root"));
	rootBox->Resize(rootBoxMaxWidth, g_WindowMan.GetResY());

	m_GamesBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxGames"));
	m_LobbyBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxLobby"));
	for (GUICollectionBox* box: {m_GamesBox, m_LobbyBox}) {
		box->CenterInParent(true, true);
		box->SetPositionAbs(box->GetXPos(), (rootBox->GetHeight() < 540) ? box->GetYPos() - 15 : 140);
	}
	m_LobbyBox->SetVisible(false);

	m_BackToMainButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonBackToMainMenu"));
	m_BackToMainButton->SetPositionAbs((rootBox->GetWidth() - m_BackToMainButton->GetWidth()) / 2, m_GamesBox->GetYPos() + m_GamesBox->GetHeight() + 10);

	m_GamesListBox = dynamic_cast<GUIListBox*>(m_GUIControlManager->GetControl("ListBoxGames"));
	m_GamesListBox->SetMouseScrolling(true);
	m_GamesListBox->SetScrollBarThickness(15);
	m_GamesListBox->SetScrollBarPadding(2);

	m_JoinGameButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonJoinGame"));
	m_GamesStatusLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelGamesStatus"));
	m_LobbySetupLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelLobbySetup"));
	m_LobbyPlayersLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelLobbyPlayers"));
	m_LobbyReadyLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelLobbyReady"));
	m_LobbyStatusLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelLobbyStatus"));
	m_ControlsButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonControls"));
	m_ReadyButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonReady"));
	m_LeaveGameButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonLeaveGame"));
}

void LANGamesGUI::UpdateGamesList() {
	const std::vector<NetLobby::Game>& games = NetLobby::GetGames();
	std::string listedGames;
	for (const NetLobby::Game& game: games) {
		listedGames += game.Address + ":" + std::to_string(game.Port) + game.HostName + game.Activity + game.Scene + (game.Full ? "1" : "0") + "\n";
	}
	if (listedGames != m_ListedGames) {
		m_ListedGames = listedGames;
		int selectedIndex = m_GamesListBox->GetSelectedIndex();
		m_GamesListBox->ClearList();
		for (size_t i = 0; i < games.size(); ++i) {
			m_GamesListBox->AddItem(games[i].HostName + "  -  " + games[i].Activity + " on " + games[i].Scene, games[i].Full ? "Full" : "Open", nullptr, nullptr, static_cast<int>(i));
		}
		m_GamesListBox->SetSelectedIndex(games.empty() ? -1 : std::clamp(selectedIndex, 0, static_cast<int>(games.size()) - 1));
	}

	int selectedIndex = m_GamesListBox->GetSelectedIndex();
	m_JoinGameButton->SetEnabled(selectedIndex >= 0 && selectedIndex < static_cast<int>(games.size()) && !games[selectedIndex].Full);

	if (!NetLobby::GetError().empty()) {
		m_GamesStatusLabel->SetText(NetLobby::GetError());
	} else if (!NetLockstep::GetLastGameEnding().empty()) {
		m_GamesStatusLabel->SetText(NetLockstep::GetLastGameEnding());
	} else if (games.empty()) {
		m_GamesStatusLabel->SetText("Looking for games...\nTo host one, set up a Scenario Battle and tick Host LAN Game.");
	} else {
		m_GamesStatusLabel->SetText("");
	}
}

void LANGamesGUI::UpdateLobby() {
	const NetLobby::Setup& setup = NetLobby::GetSetup();
	const GameActivity* activity = dynamic_cast<const GameActivity*>(g_PresetMan.GetEntityPreset(setup.ActivityClass, setup.ActivityPreset));
	bool hasContent = NetLobby::HasContentFor(setup);
	auto presetName = [](const std::string& moduleAndPresetName) { return moduleAndPresetName.substr(moduleAndPresetName.find('/') + 1); };
	auto teamName = [&](int team) { return team == Activity::Teams::NoTeam ? std::string("no team yet") : (activity ? activity->GetTeamName(team) : "Team " + std::to_string(team + 1)); };

	if (!NetLobby::HasPeer()) {
		m_LobbySetupLabel->SetText("");
		m_LobbyPlayersLabel->SetText("");
		m_LobbyReadyLabel->SetText("");
		m_LobbyStatusLabel->SetText("Connecting to " + NetLobby::GetPeerName() + "...");
		m_ReadyButton->SetVisible(false);
		return;
	}

	std::string gold = setup.StartingGold >= 1000000000 ? "Infinite" : std::to_string(setup.StartingGold) + " oz";
	m_LobbySetupLabel->SetText(
	    "Activity:  " + presetName(setup.ActivityPreset) + "\n" +
	    "Scene:  " + presetName(setup.ScenePreset) + "\n" +
	    "Difficulty:  " + Activity::GetDifficultyString(setup.Difficulty) + "\n" +
	    "Starting gold:  " + gold + "\n" +
	    "Fog of war:  " + (setup.FogOfWar ? "On" : "Off") + "     Deploy units:  " + (setup.DeployUnits ? "On" : "Off"));

	std::string players = NetLobby::GetPeerName() + " (host)  -  " + teamName(setup.PlayerTeams[0]) + "\nYou  -  " + teamName(setup.PlayerTeams[1]);
	std::string ready = "Sets up the game\n" + std::string(NetLobby::IsReady() ? "Ready" : "Not ready");
	if (setup.CPUTeam != Activity::Teams::NoTeam) {
		players += "\nCPU  -  " + teamName(setup.CPUTeam);
	}
	m_LobbyPlayersLabel->SetText(players);
	m_LobbyReadyLabel->SetText(ready);

	std::string disagreement = NetLobby::GetDisagreement();
	if (!disagreement.empty()) {
		m_LobbyStatusLabel->SetText("You can't play this game: " + disagreement);
		hasContent = false;
	} else if (!hasContent) {
		m_LobbyStatusLabel->SetText("You don't have this Activity or Scene, so you can't play this game.");
	} else if (!setup.HasPlayersOnTeams()) {
		m_LobbyStatusLabel->SetText(NetLobby::GetPeerName() + " is still putting the players on teams.");
	} else if (NetLobby::IsReady()) {
		m_LobbyStatusLabel->SetText("Waiting for " + NetLobby::GetPeerName() + " to start the game...");
	} else {
		m_LobbyStatusLabel->SetText("Press Ready when you are good to go.");
	}
	m_ReadyButton->SetVisible(hasContent);
	m_ReadyButton->SetText(NetLobby::IsReady() ? "Not Ready" : "Ready");
	m_ControlsButton->SetText("Controls: " + NetLobby::GetOwnControlsName());
	if (!hasContent) {
		NetLobby::SetReady(false);
	}
}

LANGamesGUI::UpdateResult LANGamesGUI::Update() {
	NetLobby::Update();
	// Leaving a lobby or getting dropped from one leads back to looking for games.
	NetLobby::StartBrowsing();

	bool inLobby = NetLobby::IsJoined();
	if (m_LobbyBox->GetVisible() != inLobby) {
		m_LobbyBox->SetVisible(inLobby);
		m_GamesBox->SetVisible(!inLobby);
		m_BackToMainButton->SetVisible(!inLobby);
		m_ListedGames = "\n";
	}
	if (inLobby) {
		UpdateLobby();
		if (NetLobby::GameIsToStart()) {
			if (NetLobby::LaunchGame()) {
				g_GUISound.ExitMenuSound()->Play();
				return UpdateResult::GameStarted;
			}
			NetLobby::Leave();
		}
	} else {
		UpdateGamesList();
	}

	m_GUIControlManager->Update();

	GUIEvent guiEvent;
	while (m_GUIControlManager->GetEvent(&guiEvent)) {
		if (guiEvent.GetType() == GUIEvent::Command) {
			if (guiEvent.GetControl() == m_BackToMainButton) {
				NetLobby::Leave();
				return UpdateResult::BackToMain;
			} else if (guiEvent.GetControl() == m_JoinGameButton) {
				int selectedIndex = m_GamesListBox->GetSelectedIndex();
				if (selectedIndex >= 0 && selectedIndex < static_cast<int>(NetLobby::GetGames().size())) {
					g_GUISound.ButtonPressSound()->Play();
					NetLockstep::ClearLastGameEnding();
					// The game is copied, since joining it stops the browsing that the list of games belongs to.
					NetLobby::Game game = NetLobby::GetGames()[selectedIndex];
					NetLobby::Join(game);
				}
			} else if (guiEvent.GetControl() == m_ControlsButton) {
				g_GUISound.ButtonPressSound()->Play();
				NetLobby::CycleOwnControls();
			} else if (guiEvent.GetControl() == m_ReadyButton) {
				g_GUISound.ButtonPressSound()->Play();
				NetLobby::SetReady(!NetLobby::IsReady());
			} else if (guiEvent.GetControl() == m_LeaveGameButton) {
				g_GUISound.BackButtonPressSound()->Play();
				NetLobby::Leave();
			}
		} else if (guiEvent.GetType() == GUIEvent::Notification) {
			if (guiEvent.GetMsg() == GUIButton::Focused && dynamic_cast<GUIButton*>(guiEvent.GetControl())) {
				g_GUISound.SelectionChangeSound()->Play();
			} else if (guiEvent.GetControl() == m_GamesListBox && guiEvent.GetMsg() == GUIListBox::Select) {
				g_GUISound.SelectionChangeSound()->Play();
			}
		}
	}
	return UpdateResult::NoEvent;
}

void LANGamesGUI::Draw() const {
	m_GUIControlManager->Draw();
}
