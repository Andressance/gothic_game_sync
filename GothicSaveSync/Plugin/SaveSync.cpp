#include "SaveSync.h"
#include "plugin.h"
#include "UnionAfx.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {
  const char PackageMagic[] = "GSSPKG1";
  const unsigned int PackageVersion = 1;
  const unsigned int PackageBufferSize = 64 * 1024;

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

  bool IsSafeRelativePath( const char* path ) {
    if( path[0] == '\\' || path[0] == '/' || strstr( path, ":" ) != 0 )
      return false;

    const char* segment = path;
    while( segment[0] != 0 ) {
      const char* separator = strpbrk( segment, "\\/" );
      size_t length = separator == 0 ? strlen( segment ) : (size_t)(separator - segment);
      if( length == 2 && segment[0] == '.' && segment[1] == '.' )
        return false;
      if( separator == 0 )
        break;
      segment = separator + 1;
    }
    return path[0] != 0;
  }

  bool EnsureParentDirectories( const char* path ) {
    char parent[MAX_PATH];
    strcpy_s( parent, path );
    char* separator = strrchr( parent, '\\' );
    if( separator == 0 )
      return true;

    *separator = 0;
    if( parent[0] == 0 )
      return true;
    if( EnsureDirectory( parent ) )
      return true;
    return EnsureParentDirectories( parent ) && EnsureDirectory( parent );
  }

  bool WritePackageFile( FILE* package, const char* source, const char* relativePath,
    unsigned int& fileCount ) {
    char sourcePath[MAX_PATH];
    _snprintf_s( sourcePath, sizeof(sourcePath), _TRUNCATE, "%s\\%s", source, relativePath );
    FILE* input = 0;
    fopen_s( &input, sourcePath, "rb" );
    if( !input )
      return false;

    if( fseek( input, 0, SEEK_END ) != 0 ) {
      fclose( input );
      return false;
    }
    long fileSize = ftell( input );
    if( fileSize < 0 || fseek( input, 0, SEEK_SET ) != 0 ) {
      fclose( input );
      return false;
    }

    unsigned int pathLength = (unsigned int)strlen( relativePath );
    unsigned __int64 size = (unsigned __int64)fileSize;
    if( fwrite( &pathLength, sizeof(pathLength), 1, package ) != 1 ||
        fwrite( &size, sizeof(size), 1, package ) != 1 ||
        fwrite( relativePath, 1, pathLength, package ) != pathLength ) {
      fclose( input );
      return false;
    }

    char buffer[PackageBufferSize];
    unsigned __int64 remaining = size;
    while( remaining > 0 ) {
      unsigned int requested = remaining > PackageBufferSize ? PackageBufferSize : (unsigned int)remaining;
      size_t read = fread( buffer, 1, requested, input );
      if( read != requested || fwrite( buffer, 1, read, package ) != read ) {
        fclose( input );
        return false;
      }
      remaining -= read;
    }

    fclose( input );
    ++fileCount;
    return true;
  }

  bool WritePackageDirectory( FILE* package, const char* source, const char* relativePath,
    unsigned int& fileCount ) {
    char directory[MAX_PATH];
    if( relativePath[0] == 0 )
      strcpy_s( directory, source );
    else
      _snprintf_s( directory, sizeof(directory), _TRUNCATE, "%s\\%s", source, relativePath );

    char pattern[MAX_PATH];
    _snprintf_s( pattern, sizeof(pattern), _TRUNCATE, "%s\\*", directory );
    WIN32_FIND_DATAA data;
    HANDLE search = FindFirstFileA( pattern, &data );
    if( search == INVALID_HANDLE_VALUE )
      return false;

    bool result = true;
    do {
      if( strcmp( data.cFileName, "." ) == 0 || strcmp( data.cFileName, ".." ) == 0 )
        continue;

      char childPath[MAX_PATH];
      if( relativePath[0] == 0 )
        strcpy_s( childPath, data.cFileName );
      else
        _snprintf_s( childPath, sizeof(childPath), _TRUNCATE, "%s\\%s", relativePath, data.cFileName );

      if( IsDirectory( data ) )
        result = WritePackageDirectory( package, source, childPath, fileCount ) && result;
      else if( IsSafeRelativePath( childPath ) )
        result = WritePackageFile( package, source, childPath, fileCount ) && result;
      else
        result = false;
    } while( FindNextFileA( search, &data ) != 0 );

    FindClose( search );
    return result;
  }

  bool PackDirectory( const char* source, const char* packagePath, int slotID ) {
    FILE* package = 0;
    fopen_s( &package, packagePath, "wb" );
    if( !package )
      return false;

    unsigned int fileCount = 0;
    unsigned int hash = UnionCore::Union.GetGothicHash();
    unsigned int reserved = 0;
    if( fwrite( PackageMagic, 1, sizeof(PackageMagic), package ) != sizeof(PackageMagic) ||
        fwrite( &PackageVersion, sizeof(PackageVersion), 1, package ) != 1 ||
        fwrite( &slotID, sizeof(slotID), 1, package ) != 1 ||
        fwrite( &hash, sizeof(hash), 1, package ) != 1 ||
        fwrite( &fileCount, sizeof(fileCount), 1, package ) != 1 ||
        fwrite( &reserved, sizeof(reserved), 1, package ) != 1 ) {
      fclose( package );
      return false;
    }

    bool result = WritePackageDirectory( package, source, "", fileCount );
    if( result && fseek( package, sizeof(PackageMagic) + sizeof(PackageVersion) +
      sizeof(slotID) + sizeof(hash), SEEK_SET ) == 0 )
      result = fwrite( &fileCount, sizeof(fileCount), 1, package ) == 1;

    fclose( package );
    return result;
  }

  bool ReadPackage( const char* packagePath, const char* target, int slotID ) {
    FILE* package = 0;
    fopen_s( &package, packagePath, "rb" );
    if( !package )
      return false;

    char magic[sizeof(PackageMagic)];
    unsigned int version = 0;
    int packageSlot = -1;
    unsigned int hash = 0;
    unsigned int fileCount = 0;
    unsigned int reserved = 0;
    bool validHeader = fread( magic, 1, sizeof(magic), package ) == sizeof(magic) &&
      fread( &version, sizeof(version), 1, package ) == 1 &&
      fread( &packageSlot, sizeof(packageSlot), 1, package ) == 1 &&
      fread( &hash, sizeof(hash), 1, package ) == 1 &&
      fread( &fileCount, sizeof(fileCount), 1, package ) == 1 &&
      fread( &reserved, sizeof(reserved), 1, package ) == 1;
    if( !validHeader || memcmp( magic, PackageMagic, sizeof(PackageMagic) ) != 0 ||
        version != PackageVersion || packageSlot != slotID ||
        hash != UnionCore::Union.GetGothicHash() ) {
      fclose( package );
      return false;
    }

    if( !EnsureDirectory( target ) ) {
      fclose( package );
      return false;
    }

    bool result = true;
    char buffer[PackageBufferSize];
    for( unsigned int index = 0; index < fileCount && result; ++index ) {
      unsigned int pathLength = 0;
      unsigned __int64 size = 0;
      if( fread( &pathLength, sizeof(pathLength), 1, package ) != 1 || pathLength == 0 || pathLength >= MAX_PATH ||
          fread( &size, sizeof(size), 1, package ) != 1 ) {
        result = false;
        break;
      }

      char relativePath[MAX_PATH];
      if( fread( relativePath, 1, pathLength, package ) != pathLength ) {
        result = false;
        break;
      }
      relativePath[pathLength] = 0;
      if( !IsSafeRelativePath( relativePath ) ) {
        result = false;
        break;
      }

      char targetPath[MAX_PATH];
      _snprintf_s( targetPath, sizeof(targetPath), _TRUNCATE, "%s\\%s", target, relativePath );
      if( !EnsureParentDirectories( targetPath ) ) {
        result = false;
        break;
      }

      FILE* output = 0;
      fopen_s( &output, targetPath, "wb" );
      if( !output ) {
        result = false;
        break;
      }

      unsigned __int64 remaining = size;
      while( remaining > 0 && result ) {
        unsigned int requested = remaining > PackageBufferSize ? PackageBufferSize : (unsigned int)remaining;
        size_t read = fread( buffer, 1, requested, package );
        if( read != requested || fwrite( buffer, 1, read, output ) != read )
          result = false;
        remaining -= read;
      }
      fclose( output );
    }

    fclose( package );
    return result;
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
    char stagingDirectory[MAX_PATH];
    char stagingPackage[MAX_PATH];
    const char* slotName = UnionCore::TSaveLoadGameInfo::GetSaveSlotName( slotID ).ToChar();
    _snprintf_s( source, sizeof(source), _TRUNCATE, "%s\\%s", saveDirectory,
      slotName );
    _snprintf_s( syncDirectory, sizeof(syncDirectory), _TRUNCATE, "%s\\save-sync", saveDirectory );
    _snprintf_s( root, sizeof(root), _TRUNCATE, "%s\\pending", syncDirectory );
    _snprintf_s( target, sizeof(target), _TRUNCATE, "%s\\%s.gss", root, slotName );
    _snprintf_s( stagingDirectory, sizeof(stagingDirectory), _TRUNCATE, "%s.tmpdir", target );
    _snprintf_s( stagingPackage, sizeof(stagingPackage), _TRUNCATE, "%s.tmp", target );

    DWORD sourceAttributes = GetFileAttributesA( source );
    if( sourceAttributes == INVALID_FILE_ATTRIBUTES ||
        (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 )
      return;

    if( !EnsureDirectory( syncDirectory ) || !EnsureDirectory( root ) )
      return;

    RemoveDirectoryTree( stagingDirectory );
    DeleteFileA( stagingPackage );
    if( !CopyDirectory( source, stagingDirectory ) ) {
      RemoveDirectoryTree( stagingDirectory );
      return;
    }

    WriteManifest( stagingDirectory, slotID );
    if( !PackDirectory( stagingDirectory, stagingPackage, slotID ) ) {
      RemoveDirectoryTree( stagingDirectory );
      DeleteFileA( stagingPackage );
      return;
    }

    RemoveDirectoryTree( stagingDirectory );
    DeleteFileA( target );
    MoveFileA( stagingPackage, target );
  }

  void OnLoadBegin() {
    int slotID = UnionCore::SaveLoadGameInfo.slotID;
    if( slotID < 0 )
      return;

    const char* saveDirectory = zoptions->GetDirString( DIR_SAVEGAMES ).ToChar();
    char source[MAX_PATH];
    char root[MAX_PATH];
    char pending[MAX_PATH];
    char staging[MAX_PATH];
    char backupRoot[MAX_PATH];
    char backup[MAX_PATH];
    const char* slotName = UnionCore::TSaveLoadGameInfo::GetSaveSlotName( slotID ).ToChar();
    _snprintf_s( source, sizeof(source), _TRUNCATE, "%s\\%s", saveDirectory, slotName );
    _snprintf_s( root, sizeof(root), _TRUNCATE, "%s\\save-sync", saveDirectory );
    _snprintf_s( pending, sizeof(pending), _TRUNCATE, "%s\\pending\\%s.gss", root, slotName );
    _snprintf_s( staging, sizeof(staging), _TRUNCATE, "%s\\pending\\%s.unpack", root, slotName );
    _snprintf_s( backupRoot, sizeof(backupRoot), _TRUNCATE, "%s\\backup", root );
    _snprintf_s( backup, sizeof(backup), _TRUNCATE, "%s\\%s", backupRoot, slotName );

    DWORD pendingAttributes = GetFileAttributesA( pending );
    if( pendingAttributes == INVALID_FILE_ATTRIBUTES ||
        (pendingAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 )
      return;

    if( !EnsureDirectory( root ) || !EnsureDirectory( backupRoot ) )
      return;

    RemoveDirectoryTree( staging );
    if( !ReadPackage( pending, staging, slotID ) || !IsManifestValid( staging, slotID ) ) {
      RemoveDirectoryTree( staging );
      return;
    }

    if( GetFileAttributesA( source ) != INVALID_FILE_ATTRIBUTES ) {
      if( !ReplaceDirectory( source, backup ) )
      {
        RemoveDirectoryTree( staging );
        return;
      }
    }

    if( !ReplaceDirectory( staging, source ) && GetFileAttributesA( backup ) != INVALID_FILE_ATTRIBUTES )
      ReplaceDirectory( backup, source );
    RemoveDirectoryTree( staging );
  }
}