#ifndef __LAN_SYNC_UI_H__
#define __LAN_SYNC_UI_H__

// In-game overlay for the LAN synchronisation feature.
// Renders a two-column view (local / remote saves) using the
// engine's zCView system and handles cursor-based input.

namespace LanSyncUi {
  void Open();
  void Close();
  void Poll();    // call every frame from Game_Loop / Game_MenuLoop
  bool IsActive();
}

#endif
