// The 36 front-end menus in Menus[36] @ 0x007b76d0.
//
// GUNLOK_MENU(Name, Id, TitleResourceId, "English title text")
//
// Title ids and text come from the satellite string DLLs; see menu_system_notes.md.
// Every index 0..35 is a real menu -- there are no gaps in the array, only menus that
// SetupMenus leaves empty for a runtime populator to fill in.

#ifndef GUNLOK_MENU
#define GUNLOK_MENU(...)
#endif

GUNLOK_MENU(Main, 0, 0x0419, "Main Menu")
GUNLOK_MENU(Options, 1, 0x041a, "Options")
GUNLOK_MENU(ScreenMode, 2, 0x041b, "Screen Mode")
GUNLOK_MENU(GfxCard, 3, 0x041c, "Graphics Card")
GUNLOK_MENU(Keyboard, 4, 0x041e, "Controls Menu")
GUNLOK_MENU(ChooseSinglePlayerLevel, 5, 0x041d, "Mission Menu")
GUNLOK_MENU(MouseControls, 6, 0x0420, "Mouse Menu")
GUNLOK_MENU(SinglePlayer, 7, 0x0421, "Single Player Menu")
GUNLOK_MENU(LoadSinglePlayerGame, 8, 0x03fc, "Load Game")
GUNLOK_MENU(NewSinglePlayerGame, 9, 0x0451, "Difficulty Menu")
GUNLOK_MENU(Multiplayer, 10, 0x0422, "Connection type")
GUNLOK_MENU(JoinGame, 11, 0x05fd, "Join game")
GUNLOK_MENU(JoinIPGame, 12, 0x044c, "IP Address")
GUNLOK_MENU(HostIPGame, 13, 0x05fc, "Create game")
GUNLOK_MENU(MultiplayerLevel, 14, 0x041d, "Mission Menu")
GUNLOK_MENU(MultiplayerOptions, 15, 0x05d7, "Multiplayer game options")
GUNLOK_MENU(MultiplayerPlayers, 16, 0x044f, "Select your Team by Clicking on Tabs:")
GUNLOK_MENU(MultiplayerGameType, 17, 0x0580, "Cooperative")
GUNLOK_MENU(LoadGameInGame, 18, 0x03fc, "Load Game")
GUNLOK_MENU(Preferences, 19, 0x0450, "Preferences")
GUNLOK_MENU(ConfirmScreenMode, 20, 0x0452, "Confirm Screen Mode")
GUNLOK_MENU(TrainingLevel, 21, 0x045f, "Training Area Menu")
GUNLOK_MENU(Video, 22, 0x0469, "Video Menu")
GUNLOK_MENU(Controls, 23, 0x041e, "Controls Menu")
GUNLOK_MENU(GraphicDetails, 24, 0x046a, "Graphic Detail Menu")
GUNLOK_MENU(Audio, 25, 0x046b, "Audio Menu")
GUNLOK_MENU(GamePreferences, 26, 0x03f0, "Game Preferences")
GUNLOK_MENU(IPGame, 27, 0x057a, "Play and Host")
GUNLOK_MENU(Camera, 28, 0x041e, "Controls Menu")
GUNLOK_MENU(Mines, 29, 0x041e, "Controls Menu")
GUNLOK_MENU(Character, 30, 0x041e, "Controls Menu")
GUNLOK_MENU(Gameplay, 31, 0x041e, "Controls Menu")
GUNLOK_MENU(ActivePause, 32, 0x041e, "Controls Menu")
GUNLOK_MENU(Recon, 33, 0x041e, "Controls Menu")
GUNLOK_MENU(Formations, 34, 0x041e, "Controls Menu")
GUNLOK_MENU(Music, 35, 0x041e, "Controls Menu")

#undef GUNLOK_MENU
