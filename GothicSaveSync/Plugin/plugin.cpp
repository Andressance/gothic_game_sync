#include "plugin.h"
#include "UnionAfx.h"
#include "SaveSync.h"
#include "RemoteSync.h"
#include "DeveloperUi.h"
#include "GameUi.h"
#include "SyncLog.h"

#include <windows.h>
#include <stdio.h>

extern cppimport UnionCore::TSaveLoadGameInfo UnionCore::SaveLoadGameInfo;

using namespace Common;
using namespace UnionCore;
using namespace Vdfs32;

namespace {
  void DebugLog( const char* message ) {
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA( 0, modulePath, sizeof(modulePath) );
    char* separator = strrchr( modulePath, '\\' );
    if( separator )
      *(separator + 1) = 0;

    char path[MAX_PATH];
    _snprintf_s( path, sizeof(path), _TRUNCATE,
      "%sGothicSaveSync_debug.log", modulePath );
    FILE* file = 0;
    fopen_s( &file, path, "at" );
    if( file ) {
      SYSTEMTIME time;
      GetLocalTime( &time );
      fprintf( file, "%02u:%02u:%02u.%03u %s\n",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, message );
      fclose( file );
    }
    OutputDebugStringA( message );
    OutputDebugStringA( "\n" );
  }

  void DebugLogHook( const char* hookName ) {
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA( 0, modulePath, sizeof(modulePath) );
    char* separator = strrchr( modulePath, '\\' );
    if( separator )
      *(separator + 1) = 0;

    char path[MAX_PATH];
    _snprintf_s( path, sizeof(path), _TRUNCATE,
      "%sGothicSaveSync_dllmain.log", modulePath );
    FILE* file = 0;
    fopen_s( &file, path, "at" );
    if( file ) {
      SYSTEMTIME time;
      GetLocalTime( &time );
      fprintf( file, "%02u:%02u:%02u.%03u %s\n",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, hookName );
      fclose( file );
    }
  }
}

cexport void Game_Entry() {
  DebugLogHook( "Game_Entry reached" );
  DebugLog( "Game_Entry reached" );
}

cexport void Game_Init() {
  DebugLogHook( "Game_Init reached" );
  DebugLog( "Game_Init reached" );
  GameUi::Init();
  RemoteSync::Start();
}

cexport void Game_Exit() {
  GameUi::Shutdown();
}

cexport void Game_PreLoop() {
}

cexport void Game_Loop() {
  DebugLogHook( "Game_Loop reached" );
  GameUi::Poll();
  RemoteSync::PollUi();
  DeveloperUi::Poll();
}

cexport void Game_PostLoop() {
}

cexport void Game_MenuLoop() {
  GameUi::Poll();
}

cexport void Game_SaveBegin() {
}

cexport void Game_SaveEnd() {
  DebugLogHook( "Game_SaveEnd reached" );
  DebugLog( "Game_SaveEnd reached" );
}

void __fastcall Hook_SetAndWriteSavegame( oCSavegameManager* _this, void* vtable, int slotNr, oCSavegameInfo* info );
Hook Ivk_SetAndWriteSavegame = CreateHook( (void*)0x004390E0, &Hook_SetAndWriteSavegame, Hook_Detours );
void __fastcall Hook_SetAndWriteSavegame( oCSavegameManager* _this, void* vtable, int slotNr, oCSavegameInfo* info ) {
  Ivk_SetAndWriteSavegame( _this, vtable, slotNr, info );
  
  // Now the save is fully written to disk, so we can pack and upload it.
  SyncLog::Write( "Hook_SetAndWriteSavegame: finished writing slot %d", slotNr );
  SaveSync::OnSaveEnd( slotNr );
  const char* pkg = SaveSync::GetLatestPackagePath();
  const char* id  = SaveSync::GetLatestSaveID();
  SyncLog::Write( "Hook_SetAndWriteSavegame: latestID='%s' latestPath='%s'", id, pkg );
  if( pkg[0] != 0 ) {
    SyncLog::Write( "Hook_SetAndWriteSavegame: triggering UploadSave" );
    RemoteSync::UploadSave( id, pkg );
  } else {
    SyncLog::Write( "Hook_SetAndWriteSavegame: no package to upload" );
  }
}


void LoadBegin() {
}

void LoadEnd() {
}

cexport void Game_LoadBegin_NewGame() {
  LoadBegin();
}

cexport void Game_LoadEnd_NewGame() {
  LoadEnd();
}

cexport void Game_LoadBegin_SaveGame() {
  SaveSync::OnLoadBegin();
  LoadBegin();
}

cexport void Game_LoadEnd_SaveGame() {
  LoadEnd();
}

cexport void Game_LoadBegin_ChangeLevel() {
  LoadBegin();
}

cexport void Game_LoadEnd_ChangeLevel() {
  LoadEnd();
}

cexport void Game_LoadBegin_Trigger() {
}

cexport void Game_LoadEnd_Trigger() {
}

cexport void Game_Pause() {
}

cexport void Game_Unpause() {
}

cexport void Game_DefineExternals() {
}

cexport void Game_ApplyOptions() {
}