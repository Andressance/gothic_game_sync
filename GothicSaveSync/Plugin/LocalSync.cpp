// -----------------------------------------------------------------------
// LocalSync.cpp — Peer-to-peer LAN save synchronisation
//
// Discovery : UDP broadcast on port 19216
// Data channel : TCP on port 19217
// Protocol : binary framed messages (type + length + payload)
//
// A deterministic tie-breaker (session ID comparison) ensures that
// exactly ONE TCP connection is established even when both peers
// discover each other simultaneously.
// -----------------------------------------------------------------------

// winsock2 MUST come before any header that pulls in windows.h
#include <winsock2.h>
#include <ws2tcpip.h>

#include "LocalSync.h"
#include "SaveSync.h"
#include "SyncHistory.h"
#include "SyncLog.h"
#include "plugin.h"
#include "UnionAfx.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

namespace {
  // ---- constants --------------------------------------------------------
  static const unsigned short UdpPort           = 19216;
  static const unsigned short TcpPort           = 19217;
  static const int            MaxSlots          = 20;
  static const unsigned int   BroadcastInterval = 2000;   // ms
  static const unsigned int   SelectTimeout     = 500;    // ms
  static const char           DiscoverMagic[]   = "GSS_DISCOVER";
  static const unsigned int   MaxTransferSize   = 128 * 1024 * 1024;

  // ---- TCP message types ------------------------------------------------
  enum MsgType {
    MSG_HANDSHAKE    = 1,
    MSG_SLOT_LIST    = 2,
    MSG_REQUEST_SAVE = 3,
    MSG_SAVE_DATA    = 4,
    MSG_ACK          = 5,
    MSG_NACK         = 6
  };

  #pragma pack(push, 1)
  struct MsgHeader {
    unsigned char  type;
    unsigned int   payloadSize;
  };
  #pragma pack(pop)

  // ---- shared state -----------------------------------------------------
  CRITICAL_SECTION SyncLock;
  bool             WinsockReady = false;

  volatile LocalSync::State CurrentState = LocalSync::STATE_IDLE;
  SOCKET           ConnSocket    = INVALID_SOCKET;
  char             PeerNameBuf[64]  = {};
  char             StatusBuf[256]   = {};

  SaveSync::SaveSlotInfo LocalSlotsBuf[MaxSlots];
  volatile int           LocalSlotCount = 0;
  SaveSync::SaveSlotInfo RemoteSlotsBuf[MaxSlots];
  volatile int           RemoteSlotCount = 0;

  volatile int  PendingSend   = -1;
  volatile int  PendingRecv   = -1;
  volatile bool TransferDone  = false;
  volatile bool TransferOk    = false;
  volatile bool StopFlag      = false;
  volatile bool NeedsRefresh  = false;

  HANDLE hDiscoveryThread   = 0;
  HANDLE hConnectionThread  = 0;

  char         MachineName[64] = {};
  unsigned int SessionID       = 0;

  // ---- helpers ----------------------------------------------------------

  void GenerateSessionID() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter( &counter );
    SessionID = (unsigned int)(counter.LowPart ^ GetCurrentProcessId() ^ GetTickCount());
  }

  void GetMachineName() {
    DWORD size = sizeof(MachineName);
    if( !GetComputerNameA( MachineName, &size ) )
      strcpy_s( MachineName, "Gothic-PC" );
  }

  // Send exactly `len` bytes.
  bool SendAll( SOCKET s, const void* data, int len ) {
    const char* ptr = (const char*)data;
    int remaining = len;
    while( remaining > 0 ) {
      int sent = send( s, ptr, remaining, 0 );
      if( sent <= 0 ) return false;
      ptr += sent;
      remaining -= sent;
    }
    return true;
  }

  // Receive exactly `len` bytes.
  bool RecvAll( SOCKET s, void* data, int len ) {
    char* ptr = (char*)data;
    int remaining = len;
    while( remaining > 0 ) {
      int received = recv( s, ptr, remaining, 0 );
      if( received <= 0 ) return false;
      ptr += received;
      remaining -= received;
    }
    return true;
  }

  bool SendMsg( SOCKET s, unsigned char type, const void* payload, unsigned int payloadSize ) {
    MsgHeader hdr;
    hdr.type        = type;
    hdr.payloadSize = payloadSize;
    if( !SendAll( s, &hdr, sizeof(hdr) ) ) return false;
    if( payloadSize > 0 && payload )
      return SendAll( s, payload, (int)payloadSize );
    return true;
  }

  bool RecvHeader( SOCKET s, MsgHeader& hdr ) {
    return RecvAll( s, &hdr, sizeof(hdr) );
  }

  void SetStatus( const char* msg ) {
    EnterCriticalSection( &SyncLock );
    strcpy_s( StatusBuf, msg );
    LeaveCriticalSection( &SyncLock );
  }

  void SetState( LocalSync::State state ) {
    InterlockedExchange( (LONG*)&CurrentState, (LONG)state );
  }

  void RefreshLocalSlots() {
    EnterCriticalSection( &SyncLock );
    LocalSlotCount = SaveSync::GetSaveSlotList( LocalSlotsBuf, MaxSlots );
    LeaveCriticalSection( &SyncLock );
  }

  // ---- JSON helpers (manual, no library) --------------------------------

  int BuildSlotListJson( const SaveSync::SaveSlotInfo* slots, int count,
    char* output, int outputSize ) {
    int pos = 0;
    int r;
    r = _snprintf_s( output + pos, outputSize - pos, _TRUNCATE, "[" );
    if( r > 0 ) pos += r;
    for( int i = 0; i < count; ++i ) {
      if( pos >= outputSize - 1 ) break;
      if( i > 0 ) {
        r = _snprintf_s( output + pos, outputSize - pos, _TRUNCATE, "," );
        if( r > 0 ) pos += r;
      }
      r = _snprintf_s( output + pos, outputSize - pos, _TRUNCATE,
        "{\"id\":%d,\"name\":\"%s\",\"date\":\"%s\",\"ts\":%lld}",
        slots[i].slotID, slots[i].name, slots[i].dateStr, slots[i].timestamp );
      if( r > 0 ) pos += r;
    }
    if( pos < outputSize - 1 ) {
      r = _snprintf_s( output + pos, outputSize - pos, _TRUNCATE, "]" );
      if( r > 0 ) pos += r;
    }
    return pos;
  }

  int ParseSlotListJson( const char* json, SaveSync::SaveSlotInfo* slots, int maxSlots ) {
    int count = 0;
    const char* cursor = json;
    while( count < maxSlots ) {
      const char* idField = strstr( cursor, "\"id\":" );
      if( !idField ) break;
      idField += 5;

      const char* nameField = strstr( idField, "\"name\":\"" );
      if( !nameField ) break;
      nameField += 8;
      const char* nameEnd = strchr( nameField, '"' );
      if( !nameEnd ) break;

      const char* dateField = strstr( nameEnd, "\"date\":\"" );
      if( !dateField ) break;
      dateField += 8;
      const char* dateEnd = strchr( dateField, '"' );
      if( !dateEnd ) break;

      const char* tsField = strstr( dateEnd, "\"ts\":" );
      if( !tsField ) break;
      tsField += 5;

      slots[count].slotID = atoi( idField );
      slots[count].exists = true;

      size_t nameLen = (size_t)(nameEnd - nameField);
      if( nameLen >= sizeof(slots[count].name) ) nameLen = sizeof(slots[count].name) - 1;
      memcpy( slots[count].name, nameField, nameLen );
      slots[count].name[nameLen] = 0;

      size_t dateLen = (size_t)(dateEnd - dateField);
      if( dateLen >= sizeof(slots[count].dateStr) ) dateLen = sizeof(slots[count].dateStr) - 1;
      memcpy( slots[count].dateStr, dateField, dateLen );
      slots[count].dateStr[dateLen] = 0;

      slots[count].timestamp = _atoi64( tsField );

      ++count;
      cursor = tsField;
    }
    return count;
  }

  // ---- slot-list exchange -----------------------------------------------

  bool SendSlotList( SOCKET s ) {
    char json[4096];
    EnterCriticalSection( &SyncLock );
    int len = BuildSlotListJson( LocalSlotsBuf, LocalSlotCount, json, sizeof(json) );
    LeaveCriticalSection( &SyncLock );
    return SendMsg( s, MSG_SLOT_LIST, json, (unsigned int)(len + 1) );
  }

  bool ReceiveSlotList( SOCKET s ) {
    MsgHeader hdr;
    if( !RecvHeader( s, hdr ) || hdr.type != MSG_SLOT_LIST || hdr.payloadSize > 8192 )
      return false;
    char* json = new char[hdr.payloadSize + 1];
    if( !RecvAll( s, json, (int)hdr.payloadSize ) ) {
      delete[] json;
      return false;
    }
    json[hdr.payloadSize] = 0;
    EnterCriticalSection( &SyncLock );
    RemoteSlotCount = ParseSlotListJson( json, RemoteSlotsBuf, MaxSlots );
    LeaveCriticalSection( &SyncLock );
    delete[] json;
    return true;
  }

  // ---- save transfer helpers --------------------------------------------

  // Receive a MSG_SAVE_DATA payload, install the package, return success.
  bool HandleReceiveSave( SOCKET s, unsigned int payloadSize ) {
    if( payloadSize < 12 || payloadSize > MaxTransferSize ) return false;

    int slotID = 0;
    unsigned __int64 fileSize = 0;
    if( !RecvAll( s, &slotID, 4 ) ) return false;
    if( !RecvAll( s, &fileSize, 8 ) ) return false;

    unsigned int dataSize = payloadSize - 12;
    if( (unsigned __int64)dataSize != fileSize || dataSize > MaxTransferSize ) return false;

    const char* slotName = UnionCore::TSaveLoadGameInfo::GetSaveSlotName( slotID ).ToChar();
    Common::string gameDir = UnionCore::Union.GetGameDirectory();
    const char* saveDir = zoptions->GetDirString( DIR_SAVEGAMES ).ToChar();

    char syncDir[MAX_PATH];
    char pendingDir[MAX_PATH];
    char targetPath[MAX_PATH];
    char tempPath[MAX_PATH];
    if( saveDir[0] == '\\' || saveDir[0] == '/' ) {
      _snprintf_s( syncDir, sizeof(syncDir), _TRUNCATE, "%s%ssave-sync",
        gameDir.ToChar(), saveDir );
      _snprintf_s( pendingDir, sizeof(pendingDir), _TRUNCATE, "%s%ssave-sync\\pending",
        gameDir.ToChar(), saveDir );
    } else {
      _snprintf_s( syncDir, sizeof(syncDir), _TRUNCATE, "%s\\%s\\save-sync",
        gameDir.ToChar(), saveDir );
      _snprintf_s( pendingDir, sizeof(pendingDir), _TRUNCATE, "%s\\%s\\save-sync\\pending",
        gameDir.ToChar(), saveDir );
    }
    CreateDirectoryA( syncDir, 0 );
    CreateDirectoryA( pendingDir, 0 );
    _snprintf_s( targetPath, sizeof(targetPath), _TRUNCATE, "%s\\%s.gss", pendingDir, slotName );
    _snprintf_s( tempPath, sizeof(tempPath), _TRUNCATE, "%s.lantmp", targetPath );

    FILE* file = 0;
    fopen_s( &file, tempPath, "wb" );
    if( !file ) return false;

    char buffer[8192];
    unsigned int remaining = dataSize;
    bool ok = true;
    while( remaining > 0 && ok ) {
      unsigned int chunk = remaining > sizeof(buffer) ? (unsigned int)sizeof(buffer) : remaining;
      if( !RecvAll( s, buffer, (int)chunk ) ) { ok = false; break; }
      if( fwrite( buffer, 1, chunk, file ) != chunk ) { ok = false; break; }
      remaining -= chunk;
    }
    fclose( file );

    if( !ok ) {
      DeleteFileA( tempPath );
      return false;
    }

    SyncHistory::RecordEntry( slotName, slotID, "lan", PeerNameBuf );

    MoveFileExA( tempPath, targetPath, MOVEFILE_REPLACE_EXISTING );

    bool installed = SaveSync::InstallPendingPackage( slotName, slotID, "lan" );
    SyncLog::Write( "LocalSync: received slot %d (%s) -> installed=%d", slotID, slotName, installed );

    if( installed ) {
      NeedsRefresh = true;
      RefreshLocalSlots();
    }
    return installed;
  }

  // Package a slot and send it over TCP.
  bool HandleSendRequest( SOCKET s, int slotID ) {
    Common::string gameDir = UnionCore::Union.GetGameDirectory();
    const char* saveDir = zoptions->GetDirString( DIR_SAVEGAMES ).ToChar();

    char syncDir[MAX_PATH];
    char tempPath[MAX_PATH];
    if( saveDir[0] == '\\' || saveDir[0] == '/' ) {
      _snprintf_s( syncDir, sizeof(syncDir), _TRUNCATE, "%s%ssave-sync",
        gameDir.ToChar(), saveDir );
    } else {
      _snprintf_s( syncDir, sizeof(syncDir), _TRUNCATE, "%s\\%s\\save-sync",
        gameDir.ToChar(), saveDir );
    }
    CreateDirectoryA( syncDir, 0 );
    _snprintf_s( tempPath, sizeof(tempPath), _TRUNCATE, "%s\\lansend.gss", syncDir );

    if( !SaveSync::PackSlotToFile( slotID, tempPath ) ) {
      SyncLog::Write( "LocalSync: failed to pack slot %d", slotID );
      SendMsg( s, MSG_NACK, 0, 0 );
      return false;
    }

    FILE* file = 0;
    fopen_s( &file, tempPath, "rb" );
    if( !file ) {
      SendMsg( s, MSG_NACK, 0, 0 );
      return false;
    }
    fseek( file, 0, SEEK_END );
    long fileSize = ftell( file );
    fseek( file, 0, SEEK_SET );

    if( fileSize <= 0 ) {
      fclose( file );
      SendMsg( s, MSG_NACK, 0, 0 );
      return false;
    }

    // header + slotID(4) + fileSize(8) + data
    unsigned int payloadSize = 4 + 8 + (unsigned int)fileSize;
    MsgHeader hdr;
    hdr.type        = MSG_SAVE_DATA;
    hdr.payloadSize = payloadSize;
    unsigned __int64 size64 = (unsigned __int64)fileSize;

    bool ok = SendAll( s, &hdr, sizeof(hdr) ) &&
              SendAll( s, &slotID, 4 ) &&
              SendAll( s, &size64, 8 );

    if( ok ) {
      char buf[8192];
      size_t readBytes;
      while( ok && (readBytes = fread( buf, 1, sizeof(buf), file )) > 0 )
        ok = SendAll( s, buf, (int)readBytes );
    }

    fclose( file );
    DeleteFileA( tempPath );

    SyncLog::Write( "LocalSync: sent slot %d (size=%ld) -> %d", slotID, fileSize, ok );
    return ok;
  }

  // Re-exchange updated slot lists after a transfer.
  void ReExchangeSlots( SOCKET s ) {
    RefreshLocalSlots();
    SendSlotList( s );

    MsgHeader hdr;
    if( RecvHeader( s, hdr ) && hdr.type == MSG_SLOT_LIST && hdr.payloadSize <= 8192 ) {
      char* json = new char[hdr.payloadSize + 1];
      if( RecvAll( s, json, (int)hdr.payloadSize ) ) {
        json[hdr.payloadSize] = 0;
        EnterCriticalSection( &SyncLock );
        RemoteSlotCount = ParseSlotListJson( json, RemoteSlotsBuf, MaxSlots );
        LeaveCriticalSection( &SyncLock );
      }
      delete[] json;
    }
  }

  // ---- connection thread ------------------------------------------------

  DWORD WINAPI ConnectionThreadProc( void* arg ) {
    SOCKET s = (SOCKET)(UINT_PTR)arg;
    SyncLog::Write( "LocalSync: connection thread started" );

    // --- handshake ---
    if( !SendMsg( s, MSG_HANDSHAKE, MachineName, (unsigned int)strlen( MachineName ) + 1 ) ) {
      SetStatus( "Error en handshake" );
      SetState( LocalSync::STATE_ERROR );
      closesocket( s );
      return 0;
    }

    MsgHeader hdr;
    if( !RecvHeader( s, hdr ) || hdr.type != MSG_HANDSHAKE || hdr.payloadSize > 64 ) {
      SetStatus( "Error en handshake remoto" );
      SetState( LocalSync::STATE_ERROR );
      closesocket( s );
      return 0;
    }

    char remoteName[64] = {};
    if( hdr.payloadSize > 0 )
      RecvAll( s, remoteName, (int)hdr.payloadSize );

    EnterCriticalSection( &SyncLock );
    strcpy_s( PeerNameBuf, remoteName );
    LeaveCriticalSection( &SyncLock );
    SyncLog::Write( "LocalSync: connected to '%s'", remoteName );

    // --- initial slot exchange ---
    RefreshLocalSlots();
    if( !SendSlotList( s ) || !ReceiveSlotList( s ) ) {
      SetStatus( "Error intercambiando lista de partidas" );
      SetState( LocalSync::STATE_ERROR );
      closesocket( s );
      return 0;
    }

    char statusMsg[128];
    _snprintf_s( statusMsg, sizeof(statusMsg), _TRUNCATE, "Conectado con '%s'", remoteName );
    SetStatus( statusMsg );
    SetState( LocalSync::STATE_CONNECTED );

    // --- main message loop ---
    while( !StopFlag ) {
      // -- outgoing requests from the UI thread --
      int sendReq = InterlockedExchange( (LONG*)&PendingSend, -1 );
      int recvReq = InterlockedExchange( (LONG*)&PendingRecv, -1 );

      if( sendReq >= 0 ) {
        SetState( LocalSync::STATE_TRANSFERRING );
        SetStatus( "Enviando partida..." );
        bool ok = HandleSendRequest( s, sendReq );
        if( ok ) {
          MsgHeader ack;
          ok = RecvHeader( s, ack ) && ack.type == MSG_ACK;
        }
        if( ok ) ReExchangeSlots( s );
        TransferOk   = ok;
        TransferDone = true;
        SetState( LocalSync::STATE_CONNECTED );
        SetStatus( ok ? "Partida enviada correctamente" : "Error al enviar la partida" );
      }

      if( recvReq >= 0 ) {
        SetState( LocalSync::STATE_TRANSFERRING );
        SetStatus( "Solicitando partida..." );
        bool ok = SendMsg( s, MSG_REQUEST_SAVE, &recvReq, 4 );
        if( ok ) {
          MsgHeader resp;
          if( RecvHeader( s, resp ) && resp.type == MSG_SAVE_DATA ) {
            SetStatus( "Recibiendo partida..." );
            ok = HandleReceiveSave( s, resp.payloadSize );
            SendMsg( s, ok ? MSG_ACK : MSG_NACK, 0, 0 );
          } else {
            ok = false;
          }
        }
        if( ok ) ReExchangeSlots( s );
        TransferOk   = ok;
        TransferDone = true;
        SetState( LocalSync::STATE_CONNECTED );
        SetStatus( ok ? "Partida recibida correctamente" : "Error al recibir la partida" );
      }

      // -- incoming messages from peer --
      fd_set readSet;
      FD_ZERO( &readSet );
      FD_SET( s, &readSet );
      timeval tv;
      tv.tv_sec  = 0;
      tv.tv_usec = 100 * 1000; // 100 ms

      int sel = select( 0, &readSet, 0, 0, &tv );
      if( sel > 0 && FD_ISSET( s, &readSet ) ) {
        MsgHeader inc;
        if( !RecvHeader( s, inc ) ) {
          SyncLog::Write( "LocalSync: peer disconnected" );
          SetStatus( "Conexion perdida" );
          SetState( LocalSync::STATE_ERROR );
          break;
        }

        switch( inc.type ) {
          case MSG_REQUEST_SAVE: {
            int reqSlotID = 0;
            if( inc.payloadSize == 4 && RecvAll( s, &reqSlotID, 4 ) ) {
              SyncLog::Write( "LocalSync: peer requested slot %d", reqSlotID );
              HandleSendRequest( s, reqSlotID );
              MsgHeader ack;
              RecvHeader( s, ack ); // ACK/NACK from receiver
              ReExchangeSlots( s );
            }
            break;
          }

          case MSG_SAVE_DATA: {
            SyncLog::Write( "LocalSync: receiving save from peer" );
            bool ok = HandleReceiveSave( s, inc.payloadSize );
            SendMsg( s, ok ? MSG_ACK : MSG_NACK, 0, 0 );
            if( ok ) ReExchangeSlots( s );
            break;
          }

          case MSG_SLOT_LIST: {
            if( inc.payloadSize <= 8192 ) {
              char* json = new char[inc.payloadSize + 1];
              if( RecvAll( s, json, (int)inc.payloadSize ) ) {
                json[inc.payloadSize] = 0;
                EnterCriticalSection( &SyncLock );
                RemoteSlotCount = ParseSlotListJson( json, RemoteSlotsBuf, MaxSlots );
                LeaveCriticalSection( &SyncLock );
              }
              delete[] json;
            }
            break;
          }

          default: {
            // Skip unknown payload.
            if( inc.payloadSize > 0 && inc.payloadSize < 65536 ) {
              char* skip = new char[inc.payloadSize];
              RecvAll( s, skip, (int)inc.payloadSize );
              delete[] skip;
            }
            break;
          }
        }
      }
    }

    closesocket( s );
    EnterCriticalSection( &SyncLock );
    ConnSocket = INVALID_SOCKET;
    LeaveCriticalSection( &SyncLock );

    if( CurrentState != LocalSync::STATE_ERROR )
      SetState( LocalSync::STATE_IDLE );

    SyncLog::Write( "LocalSync: connection thread ended" );
    return 0;
  }

  // ---- discovery thread -------------------------------------------------
  // Sends UDP broadcast, listens for peers, and accepts / initiates TCP.

  DWORD WINAPI DiscoveryThreadProc( void* ) {
    SyncLog::Write( "LocalSync: discovery thread started" );
    SetStatus( "Buscando dispositivos en la red..." );

    // UDP socket for broadcast / listen
    SOCKET udpSock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if( udpSock == INVALID_SOCKET ) {
      SyncLog::Write( "LocalSync: UDP socket failed (err=%d)", WSAGetLastError() );
      SetStatus( "Error al crear socket UDP" );
      SetState( LocalSync::STATE_ERROR );
      return 0;
    }

    BOOL optTrue = TRUE;
    setsockopt( udpSock, SOL_SOCKET, SO_BROADCAST, (const char*)&optTrue, sizeof(optTrue) );
    setsockopt( udpSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&optTrue, sizeof(optTrue) );

    sockaddr_in udpAddr = {};
    udpAddr.sin_family      = AF_INET;
    udpAddr.sin_addr.s_addr = INADDR_ANY;
    udpAddr.sin_port        = htons( UdpPort );

    if( bind( udpSock, (sockaddr*)&udpAddr, sizeof(udpAddr) ) == SOCKET_ERROR ) {
      SyncLog::Write( "LocalSync: UDP bind failed (err=%d)", WSAGetLastError() );
      closesocket( udpSock );
      SetStatus( "Error: puerto UDP 19216 en uso" );
      SetState( LocalSync::STATE_ERROR );
      return 0;
    }

    // TCP listen socket
    SOCKET tcpListen = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if( tcpListen == INVALID_SOCKET ) {
      closesocket( udpSock );
      SetStatus( "Error al crear socket TCP" );
      SetState( LocalSync::STATE_ERROR );
      return 0;
    }

    setsockopt( tcpListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&optTrue, sizeof(optTrue) );

    sockaddr_in tcpAddr = {};
    tcpAddr.sin_family      = AF_INET;
    tcpAddr.sin_addr.s_addr = INADDR_ANY;
    tcpAddr.sin_port        = htons( TcpPort );

    if( bind( tcpListen, (sockaddr*)&tcpAddr, sizeof(tcpAddr) ) == SOCKET_ERROR ||
        listen( tcpListen, 1 ) == SOCKET_ERROR ) {
      closesocket( udpSock );
      closesocket( tcpListen );
      SetStatus( "Error: puerto TCP 19217 en uso" );
      SetState( LocalSync::STATE_ERROR );
      return 0;
    }

    DWORD lastBroadcast = 0;
    bool  connected     = false;

    while( !StopFlag && !connected ) {
      DWORD now = GetTickCount();

      // --- periodic broadcast ---
      if( now - lastBroadcast >= BroadcastInterval ) {
        char msg[128];
        _snprintf_s( msg, sizeof(msg), _TRUNCATE, "%s|%08X|%s",
          DiscoverMagic, SessionID, MachineName );

        sockaddr_in dst = {};
        dst.sin_family      = AF_INET;
        dst.sin_addr.s_addr = INADDR_BROADCAST;
        dst.sin_port        = htons( UdpPort );
        sendto( udpSock, msg, (int)strlen( msg ), 0, (sockaddr*)&dst, sizeof(dst) );
        lastBroadcast = now;
      }

      // --- select on UDP + TCP listen ---
      fd_set readSet;
      FD_ZERO( &readSet );
      FD_SET( udpSock, &readSet );
      FD_SET( tcpListen, &readSet );

      timeval tv;
      tv.tv_sec  = 0;
      tv.tv_usec = SelectTimeout * 1000;

      int sel = select( 0, &readSet, 0, 0, &tv );
      if( sel <= 0 ) continue;

      // --- check UDP ---
      if( FD_ISSET( udpSock, &readSet ) ) {
        char buf[256] = {};
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        int received = recvfrom( udpSock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen );
        if( received > 0 ) {
          buf[received] = 0;

          if( strncmp( buf, DiscoverMagic, strlen(DiscoverMagic) ) == 0 ) {
            // Parse "GSS_DISCOVER|<session_hex>|<name>"
            const char* sessionStr = buf + strlen( DiscoverMagic ) + 1;
            unsigned int peerSession = 0;
            sscanf_s( sessionStr, "%8X", &peerSession );

            const char* sep = strchr( sessionStr, '|' );
            const char* peerName = sep ? sep + 1 : "Desconocido";

            if( peerSession == SessionID )
              continue; // own broadcast — ignore

            SyncLog::Write( "LocalSync: discovered '%s' session=%08X (ours=%08X)",
              peerName, peerSession, SessionID );

            // Tie-breaker: lower session ID is the TCP client.
            if( SessionID < peerSession ) {
              SetStatus( "Conectando..." );

              SOCKET clientSock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
              if( clientSock != INVALID_SOCKET ) {
                DWORD connTimeout = 3000;
                setsockopt( clientSock, SOL_SOCKET, SO_SNDTIMEO,
                  (const char*)&connTimeout, sizeof(connTimeout) );

                sockaddr_in peer = {};
                peer.sin_family = AF_INET;
                peer.sin_addr   = from.sin_addr;
                peer.sin_port   = htons( TcpPort );

                if( connect( clientSock, (sockaddr*)&peer, sizeof(peer) ) == 0 ) {
                  SyncLog::Write( "LocalSync: TCP connected as client" );
                  connected  = true;
                  ConnSocket = clientSock;
                  hConnectionThread = CreateThread( 0, 0, ConnectionThreadProc,
                    (void*)(UINT_PTR)clientSock, 0, 0 );
                } else {
                  SyncLog::Write( "LocalSync: TCP connect failed (err=%d)", WSAGetLastError() );
                  closesocket( clientSock );
                }
              }
            }
            // else: we are the server — just keep listening on TCP
          }
        }
      }

      // --- check TCP accept ---
      if( !connected && FD_ISSET( tcpListen, &readSet ) ) {
        sockaddr_in clientAddr = {};
        int clientLen = sizeof(clientAddr);
        SOCKET accepted = accept( tcpListen, (sockaddr*)&clientAddr, &clientLen );
        if( accepted != INVALID_SOCKET ) {
          SyncLog::Write( "LocalSync: TCP accepted from %s",
            inet_ntoa( clientAddr.sin_addr ) );
          connected  = true;
          ConnSocket = accepted;
          hConnectionThread = CreateThread( 0, 0, ConnectionThreadProc,
            (void*)(UINT_PTR)accepted, 0, 0 );
        }
      }
    }

    closesocket( udpSock );
    closesocket( tcpListen );
    SyncLog::Write( "LocalSync: discovery thread ended (connected=%d)", connected );
    return 0;
  }
}

// =========================================================================
// Public API
// =========================================================================

namespace LocalSync {

  void Init() {
    InitializeCriticalSection( &SyncLock );
    GetMachineName();
    GenerateSessionID();

    WSADATA wsaData;
    if( WSAStartup( MAKEWORD(2, 2), &wsaData ) == 0 ) {
      WinsockReady = true;
      SyncLog::Write( "LocalSync::Init: Winsock ready (session=%08X)", SessionID );
    } else {
      SyncLog::Write( "LocalSync::Init: WSAStartup failed" );
    }
  }

  void Shutdown() {
    Disconnect();
    if( WinsockReady ) {
      WSACleanup();
      WinsockReady = false;
    }
    DeleteCriticalSection( &SyncLock );
  }

  void StartDiscovery() {
    if( CurrentState != STATE_IDLE ) return;
    if( !WinsockReady ) {
      SetStatus( "Error: Winsock no disponible" );
      SetState( STATE_ERROR );
      return;
    }

    StopFlag      = false;
    TransferDone  = false;
    TransferOk    = false;
    PendingSend   = -1;
    PendingRecv   = -1;
    RemoteSlotCount = 0;

    RefreshLocalSlots();
    SetState( STATE_SCANNING );

    hDiscoveryThread = CreateThread( 0, 0, DiscoveryThreadProc, 0, 0, 0 );
    SyncLog::Write( "LocalSync::StartDiscovery" );
  }

  void Disconnect() {
    StopFlag = true;

    EnterCriticalSection( &SyncLock );
    if( ConnSocket != INVALID_SOCKET ) {
      closesocket( ConnSocket );
      ConnSocket = INVALID_SOCKET;
    }
    LeaveCriticalSection( &SyncLock );

    if( hDiscoveryThread ) {
      WaitForSingleObject( hDiscoveryThread, 5000 );
      CloseHandle( hDiscoveryThread );
      hDiscoveryThread = 0;
    }
    if( hConnectionThread ) {
      WaitForSingleObject( hConnectionThread, 5000 );
      CloseHandle( hConnectionThread );
      hConnectionThread = 0;
    }

    SetState( STATE_IDLE );
    SetStatus( "" );
    RemoteSlotCount = 0;
    SyncLog::Write( "LocalSync::Disconnect" );
  }

  void Poll() {
    if( NeedsRefresh ) {
      NeedsRefresh = false;
      if( gameMan && gameMan->savegameManager ) {
        gameMan->savegameManager->Reinit();
        SyncLog::Write( "LocalSync::Poll: savegame manager reinitialized" );
      }
    }
  }

  State GetState() {
    return (State)InterlockedCompareExchange( (LONG*)&CurrentState, 0, 0 );
  }

  const char* GetPeerName() {
    return PeerNameBuf;
  }

  const char* GetStatusMessage() {
    return StatusBuf;
  }

  int GetLocalSlotCount() {
    return LocalSlotCount;
  }

  const SaveSync::SaveSlotInfo* GetLocalSlots() {
    return LocalSlotsBuf;
  }

  int GetRemoteSlotCount() {
    return RemoteSlotCount;
  }

  const SaveSync::SaveSlotInfo* GetRemoteSlots() {
    return RemoteSlotsBuf;
  }

  void RequestSendSave( int localSlotIndex ) {
    if( CurrentState != STATE_CONNECTED ) return;
    if( localSlotIndex < 0 || localSlotIndex >= LocalSlotCount ) return;
    InterlockedExchange( (LONG*)&PendingSend, LocalSlotsBuf[localSlotIndex].slotID );
  }

  void RequestReceiveSave( int remoteSlotIndex ) {
    if( CurrentState != STATE_CONNECTED ) return;
    if( remoteSlotIndex < 0 || remoteSlotIndex >= RemoteSlotCount ) return;
    InterlockedExchange( (LONG*)&PendingRecv, RemoteSlotsBuf[remoteSlotIndex].slotID );
  }

  bool IsTransferComplete() {
    return TransferDone;
  }

  bool IsTransferSuccess() {
    return TransferOk;
  }

  void AcknowledgeTransfer() {
    TransferDone = false;
    TransferOk   = false;
  }
}
