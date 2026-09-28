// -----------------------------------------------------------------------
// SyncHistory.cpp — Versioned history of save-slot states.
// -----------------------------------------------------------------------

#include "SyncHistory.h"
#include "SaveSync.h"
#include "SyncLog.h"
#include "plugin.h"
#include "UnionAfx.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {
  void GetHistoryRoot( char* pathOut, size_t maxLen ) {
    Common::string gameDir = UnionCore::Union.GetGameDirectory();
    const char* saveDir = zoptions->GetDirString( DIR_SAVEGAMES ).ToChar();
    if( saveDir[0] == '\\' || saveDir[0] == '/' )
      _snprintf_s( pathOut, maxLen, _TRUNCATE, "%s%ssave-sync\\history",
        gameDir.ToChar(), saveDir );
    else
      _snprintf_s( pathOut, maxLen, _TRUNCATE, "%s\\%s\\save-sync\\history",
        gameDir.ToChar(), saveDir );
    SaveSync::EnsureParentDirectories( pathOut );
    CreateDirectoryA( pathOut, 0 );
  }

  void GetSlotPath( int slotID, char* pathOut, size_t maxLen ) {
    const char* slotName = UnionCore::TSaveLoadGameInfo::GetSaveSlotName( slotID ).ToChar();
    Common::string gameDir = UnionCore::Union.GetGameDirectory();
    const char* saveDir = zoptions->GetDirString( DIR_SAVEGAMES ).ToChar();
    if( saveDir[0] == '\\' || saveDir[0] == '/' )
      _snprintf_s( pathOut, maxLen, _TRUNCATE, "%s%s%s",
        gameDir.ToChar(), saveDir, slotName );
    else
      _snprintf_s( pathOut, maxLen, _TRUNCATE, "%s\\%s\\%s",
        gameDir.ToChar(), saveDir, slotName );
  }

  // Very basic string replace for escaping values (commas)
  void SanitizeCSV( char* str ) {
    for( char* p = str; *p; ++p ) {
      if( *p == ',' || *p == '\n' || *p == '\r' )
        *p = ' ';
    }
  }

  int SortHistoryDesc( const void* a, const void* b ) {
    const SyncHistory::HistoryEntry* ea = (const SyncHistory::HistoryEntry*)a;
    const SyncHistory::HistoryEntry* eb = (const SyncHistory::HistoryEntry*)b;
    if( ea->timestamp > eb->timestamp ) return -1;
    if( ea->timestamp < eb->timestamp ) return 1;
    return 0;
  }
}

namespace SyncHistory {

  bool RecordEntry( const char* slotName, int slotID,
    const char* source, const char* peerName ) {

    char sourcePath[MAX_PATH];
    GetSlotPath( slotID, sourcePath, sizeof(sourcePath) );

    DWORD attr = GetFileAttributesA( sourcePath );
    if( attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0 )
      return false; // Nothing to record

    char historyRoot[MAX_PATH];
    GetHistoryRoot( historyRoot, sizeof(historyRoot) );

    SYSTEMTIME st;
    GetLocalTime( &st );

    // Generate unique backup directory name: history/slot_timestamp
    char backupDir[MAX_PATH];
    _snprintf_s( backupDir, sizeof(backupDir), _TRUNCATE, "%s\\%s_%04u%02u%02u_%02u%02u%02u",
      historyRoot, slotName, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond );

    if( !SaveSync::CopyDirectory( sourcePath, backupDir ) ) {
      SyncLog::Write( "SyncHistory: Failed to copy %s to %s", sourcePath, backupDir );
      return false;
    }

    // Write metadata file (info.csv) inside the backup dir
    char metaPath[MAX_PATH];
    _snprintf_s( metaPath, sizeof(metaPath), _TRUNCATE, "%s\\info.csv", backupDir );

    FILE* f = 0;
    fopen_s( &f, metaPath, "wt" );
    if( f ) {
      // Get UNIX timestamp
      FILETIME ft;
      SystemTimeToFileTime( &st, &ft );
      ULARGE_INTEGER val;
      val.LowPart = ft.dwLowDateTime;
      val.HighPart = ft.dwHighDateTime;
      __int64 ts = (__int64)(val.QuadPart / 10000000ULL - 11644473600ULL);

      char src[32]; strcpy_s( src, source ); SanitizeCSV( src );
      char peer[64]; strcpy_s( peer, peerName ? peerName : "" ); SanitizeCSV( peer );

      // slotID,source,peerName,timestamp
      fprintf( f, "%d,%s,%s,%lld\n", slotID, src, peer, ts );
      fclose( f );
    }

    SyncLog::Write( "SyncHistory: Recorded state of %s to %s (src: %s)", slotName, backupDir, source );
    return true;
  }

  int GetEntries( HistoryEntry* entries, int maxEntries ) {
    char historyRoot[MAX_PATH];
    GetHistoryRoot( historyRoot, sizeof(historyRoot) );

    char searchPath[MAX_PATH];
    _snprintf_s( searchPath, sizeof(searchPath), _TRUNCATE, "%s\\saveold*", historyRoot );

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA( searchPath, &fd );
    if( hFind == INVALID_HANDLE_VALUE )
      return 0;

    int count = 0;
    do {
      if( !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ) continue;
      if( fd.cFileName[0] == '.' ) continue;

      char dirPath[MAX_PATH];
      _snprintf_s( dirPath, sizeof(dirPath), _TRUNCATE, "%s\\%s", historyRoot, fd.cFileName );

      char metaPath[MAX_PATH];
      _snprintf_s( metaPath, sizeof(metaPath), _TRUNCATE, "%s\\info.csv", dirPath );

      FILE* f = 0;
      fopen_s( &f, metaPath, "rt" );
      if( f ) {
        char line[256];
        if( fgets( line, sizeof(line), f ) ) {
          HistoryEntry& e = entries[count];
          memset( &e, 0, sizeof(HistoryEntry) );
          strcpy_s( e.dirPath, dirPath );

          // Extract slot name from dir (e.g. saveold10_2026...)
          const char* underscore = strchr( fd.cFileName, '_' );
          if( underscore ) {
            size_t len = underscore - fd.cFileName;
            if( len > 63 ) len = 63;
            strncpy_s( e.slotName, fd.cFileName, len );
            e.slotName[len] = 0;
          }

          // Parse CSV
          char* ctx = 0;
          char* pSlotID = strtok_s( line, ",", &ctx );
          char* pSource = strtok_s( 0, ",", &ctx );
          char* pPeer   = strtok_s( 0, ",", &ctx );
          char* pTs     = strtok_s( 0, "\n", &ctx );

          if( pSlotID ) e.slotID = atoi( pSlotID );
          if( pSource ) strcpy_s( e.source, pSource );
          if( pPeer )   strcpy_s( e.peerName, pPeer );
          if( pTs )     e.timestamp = _atoi64( pTs );

          // Format date
          if( e.timestamp > 0 ) {
            ULARGE_INTEGER val;
            val.QuadPart = (e.timestamp + 11644473600ULL) * 10000000ULL;
            FILETIME ft;
            ft.dwLowDateTime = val.LowPart;
            ft.dwHighDateTime = val.HighPart;
            SYSTEMTIME st;
            FileTimeToSystemTime( &ft, &st );
            _snprintf_s( e.dateStr, sizeof(e.dateStr), _TRUNCATE,
              "%02u/%02u/%04u %02u:%02u", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute );
          }

          count++;
        }
        fclose( f );
      }

    } while( count < maxEntries && FindNextFileA( hFind, &fd ) );
    FindClose( hFind );

    // Sort newest first
    qsort( entries, count, sizeof(HistoryEntry), SortHistoryDesc );
    return count;
  }

  bool RestoreEntry( const HistoryEntry& entry ) {
    char targetPath[MAX_PATH];
    GetSlotPath( entry.slotID, targetPath, sizeof(targetPath) );

    // Auto-record current state before restoring
    RecordEntry( entry.slotName, entry.slotID, "restaurar", "" );

    char tempPath[MAX_PATH];
    _snprintf_s( tempPath, sizeof(tempPath), _TRUNCATE, "%s.restoretmp", targetPath );

    SaveSync::RemoveDirectoryTree( tempPath );
    if( !SaveSync::CopyDirectory( entry.dirPath, tempPath ) ) {
      SyncLog::Write( "SyncHistory: Failed to copy %s to %s", entry.dirPath, tempPath );
      SaveSync::RemoveDirectoryTree( tempPath );
      return false;
    }

    // Drop info.csv from the temp copy since we don't want it in the actual slot
    char metaFile[MAX_PATH];
    _snprintf_s( metaFile, sizeof(metaFile), _TRUNCATE, "%s\\info.csv", tempPath );
    DeleteFileA( metaFile );

    SaveSync::RemoveDirectoryTree( targetPath );
    MoveFileExA( tempPath, targetPath, MOVEFILE_REPLACE_EXISTING );

    SyncLog::Write( "SyncHistory: Restored %s from %s", entry.slotName, entry.dirPath );
    return true;
  }
}
