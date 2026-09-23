#include "RemoteSync.h"
#include "plugin.h"
#include "UnionAfx.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

namespace {
  const unsigned int MaxResponseSize = 1024 * 1024;

  struct Url {
    wchar_t host[256];
    wchar_t path[1024];
    INTERNET_PORT port;
    bool secure;
  };

  struct UploadJob {
    char saveID[MAX_PATH];
    char packagePath[MAX_PATH];
    char serverUrl[1024];
  };

  struct RemoteSave {
    char saveID[MAX_PATH];
    __int64 uploadedAt;
  };

  RemoteSave RemoteSaves[32] = {};
  unsigned int RemoteSaveCount = 0;
  bool RemotePromptShown = false;
  bool RemoteDownloadStarted = false;
  LONG ServerStatus = 0;
  bool ServerStatusShown = false;
  char ConfiguredServerUrl[1024] = {};

  bool ReadServerUrl( char* result, size_t resultSize ) {
    Common::string gameDirectory = UnionCore::Union.GetGameDirectory();
    char path[MAX_PATH];
    _snprintf_s( path, sizeof(path), _TRUNCATE, "%s\\.env",
      gameDirectory.ToChar() );

    FILE* file = 0;
    fopen_s( &file, path, "rt" );
    if( !file )
      return false;

    char line[1200];
    bool found = false;
    while( fgets( line, sizeof(line), file ) != 0 ) {
      char value[1024];
      if( sscanf_s( line, "GOTHICSAVE_SERVER_URL=%1023[^\r\n]", value,
        (unsigned)_countof(value) ) == 1 ) {
        strcpy_s( result, resultSize, value );
        found = true;
        break;
      }
    }
    fclose( file );
    return found && result[0] != 0;
  }

  bool ParseUrl( const char* value, Url& result ) {
    wchar_t wide[1024];
    MultiByteToWideChar( CP_UTF8, 0, value, -1, wide, _countof(wide) );
    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = result.host;
    components.dwHostNameLength = _countof(result.host);
    components.lpszUrlPath = result.path;
    components.dwUrlPathLength = _countof(result.path);
    if( !WinHttpCrackUrl( wide, 0, 0, &components ) )
      return false;
    result.port = components.nPort;
    result.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
  }

  HINTERNET OpenRequest( const Url& url, const wchar_t* method,
    HINTERNET& session ) {
    session = WinHttpOpen( L"GothicSaveSync/0.1",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0 );
    if( !session )
      return 0;
    HINTERNET connection = WinHttpConnect( session, url.host, url.port, 0 );
    if( !connection ) {
      WinHttpCloseHandle( session );
      session = 0;
      return 0;
    }
    DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest( connection, method, url.path,
      0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags );
    WinHttpCloseHandle( connection );
    if( !request ) {
      WinHttpCloseHandle( session );
      session = 0;
    }
    return request;
  }

  bool ReadResponse( HINTERNET request, const char* path ) {
    FILE* output = 0;
    fopen_s( &output, path, "wb" );
    if( !output )
      return false;

    char buffer[8192];
    DWORD read = 0;
    unsigned int total = 0;
    bool result = true;
    while( WinHttpReadData( request, buffer, sizeof(buffer), &read ) && read > 0 ) {
      if( total + read > MaxResponseSize ||
          fwrite( buffer, 1, read, output ) != read ) {
        result = false;
        break;
      }

      __int64 LocalPackageTime( const char* saveID ) {
        Common::string saveDirectory = zoptions->GetDirString( DIR_SAVEGAMES );
        char path[MAX_PATH];
        _snprintf_s( path, sizeof(path), _TRUNCATE, "%s\\save-sync\\pending\\%s.gss",
          saveDirectory.ToChar(), saveID );
        WIN32_FILE_ATTRIBUTE_DATA data;
        if( !GetFileAttributesExA( path, GetFileExInfoStandard, &data ) )
          return 0;
        ULARGE_INTEGER value;
        value.LowPart = data.ftLastWriteTime.dwLowDateTime;
        value.HighPart = data.ftLastWriteTime.dwHighDateTime;
        return (__int64)(value.QuadPart / 10000000ULL - 11644473600ULL);
      }

      unsigned int ReadRemoteSaves() {
        Common::string gameDirectory = UnionCore::Union.GetGameDirectory();
        char path[MAX_PATH];
        _snprintf_s( path, sizeof(path), _TRUNCATE, "%s\\save-sync\\remote.json",
          gameDirectory.ToChar() );
        FILE* file = 0;
        fopen_s( &file, path, "rt" );
        if( !file )
          return 0;
        fseek( file, 0, SEEK_END );
        long length = ftell( file );
        fseek( file, 0, SEEK_SET );
        if( length <= 0 || length > (long)MaxResponseSize ) {
          fclose( file );
          return 0;
        }
        char* json = new char[(size_t)length + 1];
        fread( json, 1, (size_t)length, file );
        json[length] = 0;
        fclose( file );

        RemoteSaveCount = 0;
        const char* cursor = json;
        while( RemoteSaveCount < _countof(RemoteSaves) ) {
          const char* id = strstr( cursor, "\"save_id\":\"" );
          if( !id )
            break;
          id += 11;
          const char* end = strchr( id, '"' );
          const char* timestamp = strstr( end ? end : id, "\"uploaded_at_epoch\":" );
          if( !end || !timestamp )
            break;
          size_t idLength = (size_t)(end - id);
          if( idLength == 0 || idLength >= sizeof(RemoteSaves[0].saveID) )
            break;
          memcpy( RemoteSaves[RemoteSaveCount].saveID, id, idLength );
          RemoteSaves[RemoteSaveCount].saveID[idLength] = 0;
          RemoteSaves[RemoteSaveCount].uploadedAt = _atoi64( timestamp + 20 );
          ++RemoteSaveCount;
          cursor = end + 1;
        }
        delete[] json;
        return RemoteSaveCount;
      }

      bool HasNewerRemoteSave() {
        for( unsigned int index = 0; index < RemoteSaveCount; ++index )
          if( RemoteSaves[index].uploadedAt > LocalPackageTime( RemoteSaves[index].saveID ) )
            return true;
        return false;
      }

      unsigned int CountNewerRemoteSaves() {
        unsigned int count = 0;
        for( unsigned int index = 0; index < RemoteSaveCount; ++index )
          if( RemoteSaves[index].uploadedAt > LocalPackageTime( RemoteSaves[index].saveID ) )
            ++count;
        return count;
      }

      void DownloadSave( const char* serverUrl, const RemoteSave& save ) {
        Url url;
        if( !ParseUrl( serverUrl, url ) )
          return;
        wchar_t id[128];
        MultiByteToWideChar( CP_UTF8, 0, save.saveID, -1, id, _countof(id) );
        wchar_t path[1024];
        _snwprintf_s( path, _countof(path), _TRUNCATE, L"%s/saves/%s/download",
          url.path, id );
        wcscpy_s( url.path, path );
        HINTERNET session = 0;
        HINTERNET request = OpenRequest( url, L"GET", session );
        if( !request )
          return;
        bool sent = WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
          WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) &&
          WinHttpReceiveResponse( request, 0 );
        if( sent ) {
          Common::string saveDirectory = zoptions->GetDirString( DIR_SAVEGAMES );
          char directory[MAX_PATH];
          char target[MAX_PATH];
          _snprintf_s( directory, sizeof(directory), _TRUNCATE, "%s\\save-sync\\pending",
            saveDirectory.ToChar() );
          CreateDirectoryA( directory, 0 );
          _snprintf_s( target, sizeof(target), _TRUNCATE, "%s\\%s.gss",
            directory, save.saveID );
          char temporary[MAX_PATH];
          _snprintf_s( temporary, sizeof(temporary), _TRUNCATE, "%s.download",
            target );
          if( ReadResponse( request, temporary ) )
            MoveFileExA( temporary, target, MOVEFILE_REPLACE_EXISTING );
          else
            DeleteFileA( temporary );
        }
        WinHttpCloseHandle( request );
        WinHttpCloseHandle( session );
      }

      DWORD WINAPI DownloadThread( void* argument ) {
        char* serverUrl = (char*)argument;
        for( unsigned int index = 0; index < RemoteSaveCount; ++index )
          if( RemoteSaves[index].uploadedAt > LocalPackageTime( RemoteSaves[index].saveID ) )
            DownloadSave( serverUrl, RemoteSaves[index] );
        delete[] serverUrl;
        return 0;
      }
      total += read;
    }
    fclose( output );
    return result;
  }

  void FetchRemoteSaves( const char* serverUrl ) {
    Url url;
    if( !ParseUrl( serverUrl, url ) )
      return;

    wchar_t path[1024];
    _snwprintf_s( path, _countof(path), _TRUNCATE, L"%s/saves", url.path );
    wcscpy_s( url.path, path );
    HINTERNET session = 0;
    HINTERNET request = OpenRequest( url, L"GET", session );
    if( !request )
      return;

    const wchar_t* headers = L"Accept: application/json\r\n";
    bool sent = WinHttpSendRequest( request, headers, (DWORD)-1L,
      WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) &&
      WinHttpReceiveResponse( request, 0 );
    if( sent ) {
      Common::string gameDirectory = UnionCore::Union.GetGameDirectory();
      char directory[MAX_PATH];
      _snprintf_s( directory, sizeof(directory), _TRUNCATE, "%s\\save-sync",
        gameDirectory.ToChar() );
      CreateDirectoryA( directory, 0 );
      char output[MAX_PATH];
      _snprintf_s( output, sizeof(output), _TRUNCATE, "%s\\remote.json", directory );
      ReadResponse( request, output );
    }

    bool CheckServerStatus( const char* serverUrl ) {
      Url url;
      if( !ParseUrl( serverUrl, url ) )
        return false;

      wchar_t path[1024];
      _snwprintf_s( path, _countof(path), _TRUNCATE, L"%s/status", url.path );
      wcscpy_s( url.path, path );
      HINTERNET session = 0;
      HINTERNET request = OpenRequest( url, L"GET", session );
      if( !request )
        return false;

      bool result = WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) &&
        WinHttpReceiveResponse( request, 0 );
      if( result ) {
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        result = WinHttpQueryHeaders( request,
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
          WINHTTP_NO_HEADER_INDEX ) && statusCode == 200;
      }
      WinHttpCloseHandle( request );
      WinHttpCloseHandle( session );
      return result;
    }
    WinHttpCloseHandle( request );
    WinHttpCloseHandle( session );
  }

  DWORD WINAPI StartupThread( void* argument ) {
    char* serverUrl = (char*)argument;
    InterlockedExchange( &ServerStatus, CheckServerStatus( serverUrl ) ? 1 : -1 );
    FetchRemoteSaves( serverUrl );
    delete[] serverUrl;
    return 0;
  }

  DWORD WINAPI UploadThread( void* argument ) {
    UploadJob* job = (UploadJob*)argument;
    Url url;
    if( ParseUrl( job->serverUrl, url ) ) {
      wchar_t path[1024];
      wchar_t saveID[128];
      MultiByteToWideChar( CP_UTF8, 0, job->saveID, -1, saveID, _countof(saveID) );
      _snwprintf_s( path, _countof(path), _TRUNCATE, L"%s/saves/%s", url.path, saveID );
      wcscpy_s( url.path, path );

      FILE* package = 0;
      fopen_s( &package, job->packagePath, "rb" );
      if( package ) {
        fseek( package, 0, SEEK_END );
        long length = ftell( package );
        fseek( package, 0, SEEK_SET );
        if( length > 0 ) {
          const char* boundary = "----GothicSaveSyncBoundary";
          char prefix[512];
          _snprintf_s( prefix, sizeof(prefix), _TRUNCATE,
            "--%s\r\nContent-Disposition: form-data; name=\"save\"; filename=\"%s.gss\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n", boundary, job->saveID );
          const char* suffix = "\r\n------GothicSaveSyncBoundary--\r\n";
          size_t prefixLength = strlen(prefix);
          size_t suffixLength = strlen(suffix);
          size_t bodyLength = prefixLength + (size_t)length + suffixLength;
          char* body = new char[bodyLength];
          memcpy( body, prefix, prefixLength );
          if( fread( body + prefixLength, 1, (size_t)length, package ) == (size_t)length ) {
            memcpy( body + prefixLength + (size_t)length, suffix, suffixLength );
            HINTERNET session = 0;
            HINTERNET request = OpenRequest( url, L"POST", session );
            if( request ) {
              wchar_t headers[256];
              _snwprintf_s( headers, _countof(headers), _TRUNCATE,
                L"Content-Type: multipart/form-data; boundary=%S\r\n", boundary );
              WinHttpSendRequest( request, headers, (DWORD)-1L, body,
                (DWORD)bodyLength, (DWORD)bodyLength, 0 );
              WinHttpReceiveResponse( request, 0 );
              WinHttpCloseHandle( request );
              WinHttpCloseHandle( session );
            }
          }
          delete[] body;
        }
        fclose( package );
      }
    }
    delete job;
    return 0;
  }
}

namespace RemoteSync {
  void Start() {
    char serverUrl[1024] = {};
    if( !ReadServerUrl( serverUrl, sizeof(serverUrl) ) )
      return;
    strcpy_s( ConfiguredServerUrl, serverUrl );
    char* argument = new char[strlen(serverUrl) + 1];
    strcpy_s( argument, strlen(serverUrl) + 1, serverUrl );
    CloseHandle( CreateThread( 0, 0, StartupThread, argument, 0, 0 ) );
  }

  void PollUi() {
    if( ConfiguredServerUrl[0] == 0 )
      return;
    LONG status = InterlockedCompareExchange( &ServerStatus, 0, 0 );
    if( status != 0 && !ServerStatusShown ) {
      ServerStatusShown = true;
      if( status > 0 )
        Common::Message::Info( "Servidor remoto disponible. GothicSaveSync puede sincronizar partidas.",
          "GothicSaveSync" );
      else
        Common::Message::Warning( "No se puede conectar con el servidor remoto. Las partidas locales siguen disponibles.",
          "GothicSaveSync" );
    }
    if( RemotePromptShown || RemoteDownloadStarted || status <= 0 )
      return;
    if( ReadRemoteSaves() == 0 || !HasNewerRemoteSave() )
      return;

    RemotePromptShown = true;
    char message[256];
    _snprintf_s( message, sizeof(message), _TRUNCATE,
      "Hay %u partidas mas avanzadas en el servidor remoto. Quieres sincronizarlas?",
      CountNewerRemoteSaves() );
    if( Common::Message::Question( message, "GothicSaveSync" ) ) {
      RemoteDownloadStarted = true;
      char* serverUrl = new char[strlen(ConfiguredServerUrl) + 1];
      strcpy_s( serverUrl, strlen(ConfiguredServerUrl) + 1, ConfiguredServerUrl );
      CloseHandle( CreateThread( 0, 0, DownloadThread, serverUrl, 0, 0 ) );
    }
  }

  void UploadSave( const char* saveID, const char* packagePath ) {
    char serverUrl[1024] = {};
    if( !ReadServerUrl( serverUrl, sizeof(serverUrl) ) )
      return;
    UploadJob* job = new UploadJob();
    strcpy_s( job->saveID, saveID );
    strcpy_s( job->packagePath, packagePath );
    strcpy_s( job->serverUrl, serverUrl );
    CloseHandle( CreateThread( 0, 0, UploadThread, job, 0, 0 ) );
  }
}
