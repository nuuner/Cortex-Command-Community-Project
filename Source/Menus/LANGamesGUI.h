#pragma once

#include <memory>
#include <string>

namespace RTE {

	class AllegroScreen;
	class GUIInputWrapper;
	class GUIControlManager;
	class GUICollectionBox;
	class GUILabel;
	class GUIButton;
	class GUIListBox;

	/// The menu screen that lists the games being hosted on the local network, and the lobby of the one that was joined. Hosting is done from the scenario menu, see ScenarioActivityConfigGUI.
	class LANGamesGUI {

	public:
		/// What came of updating the screen.
		enum class UpdateResult {
			NoEvent,
			BackToMain,
			GameStarted
		};

#pragma region Creation
		/// Constructor method used to instantiate a LANGamesGUI object in system memory and make it ready for use.
		/// @param guiScreen Pointer to a GUIScreen interface that will be used by this LANGamesGUI's GUIControlManager. Ownership is NOT transferred!
		/// @param guiInput Pointer to a GUIInput interface that will be used by this LANGamesGUI's GUIControlManager. Ownership is NOT transferred!
		LANGamesGUI(AllegroScreen* guiScreen, GUIInputWrapper* guiInput);
#pragma endregion

#pragma region Concrete Methods
		/// Handles the player interaction with the screen, and keeps up with the games on the network and the lobby.
		/// @return What came of it, see the UpdateResult enumeration.
		UpdateResult Update();

		/// Draws the screen.
		void Draw() const;
#pragma endregion

	private:
		std::unique_ptr<GUIControlManager> m_GUIControlManager; //!< The GUIControlManager which holds all the GUIControls of the LANGamesGUI.

		std::string m_ListedGames; //!< The games that the list was last filled with, for telling when it has to be filled again.

		GUICollectionBox* m_GamesBox;
		GUIListBox* m_GamesListBox;
		GUIButton* m_JoinGameButton;
		GUILabel* m_GamesStatusLabel;
		GUICollectionBox* m_LobbyBox;
		GUILabel* m_LobbySetupLabel;
		GUILabel* m_LobbyPlayersLabel;
		GUILabel* m_LobbyReadyLabel;
		GUILabel* m_LobbyStatusLabel;
		GUIButton* m_ControlsButton;
		GUIButton* m_ReadyButton;
		GUIButton* m_LeaveGameButton;
		GUIButton* m_BackToMainButton;

		/// Fills the list with the games on the network, when they have changed.
		void UpdateGamesList();

		/// Shows how the joined game is set up and who is in it.
		void UpdateLobby();

		// Disallow the use of some implicit methods.
		LANGamesGUI(const LANGamesGUI& reference) = delete;
		LANGamesGUI& operator=(const LANGamesGUI& rhs) = delete;
	};
} // namespace RTE
