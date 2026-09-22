#include "SaveSync.h"
#include "plugin.h"
#include "UnionAfx.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {
  bool IsDirectory( const WIN32_FIND_DATAA& data ) {
    return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  }

  bool EnsureDirectory( const char* path ) {
    if( CreateDirectoryA( path, 0 ) != 0 )
      return true;

    return GetLastError() == ERROR_ALREADY_EXISTS;
  }

  bool RemoveDirectoryTree( const char* path ) {
    char pattern[MAX_PATH];
    _snprintf_s( pattern, sizeof(pattern), _TRUNCATE, "%s\\*", path );

    WIN32_FIND_DATAA data;
    HANDLE search = FindFirstFileA( pattern, &data );
    if( search == INVALID_HANDLE_VALUE )
      return GetLastError() == ERROR_FILE_NOT_FOUND;

    bool result = true;
    do {
      if( strcmp( data.cFileName, "." ) == 0 || strcmp( data.cFileName, ".." ) == 0 )
        continue;

      char childPath[MAX_PATH];
      _snprintf_s( childPath, sizeof(childPath), _TRUNCATE, "%s\\%s", path, data.cFileName );
      if( IsDirectory( data ) )
        result = RemoveDirectoryTree( childPath ) && result;
      else
        result = (DeleteFileA( childPath ) != 0) && result;
    } while( FindNextFileA( search, &data ) != 0 );

    FindClose( search );
    return (RemoveDirectoryA( path ) != 0) && result;
  }

  bool CopyDirectory( const char* source, const char* target ) {
    if( !EnsureDirectory( target ) )
      return false;

    char pattern[MAX_PATH];
    _snprintf_s( pattern, sizeof(pattern), _TRUNCATE, "%s\\*", source );

    WIN32_FIND_DATAA data;
    HANDLE search = FindFirstFileA( pattern, &data );
    if( search == INVALID_HANDLE_VALUE )
      return false;

    bool result = true;
    do {
      if( strcmp( data.cFileName, "." ) == 0 || strcmp( data.cFileName, ".." ) == 0 )
        continue;

      char sourcePath[MAX_PATH];
      char targetPath[MAX_PATH];
      _snprintf_s( sourcePath, sizeof(sourcePath), _TRUNCATE, "%s\\%s", source, data.cFileName );
      _snprintf_s( targetPath, sizeof(targetPath), _TRUNCATE, "%s\\%s", target, data.cFileName );

      if( IsDirectory( data ) ) {
        result = CopyDirectory( sourcePath, targetPath ) && result;
      }
      else {
        result = (CopyFileA( sourcePath, targetPath, FALSE ) != 0) && result;
      }
    } while( FindNextFileA( search, &data ) != 0 );

    FindClose( search );
    return result;
  }

  void WriteManifest( const char* target, int slotID ) {
    char path[MAX_PATH];
    _snprintf_s( path, sizeof(path), _TRUNCATE, "%s\\manifest.txt", target );

    FILE* file = 0;
    fopen_s( &file, path, "wb" );
    if( !file )
      return;

    fprintf( file, "format=1\n" );
    fprintf( file, "slot=%d\n", slotID );
    fprintf( file, "gothic_hash=%u\n", UnionCore::Union.GetGothicHash() );
    fclose( file );
  }

  bool IsManifestValid( const char* target, int slotID ) {
    char path[MAX_PATH];
    char line[128];
    unsigned int gothicHash = 0;
    int manifestSlot = -1;
    bool hasFormat = false;
    bool hasSlot = false;
    bool hasHash = false;
    _snprintf_s( path, sizeof(path), _TRUNCATE, "%s\\manifest.txt", target );

    FILE* file = 0;
    fopen_s( &file, path, "rb" );
    if( !file )
      return false;

    while( fgets( line, sizeof(line), file ) != 0 ) {
      if( strcmp( line, "format=1\n" ) == 0 )
        hasFormat = true;
      else if( sscanf_s( line, "slot=%d", &manifestSlot ) == 1 )
        hasSlot = true;
      else if( sscanf_s( line, "gothic_hash=%u", &gothicHash ) == 1 )
        hasHash = true;
    }
    fclose( file );

    return hasFormat && hasSlot && hasHash && manifestSlot == slotID &&
      gothicHash == UnionCore::Union.GetGothicHash();
  }

  bool ReplaceDirectory( const char* source, const char* target ) {
    char staging[MAX_PATH];
    _snprintf_s( staging, sizeof(staging), _TRUNCATE, "%s.sync", target );
    RemoveDirectoryTree( staging );

    if( !CopyDirectory( source, staging ) ) {
      RemoveDirectoryTree( staging );
      return false;
    }

    RemoveDirectoryTree( target );
    if( MoveFileA( staging, target ) != 0 )
      return true;

    RemoveDirectoryTree( staging );
    return false;
  }
}

namespace SaveSync {
  void OnSaveEnd() {
    int slotID = UnionCore::SaveLoadGameInfo.slotID;
    if( slotID < 0 )
      return;

    const char* saveDirectory = zoptions->GetDirString( DIR_SAVEGAMES ).ToChar();
    char source[MAX_PATH];
    char syncDirectory[MAX_PATH];
    char root[MAX_PATH];
    char target[MAX_PATH];
    _snprintf_s( source, sizeof(source), _TRUNCATE, "%s\\%s", saveDirectory,
      UnionCore::TSaveLoadGameInfo::GetSaveSlotName( slotID ).ToChar() );
    _snprintf_s( syncDirectory, sizeof(syncDirectory), _TRUNCATE, "%s\\save-sync", saveDirectory );
    _snprintf_s( root, sizeof(root), _TRUNCATE, "%s\\pending", syncDirectory );
    _snprintf_s( target, sizeof(target), _TRUNCATE, "%s\\%s", root,
      UnionCore::TSaveLoadGameInfo::GetSaveSlotName( slotID ).ToChar() );

    DWORD sourceAttributes = GetFileAttributesA( source );
    if( sourceAttributes == INVALID_FILE_ATTRIBUTES ||
        (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 )
      return;

    if( !EnsureDirectory( syncDirectory ) || !EnsureDirectory( root ) )
      return;

    char staging[MAX_PATH];
    _snprintf_s( staging, sizeof(staging), _TRUNCATE, "%s.tmp", target );
    RemoveDirectoryTree( staging );
    if( !CopyDirectory( source, staging ) ) {
      RemoveDirectoryTree( staging );
      return;
    }

    WriteManifest( staging, slotID );
    RemoveDirectoryTree( target );
    MoveFileA( staging, target );
  }

  void OnLoadBegin() {
    int slotID = UnionCore::SaveLoadGameInfo.slotID;
    if( slotID < 0 )
      return;

    const char* saveDirectory = zoptions->GetDirString( DIR_SAVEGAMES ).ToChar();
    char source[MAX_PATH];
    char root[MAX_PATH];
    char pending[MAX_PATH];
    char backupRoot[MAX_PATH];
    char backup[MAX_PATH];
    const char* slotName = UnionCore::TSaveLoadGameInfo::GetSaveSlotName( slotID ).ToChar();
    _snprintf_s( source, sizeof(source), _TRUNCATE, "%s\\%s", saveDirectory, slotName );
    _snprintf_s( root, sizeof(root), _TRUNCATE, "%s\\save-sync", saveDirectory );
    _snprintf_s( pending, sizeof(pending), _TRUNCATE, "%s\\pending\\%s", root, slotName );
    _snprintf_s( backupRoot, sizeof(backupRoot), _TRUNCATE, "%s\\backup", root );
    _snprintf_s( backup, sizeof(backup), _TRUNCATE, "%s\\%s", backupRoot, slotName );

    DWORD pendingAttributes = GetFileAttributesA( pending );
    if( pendingAttributes == INVALID_FILE_ATTRIBUTES ||
        (pendingAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        !IsManifestValid( pending, slotID ) )
      return;

    if( !EnsureDirectory( root ) || !EnsureDirectory( backupRoot ) )
      return;

    if( GetFileAttributesA( source ) != INVALID_FILE_ATTRIBUTES ) {
      if( !ReplaceDirectory( source, backup ) )
        return;
    }

    if( !ReplaceDirectory( pending, source ) && GetFileAttributesA( backup ) != INVALID_FILE_ATTRIBUTES )
      ReplaceDirectory( backup, source );
  }
}