#ifndef __GAME_UI_H__
#define __GAME_UI_H__

// In-game overlay UI for GothicSaveSync.
// Renders messages and prompts directly on the game screen using
// the engine's own zCView system, replacing Windows MessageBox calls.

namespace GameUi {
  // Possible overlay states used by the polling loop.
  enum OverlayState {
    OVERLAY_NONE,       // Nothing visible
    OVERLAY_INFO,       // Informational message (auto-dismiss or key-dismiss)
    OVERLAY_WARNING,    // Warning message (key-dismiss)
    OVERLAY_QUESTION,   // Yes/No prompt waiting for input
    OVERLAY_MENU        // Multi-option developer menu
  };

  // Result from a question overlay, consumed by the caller.
  enum QuestionResult {
    QUESTION_PENDING,
    QUESTION_YES,
    QUESTION_NO
  };

  // Menu items for the developer panel.
  enum MenuItem {
    MENUITEM_DOWNLOAD_REMOTE,
    MENUITEM_UPLOAD_SAVE,
    MENUITEM_RESTORE_BACKUP,
    MENUITEM_CLOSE,
    MENUITEM_COUNT
  };

  // Initialise / shut down the overlay system. Call from Game_Init / Game_Exit.
  void Init();
  void Shutdown();

  // Show an informational message overlay (auto-dismisses after a few seconds
  // or on any key press).
  void ShowInfo( const char* title, const char* message );

  // Show a warning overlay (stays until key press).
  void ShowWarning( const char* title, const char* message );

  // Begin a yes/no question overlay.  Returns immediately; poll
  // GetQuestionResult() each frame to get the answer.
  void ShowQuestion( const char* title, const char* message );

  // Returns QUESTION_PENDING while the overlay is open, or
  // QUESTION_YES / QUESTION_NO once the user decides.
  QuestionResult GetQuestionResult();

  // Show the developer panel menu.  Returns immediately.
  void ShowDeveloperMenu();

  // Returns -1 while the menu is open, or the chosen MenuItem index
  // once the player picks an option.
  int GetMenuResult();

  // Must be called once per frame (from Game_Loop).
  // Handles rendering, input, and auto-dismiss timers.
  void Poll();

  // Returns true when any overlay is currently visible.
  bool IsActive();
}

#endif // __GAME_UI_H__
