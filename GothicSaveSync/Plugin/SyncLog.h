#ifndef __SYNC_LOG_H__
#define __SYNC_LOG_H__

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

// Centralised debug logger for GothicSaveSync.
// Writes timestamped messages to GothicSaveSync_debug.log in the game folder
// and also to OutputDebugString for capture with DebugView/VS.
namespace SyncLog {

  inline void Write( const char* format, ... ) {
    // Build message
    char message[1024];
    va_list args;
    va_start( args, format );
    _vsnprintf_s( message, sizeof(message), _TRUNCATE, format, args );
    va_end( args );

    // Resolve log path (next to the game executable)
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
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
        message );
      fclose( file );
    }
    OutputDebugStringA( "[GSS] " );
    OutputDebugStringA( message );
    OutputDebugStringA( "\n" );
  }

}

#endif
