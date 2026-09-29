// -----------------------------------------------------------------------
// ManualSyncUi.cpp — Two-column overlay for manual per-slot management
//                    of local vs. remote saves.
//
// Layout (virtual coords 0–8192):
//   +---------------------------------------------------+
//   |          Gestion Manual de Partidas                |
//   |      Servidor: https://...                        |
//   |                                                   |
//   |  PARTIDAS LOCALES     | PARTIDAS EN SERVIDOR      |
//   |  --------------------   ----------------------    |
//   |  >> savegame0          |    savegame0             |
//   |     28/09/2026 18:00   |    28/09/2026 17:30      |
//   |     savegame1          |    savegame2             |
//   |     28/09/2026 16:00   |    28/09/2026 15:00      |
//   |                                                   |
//   |  [Izq/Der: Columna]  [ENTER: Accion]  [B: Backup]|
//   |  [R: Refrescar]  [ESC: Cerrar]                    |
//   +---------------------------------------------------+
//
// Actions:
//   - Left column (local)  + ENTER = Upload selected slot to server
//   - Right column (remote) + ENTER = Download selected save from server
//   - B = Restore backup for the currently selected local slot
//   - R = Refresh both lists
// -----------------------------------------------------------------------

#include "ManualSyncUi.h"
#include "RemoteSync.h"
#include "SaveSync.h"
#include "SyncLog.h"
#include "plugin.h"
#include "UnionAfx.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {
  // ---- layout constants (virtual 8192 coords) --------------------------
  static const int VMax        = 8192;
  static const int PanelW      = 7000;
  static const int PanelH      = 6200;
  static const int PanelX      = (VMax - PanelW) / 2;
  static const int PanelY      = (VMax - PanelH) / 2;

  static const int MarginX     = 200;
  static const int MarginY     = 150;
  static const int LineH       = 270;
  static const int ColDivider  = PanelW / 2;
  static const int MaxLocalSlots  = 20;
  static const int MaxRemoteSlots = 32;
  static const int VisibleRows = 8;

  // ---- state ------------------------------------------------------------
  bool     Active           = false;
  zCView*  ManualView       = 0;
  int      SelColumn        = 0;   // 0 = local, 1 = remote
  int      LocalCursor      = 0;
  int      RemoteCursor     = 0;
  int      LocalScrollOff   = 0;
  int      RemoteScrollOff  = 0;
  float    ResultTimer      = 0.0f;
  bool     ShowingResult    = false;
  char     ResultMsg[256]   = {};
  float    LastFrameTime    = 0.0f;
  bool     LoadingRemote    = false;

  SaveSync::SaveSlotInfo LocalSlots[MaxLocalSlots];
  int LocalSlotCount = 0;

  RemoteSync::RemoteSlotInfo RemoteSlots[MaxRemoteSlots];
  int RemoteSlotCount = 0;

  // ---- input helper (same pattern as other overlays) --------------------
  bool KeyToggled( int vk ) {
    static bool pressed[256] = {};
    if( vk < 0 || vk > 255 ) return false;
    bool down = (GetAsyncKeyState( vk ) & 0x8000) != 0;
    if( down && !pressed[vk] ) { pressed[vk] = true; return true; }
    if( !down ) pressed[vk] = false;
    return false;
  }

  float GetEngineTime() {
    if( ztimer ) return ztimer->totalTimeFloat / 1000.0f;
    return 0.0f;
  }

  // ---- view management --------------------------------------------------
  void CreateManualPanel() {
    if( ManualView && screen ) {
      screen->RemoveItem( ManualView );
      delete ManualView;
    }
    ManualView = new zCView( PanelX, PanelY, PanelX + PanelW, PanelY + PanelH );
    ManualView->InsertBack( "DLG_CONVERSATION.TGA" );
    ManualView->SetAlphaBlendFunc( zRND_ALPHA_FUNC_BLEND );
    ManualView->SetTransparency( 30 );
    ManualView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    if( screen )
      screen->InsertItem( ManualView );
  }

  void DestroyManualPanel() {
    if( ManualView && screen ) {
      screen->RemoveItem( ManualView );
      delete ManualView;
      ManualView = 0;
    }
  }

  // ---- data loading -----------------------------------------------------
  void RefreshLocalSlots() {
    LocalSlotCount = SaveSync::GetSaveSlotList( LocalSlots, MaxLocalSlots );
    SyncLog::Write( "ManualSyncUi: refreshed %d local slots", LocalSlotCount );
  }

  // Fetching remote slots (blocking – called once on open and on refresh).
  DWORD WINAPI FetchRemoteThread( void* ) {
    RemoteSlotCount = RemoteSync::FetchRemoteSlotList( RemoteSlots, MaxRemoteSlots );
    LoadingRemote = false;
    SyncLog::Write( "ManualSyncUi: fetched %d remote slots", RemoteSlotCount );
    return 0;
  }

  void RefreshRemoteSlots() {
    if( !RemoteSync::IsServerConfigured() ) {
      RemoteSlotCount = 0;
      return;
    }
    LoadingRemote = true;
    CloseHandle( CreateThread( 0, 0, FetchRemoteThread, 0, 0, 0 ) );
  }

  // ---- rendering helpers ------------------------------------------------
  void RenderHeader() {
    if( !ManualView ) return;

    zCOLOR gold( 255, 215, 0, 255 );
    ManualView->SetFontColor( gold );
    ManualView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    ManualView->PrintCX( MarginY, zSTRING( "Gestion Manual de Partidas" ) );

    zCOLOR silver( 200, 200, 200, 255 );
    ManualView->SetFontColor( silver );
    ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );

    const char* url = RemoteSync::GetServerUrl();
    if( url && url[0] != 0 ) {
      char sub[256];
      _snprintf_s( sub, sizeof(sub), _TRUNCATE, "Servidor: %s", url );
      ManualView->PrintCX( MarginY + LineH, zSTRING( sub ) );
    } else {
      ManualView->PrintCX( MarginY + LineH, zSTRING( "(Sin servidor configurado)" ) );
    }
  }

  void RenderFooter() {
    if( !ManualView ) return;

    zCOLOR gray( 160, 160, 160, 255 );
    ManualView->SetFontColor( gray );
    ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );

    int sX, sY;
    ManualView->GetSize( sX, sY );

    if( SelColumn == 0 )
      ManualView->PrintCX( sY - MarginY - LineH,
        zSTRING( "[Izq/Der: Columna]  [Arriba/Abajo: Navegar]  [ENTER: Subir al servidor]" ) );
    else
      ManualView->PrintCX( sY - MarginY - LineH,
        zSTRING( "[Izq/Der: Columna]  [Arriba/Abajo: Navegar]  [ENTER: Descargar]" ) );

    ManualView->PrintCX( sY - MarginY,
      zSTRING( "[B: Restaurar backup]  [R: Refrescar listas]  [ESC: Cerrar]" ) );
  }

  void RenderLocalColumn( int startX, int startY ) {
    if( !ManualView ) return;

    // Column header
    zCOLOR hdrColor( 255, 215, 0, 255 );
    ManualView->SetFontColor( hdrColor );
    ManualView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    ManualView->Print( startX, startY, zSTRING( "Partidas Locales" ) );

    // Divider
    zCOLOR divColor( 100, 100, 100, 255 );
    ManualView->SetFontColor( divColor );
    ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );
    ManualView->Print( startX, startY + LineH, zSTRING( "--------------------" ) );

    int y = startY + LineH * 2;

    if( LocalSlotCount == 0 ) {
      zCOLOR dim( 120, 120, 120, 255 );
      ManualView->SetFontColor( dim );
      ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      ManualView->Print( startX, y, zSTRING( "(sin partidas locales)" ) );
      return;
    }

    int endIdx = LocalScrollOff + VisibleRows;
    if( endIdx > LocalSlotCount ) endIdx = LocalSlotCount;

    for( int i = LocalScrollOff; i < endIdx; ++i ) {
      bool cur = (SelColumn == 0) && (i == LocalCursor);

      if( cur ) {
        zCOLOR sel( 255, 215, 0, 255 );
        ManualView->SetFontColor( sel );
        ManualView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
      } else {
        zCOLOR norm( 200, 200, 200, 255 );
        ManualView->SetFontColor( norm );
        ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      }

      char label[128];
      if( cur )
        _snprintf_s( label, sizeof(label), _TRUNCATE, ">> Slot %d: %s", LocalSlots[i].slotID, LocalSlots[i].name );
      else
        _snprintf_s( label, sizeof(label), _TRUNCATE, "   Slot %d: %s", LocalSlots[i].slotID, LocalSlots[i].name );
      ManualView->Print( startX, y, zSTRING( label ) );
      y += LineH;

      // Date line (always dim)
      zCOLOR dateClr( 150, 150, 150, 255 );
      ManualView->SetFontColor( dateClr );
      ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      char dateLine[128];
      _snprintf_s( dateLine, sizeof(dateLine), _TRUNCATE, "   %s", LocalSlots[i].dateStr );
      ManualView->Print( startX, y, zSTRING( dateLine ) );
      y += LineH;
    }
  }

  void RenderRemoteColumn( int startX, int startY ) {
    if( !ManualView ) return;

    // Column header
    zCOLOR hdrColor( 255, 215, 0, 255 );
    ManualView->SetFontColor( hdrColor );
    ManualView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    ManualView->Print( startX, startY, zSTRING( "Partidas en Servidor" ) );

    // Divider
    zCOLOR divColor( 100, 100, 100, 255 );
    ManualView->SetFontColor( divColor );
    ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );
    ManualView->Print( startX, startY + LineH, zSTRING( "--------------------" ) );

    int y = startY + LineH * 2;

    if( LoadingRemote ) {
      zCOLOR dim( 180, 180, 60, 255 );
      ManualView->SetFontColor( dim );
      ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      ManualView->Print( startX, y, zSTRING( "Cargando..." ) );
      return;
    }

    if( RemoteSlotCount == 0 ) {
      zCOLOR dim( 120, 120, 120, 255 );
      ManualView->SetFontColor( dim );
      ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      ManualView->Print( startX, y, zSTRING( "(sin partidas remotas)" ) );
      return;
    }

    int endIdx = RemoteScrollOff + VisibleRows;
    if( endIdx > RemoteSlotCount ) endIdx = RemoteSlotCount;

    for( int i = RemoteScrollOff; i < endIdx; ++i ) {
      bool cur = (SelColumn == 1) && (i == RemoteCursor);

      if( cur ) {
        zCOLOR sel( 255, 215, 0, 255 );
        ManualView->SetFontColor( sel );
        ManualView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
      } else {
        zCOLOR norm( 200, 200, 200, 255 );
        ManualView->SetFontColor( norm );
        ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      }

      char label[128];
      if( cur )
        _snprintf_s( label, sizeof(label), _TRUNCATE, ">> %s", RemoteSlots[i].saveID );
      else
        _snprintf_s( label, sizeof(label), _TRUNCATE, "   %s", RemoteSlots[i].saveID );
      ManualView->Print( startX, y, zSTRING( label ) );
      y += LineH;

      // Date line (always dim)
      zCOLOR dateClr( 150, 150, 150, 255 );
      ManualView->SetFontColor( dateClr );
      ManualView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      char dateLine[128];
      _snprintf_s( dateLine, sizeof(dateLine), _TRUNCATE, "   %s", RemoteSlots[i].dateStr );
      ManualView->Print( startX, y, zSTRING( dateLine ) );
      y += LineH;
    }
  }

  void ShowResult( const char* msg ) {
    strncpy_s( ResultMsg, msg, sizeof(ResultMsg) - 1 );
    ShowingResult = true;
    ResultTimer = 0.0f;
  }
}

// =========================================================================
// Public API
// =========================================================================

namespace ManualSyncUi {

  void Open() {
    if( Active ) return;
    Active         = true;
    SelColumn      = 0;
    LocalCursor    = 0;
    RemoteCursor   = 0;
    LocalScrollOff = 0;
    RemoteScrollOff= 0;
    ShowingResult  = false;
    ResultTimer    = 0.0f;
    LastFrameTime  = GetEngineTime();

    RefreshLocalSlots();
    RefreshRemoteSlots();
    CreateManualPanel();
    SyncLog::Write( "ManualSyncUi::Open" );
  }

  void Close() {
    if( !Active ) return;
    Active = false;
    DestroyManualPanel();
    SyncLog::Write( "ManualSyncUi::Close" );
  }

  bool IsActive() {
    return Active;
  }

  void Poll() {
    if( !Active || !ManualView ) return;

    // Delta time
    float now = GetEngineTime();
    float dt  = now - LastFrameTime;
    if( dt < 0.0f ) dt = 0.0f;
    if( dt > 0.5f ) dt = 0.5f;
    LastFrameTime = now;

    ManualView->ClrPrintwin();

    // ESC always closes.
    if( KeyToggled( VK_ESCAPE ) ) {
      Close();
      return;
    }

    // --- Result overlay (auto-dismiss after 4 seconds) ---
    if( ShowingResult ) {
      ResultTimer += dt;
      if( ResultTimer > 4.0f ) {
        ShowingResult = false;
        // Refresh lists after an action
        RefreshLocalSlots();
        RefreshRemoteSlots();
      } else {
        RenderHeader();
        zCOLOR gold( 255, 215, 0, 255 );
        ManualView->SetFontColor( gold );
        ManualView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
        ManualView->PrintCX( PanelH / 2, zSTRING( ResultMsg ) );
        if( zinput ) zinput->ClearKeyBuffer();
        return;
      }
    }

    // --- Normal rendering ---
    RenderHeader();

    int colY = MarginY + LineH * 3;
    RenderLocalColumn( MarginX, colY );
    RenderRemoteColumn( ColDivider + MarginX, colY );
    RenderFooter();

    // --- Input handling ---
    // Column switching
    if( KeyToggled( VK_LEFT ) || KeyToggled( 'A' ) )
      SelColumn = 0;
    if( KeyToggled( VK_RIGHT ) || KeyToggled( 'D' ) )
      SelColumn = 1;

    // Navigation
    if( SelColumn == 0 ) {
      if( KeyToggled( VK_UP ) || KeyToggled( 'W' ) ) {
        LocalCursor--;
        if( LocalCursor < 0 )
          LocalCursor = LocalSlotCount > 0 ? LocalSlotCount - 1 : 0;
        // Scroll
        if( LocalCursor < LocalScrollOff )
          LocalScrollOff = LocalCursor;
        if( LocalCursor >= LocalScrollOff + VisibleRows )
          LocalScrollOff = LocalCursor - VisibleRows + 1;
      }
      if( KeyToggled( VK_DOWN ) || KeyToggled( 'S' ) ) {
        LocalCursor++;
        if( LocalCursor >= LocalSlotCount )
          LocalCursor = 0;
        if( LocalCursor < LocalScrollOff )
          LocalScrollOff = 0;
        if( LocalCursor >= LocalScrollOff + VisibleRows )
          LocalScrollOff = LocalCursor - VisibleRows + 1;
      }

      // ENTER = Upload selected local slot
      if( KeyToggled( VK_RETURN ) && LocalSlotCount > 0 ) {
        int slotID = LocalSlots[LocalCursor].slotID;
        SyncLog::Write( "ManualSyncUi: uploading slot %d ('%s')", slotID, LocalSlots[LocalCursor].name );
        RemoteSync::UploadSlot( slotID );
        char msg[256];
        _snprintf_s( msg, sizeof(msg), _TRUNCATE,
          "Subiendo '%s' (slot %d) al servidor...", LocalSlots[LocalCursor].name, slotID );
        ShowResult( msg );
      }

    } else {
      if( KeyToggled( VK_UP ) || KeyToggled( 'W' ) ) {
        RemoteCursor--;
        if( RemoteCursor < 0 )
          RemoteCursor = RemoteSlotCount > 0 ? RemoteSlotCount - 1 : 0;
        if( RemoteCursor < RemoteScrollOff )
          RemoteScrollOff = RemoteCursor;
        if( RemoteCursor >= RemoteScrollOff + VisibleRows )
          RemoteScrollOff = RemoteCursor - VisibleRows + 1;
      }
      if( KeyToggled( VK_DOWN ) || KeyToggled( 'S' ) ) {
        RemoteCursor++;
        if( RemoteCursor >= RemoteSlotCount )
          RemoteCursor = 0;
        if( RemoteCursor < RemoteScrollOff )
          RemoteScrollOff = 0;
        if( RemoteCursor >= RemoteScrollOff + VisibleRows )
          RemoteScrollOff = RemoteCursor - VisibleRows + 1;
      }

      // ENTER = Download selected remote save
      if( KeyToggled( VK_RETURN ) && RemoteSlotCount > 0 ) {
        const char* saveID = RemoteSlots[RemoteCursor].saveID;
        SyncLog::Write( "ManualSyncUi: downloading remote '%s'", saveID );
        RemoteSync::DownloadSlotBySaveID( saveID );
        char msg[256];
        _snprintf_s( msg, sizeof(msg), _TRUNCATE,
          "Descargando '%s' del servidor...", saveID );
        ShowResult( msg );

        // Reinit savegame manager so the UI reflects changes.
        if( gameMan && gameMan->savegameManager ) {
          gameMan->savegameManager->Reinit();
          SyncLog::Write( "ManualSyncUi: Savegame manager reinitialized" );
        }
      }
    }

    // B = Restore backup for the selected local slot
    if( KeyToggled( 'B' ) && LocalSlotCount > 0 ) {
      int slotID = LocalSlots[LocalCursor].slotID;
      SyncLog::Write( "ManualSyncUi: restoring backup for slot %d", slotID );
      if( RemoteSync::RestoreBackupForSlot( slotID ) ) {
        char msg[256];
        _snprintf_s( msg, sizeof(msg), _TRUNCATE,
          "Backup de '%s' (slot %d) restaurado.", LocalSlots[LocalCursor].name, slotID );
        ShowResult( msg );

        if( gameMan && gameMan->savegameManager ) {
          gameMan->savegameManager->Reinit();
          SyncLog::Write( "ManualSyncUi: Savegame manager reinitialized after backup restore" );
        }
      } else {
        ShowResult( "No hay backup disponible para este slot." );
      }
    }

    // R = Refresh both lists
    if( KeyToggled( 'R' ) ) {
      RefreshLocalSlots();
      RefreshRemoteSlots();
      LocalCursor = 0;
      RemoteCursor = 0;
      LocalScrollOff = 0;
      RemoteScrollOff = 0;
      ShowResult( "Listas actualizadas." );
    }

    // Block game input while overlay is visible.
    if( zinput )
      zinput->ClearKeyBuffer();
  }
}
