#include "plugin\plugin.h"
#include "UnionAfx.h"
#include <crtversion.h>
#include <stdio.h>
#include <time.h>

namespace {
  void WriteLoadMarker() {
    char currentDirectory[MAX_PATH] = {};
    DWORD length = GetCurrentDirectoryA(
      sizeof(currentDirectory),
      currentDirectory );
    if( length == 0 || length >= sizeof(currentDirectory) )
      return;

    char path[MAX_PATH] = {};
    _snprintf_s(
      path,
      sizeof(path),
      _TRUNCATE,
      "%s\\GothicSaveSync_dllmain.log",
      currentDirectory );

    FILE* file = 0;
    fopen_s( &file, path, "at" );
    if( file ) {
      SYSTEMTIME now;
      GetLocalTime( &now );
      fprintf(
        file,
        "%02u:%02u:%02u.%03u DllMain PROCESS_ATTACH\n",
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds );
      fclose( file );
    }
    OutputDebugStringA( "GothicSaveSync: DllMain PROCESS_ATTACH\n" );
  }
}

extern "C"
int __stdcall DllMain( HMODULE hModule, DWORD fdwReason, LPVOID lpvReserved ) {
  if( fdwReason == DLL_PROCESS_ATTACH ) {
    WriteLoadMarker();
#if _DLL == 1
    UnionCore::Union.DefineCRTVersion(
      _VC_CRT_MAJOR_VERSION,
      _VC_CRT_MINOR_VERSION,
      _VC_CRT_BUILD_VERSION,
      _VC_CRT_RBUILD_VERSION );
#endif
  }
  if( fdwReason == DLL_PROCESS_DETACH ) {
  }
  return TRUE;
}