#ifndef __LOCAL_SYNC_H__
#define __LOCAL_SYNC_H__

#include "SaveSync.h"

// Peer-to-peer LAN synchronisation module for GothicSaveSync.
// Uses UDP broadcast for discovery and TCP for data transfer.
// All networking runs on background threads; the main game loop
// polls state via the accessor functions below.

namespace LocalSync {
  enum State {
    STATE_IDLE,
    STATE_SCANNING,
    STATE_CONNECTED,
    STATE_TRANSFERRING,
    STATE_ERROR
  };

  // Lifecycle — call from Game_Init / Game_Exit.
  void Init();
  void Shutdown();

  // Start / stop a discovery scan.
  void StartDiscovery();
  void Disconnect();

  // Must be called once per frame to handle post-transfer actions
  // on the main thread (e.g. savegame manager reinit).
  void Poll();

  // State accessors (thread-safe reads).
  State       GetState();
  const char* GetPeerName();
  const char* GetStatusMessage();

  int                          GetLocalSlotCount();
  const SaveSync::SaveSlotInfo* GetLocalSlots();
  int                          GetRemoteSlotCount();
  const SaveSync::SaveSlotInfo* GetRemoteSlots();

  // Request a transfer (from UI).  Index is into the local/remote
  // slot arrays returned by GetLocalSlots / GetRemoteSlots.
  void RequestSendSave( int localSlotIndex );
  void RequestReceiveSave( int remoteSlotIndex );

  // Transfer result polling.
  bool IsTransferComplete();
  bool IsTransferSuccess();
  void AcknowledgeTransfer();
}

#endif
