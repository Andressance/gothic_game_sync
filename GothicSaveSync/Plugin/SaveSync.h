#ifndef __SAVE_SYNC_H__
#define __SAVE_SYNC_H__

namespace SaveSync {
  struct SaveSlotInfo {
    int slotID;
    char name[64];
    char dateStr[32];
    __int64 timestamp;
    bool exists;
  };

  void OnSaveEnd( int slotID );
  void OnLoadBegin();
  bool InstallPendingPackage( const char* slotName, int slotID, const char* source );
  const char* GetLatestPackagePath();
  const char* GetLatestSaveID();
  bool RestoreBackup();
  bool HasBackup();
  int GetSaveSlotList( SaveSlotInfo* slots, int maxSlots );
  bool PackSlotToFile( int slotID, const char* outputPath );
  bool EnsureDirectory( const char* path );
  bool EnsureParentDirectories( const char* path );
  bool CopyDirectory( const char* source, const char* target );
  bool RemoveDirectoryTree( const char* path );
}

#endif