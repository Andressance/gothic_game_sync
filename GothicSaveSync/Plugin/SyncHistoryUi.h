#ifndef __SYNC_HISTORY_UI_H__
#define __SYNC_HISTORY_UI_H__

// Scrollable in-game overlay that lists all history entries and
// lets the user restore a previous save-slot version.

namespace SyncHistoryUi {
  void Open();
  void Close();
  void Poll();     // call every frame from Game_Loop / Game_MenuLoop
  bool IsActive();
}

#endif
