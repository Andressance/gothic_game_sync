#ifndef __REMOTE_SYNC_H__
#define __REMOTE_SYNC_H__

namespace RemoteSync {
  void Start();
  void UploadSave( const char* saveID, const char* packagePath );
  void PollUi();
  void UploadLatestSave();
}

#endif
