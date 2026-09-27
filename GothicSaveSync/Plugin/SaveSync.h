#ifndef __SAVE_SYNC_H__
#define __SAVE_SYNC_H__

namespace SaveSync {
  void OnSaveEnd( int slotID );
  void OnLoadBegin();
  bool InstallPendingPackage( const char* slotName, int slotID );
  const char* GetLatestPackagePath();
  const char* GetLatestSaveID();
  bool RestoreBackup();
  bool HasBackup();
}

#endif