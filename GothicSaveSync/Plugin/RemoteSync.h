#ifndef __REMOTE_SYNC_H__
#define __REMOTE_SYNC_H__

#include "SaveSync.h"

namespace RemoteSync {
  void Start();
  void UploadSave( const char* saveID, const char* packagePath );
  void PollUi();
  void OpenDeveloperPanel();
  void DownloadLatest();
  void UploadLatestSave();

  // --- Manual management helpers ---
  // Upload a specific local slot to the server (packs + uploads in background).
  void UploadSlot( int slotID );

  // Download a specific remote save by its save_id (downloads + installs in
  // background).  Sets NeedsSavegameRefresh when done.
  void DownloadSlotBySaveID( const char* saveID );

  // Fetch /saves/slots JSON from the server and populate the remote slot
  // list.  Returns the number of remote entries read.
  struct RemoteSlotInfo {
    char saveID[128];
    char dateStr[32];
    __int64 uploadedAtEpoch;
    int size;
  };

  int  FetchRemoteSlotList( RemoteSlotInfo* slots, int maxSlots );
  bool IsServerConfigured();
  const char* GetServerUrl();

  // Restore backup for a specific slot by ID.
  bool RestoreBackupForSlot( int slotID );
}

#endif
