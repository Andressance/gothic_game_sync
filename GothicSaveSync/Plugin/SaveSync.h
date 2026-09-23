#ifndef __SAVE_SYNC_H__
#define __SAVE_SYNC_H__

namespace SaveSync {
  void OnSaveEnd();
  void OnLoadBegin();
  const char* GetLatestPackagePath();
  const char* GetLatestSaveID();
}

#endif