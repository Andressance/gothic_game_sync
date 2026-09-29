#ifndef __MANUAL_SYNC_UI_H__
#define __MANUAL_SYNC_UI_H__

// In-game overlay for manual per-slot management of saves against the
// remote server.  Shows two columns (local / remote) and lets the user
// upload, download, or restore backups for any individual slot.

namespace ManualSyncUi {
  void Open();
  void Close();
  void Poll();     // call every frame from Game_Loop / Game_MenuLoop
  bool IsActive();
}

#endif
