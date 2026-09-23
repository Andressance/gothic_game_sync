#include "DeveloperUi.h"
#include "RemoteSync.h"

namespace DeveloperUi {
  void Poll() {
    RemoteSync::OpenDeveloperPanel();
  }
}
