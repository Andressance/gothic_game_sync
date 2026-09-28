#ifndef __SYNC_HISTORY_H__
#define __SYNC_HISTORY_H__

#include <windows.h>

// Versioned history of save-slot states.
// Every time a slot is overwritten by a sync operation, the previous
// state is saved to a timestamped directory under save-sync/history/.
// The user can browse and restore any entry from the in-game UI.

namespace SyncHistory {
  static const int MaxEntriesPerSlot = 5;
  static const int MaxTotalEntries   = 100;

  struct HistoryEntry {
    char    slotName[64];
    int     slotID;
    char    dateStr[32];       // "28/09/2026 18:53"
    char    source[32];        // "lan", "remoto", "auto", "restaurar"
    char    peerName[64];      // peer name (LAN) or empty
    char    dirPath[MAX_PATH]; // full path to the backup directory
    __int64 timestamp;         // unix epoch
  };

  // Save the current state of a slot before it gets overwritten.
  // Returns false if the slot doesn't exist (nothing to record).
  bool RecordEntry( const char* slotName, int slotID,
    const char* source, const char* peerName );

  // Retrieve all history entries sorted newest-first.
  int GetEntries( HistoryEntry* entries, int maxEntries );

  // Restore a history entry back to its original slot.
  // The current slot state is automatically recorded to history
  // first, so the restore itself can be undone.
  bool RestoreEntry( const HistoryEntry& entry );
}

#endif
