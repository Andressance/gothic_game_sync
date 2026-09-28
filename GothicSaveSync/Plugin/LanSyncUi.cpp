// -----------------------------------------------------------------------
// LanSyncUi.cpp — Two-column in-game overlay for LAN synchronisation.
//
// Layout (virtual coords 0-8192):
//   +-------------------------------------------+
//   |       Sincronizacion LAN                  |
//   |     Conectado con 'PC-Remote'             |
//   |                                           |
//   |  TUS PARTIDAS       | PARTIDAS REMOTAS    |
//   |  ------------------   ------------------  |
//   |  >> savegame0       |    savegame0        |
//   |     28/09/2026 18:00|    28/09/2026 17:30 |
//   |     savegame1       |    savegame2        |
//   |     28/09/2026 16:00|    28/09/2026 15:00 |
//   |                                           |
//   | [Izq/Der] [Arriba/Abajo] [ENTER] [ESC]   |
//   +-------------------------------------------+
// -----------------------------------------------------------------------

#include "LanSyncUi.h"
#include "LocalSync.h"
#include "SyncLog.h"
#include "plugin.h"
#include "UnionAfx.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {
  // ---- layout constants (virtual 8192 coords) --------------------------
  static const int VMax        = 8192;
  static const int PanelW      = 6800;
  static const int PanelH      = 5800;
  static const int PanelX      = (VMax - PanelW) / 2;
  static const int PanelY      = (VMax - PanelH) / 2;

  static const int MarginX     = 200;
  static const int MarginY     = 150;
  static const int LineH       = 280;
  static const int ColDivider  = PanelW / 2;

  // ---- state ------------------------------------------------------------
  bool     Active        = false;
  zCView*  LanView       = 0;
  int      SelColumn     = 0;   // 0 = local, 1 = remote
  int      LocalCursor   = 0;
  int      RemoteCursor  = 0;
  float    ResultTimer   = 0.0f;
  bool     ShowingResult = false;
  float    LastFrameTime = 0.0f;

  // ---- input helper (same pattern as GameUi) ----------------------------
  bool KeyToggled( int vk ) {
    static bool pressed[256] = {};
    if( vk < 0 || vk > 255 ) return false;
    bool down = (GetAsyncKeyState( vk ) & 0x8000) != 0;
    if( down && !pressed[vk] ) { pressed[vk] = true; return true; }
    if( !down ) pressed[vk] = false;
    return false;
  }

  float GetEngineTime() {
    if( ztimer )
      return ztimer->totalTimeFloat / 1000.0f;
    return 0.0f;
  }

  // ---- view management --------------------------------------------------
  void CreateLanPanel() {
    if( LanView && screen ) {
      screen->RemoveItem( LanView );
      delete LanView;
    }
    LanView = new zCView( PanelX, PanelY, PanelX + PanelW, PanelY + PanelH );
    LanView->InsertBack( "DLG_CONVERSATION.TGA" );
    LanView->SetAlphaBlendFunc( zRND_ALPHA_FUNC_BLEND );
    LanView->SetTransparency( 30 );
    LanView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    if( screen )
      screen->InsertItem( LanView );
  }

  void DestroyLanPanel() {
    if( LanView && screen ) {
      screen->RemoveItem( LanView );
      delete LanView;
      LanView = 0;
    }
  }

  // ---- rendering helpers ------------------------------------------------

  void RenderHeader( const char* title, const char* subtitle ) {
    if( !LanView ) return;

    zCOLOR gold( 255, 215, 0, 255 );
    LanView->SetFontColor( gold );
    LanView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    LanView->PrintCX( MarginY, zSTRING( title ) );

    zCOLOR silver( 200, 200, 200, 255 );
    LanView->SetFontColor( silver );
    LanView->SetFont( "FONT_OLD_10_WHITE.TGA" );
    LanView->PrintCX( MarginY + LineH, zSTRING( subtitle ) );
  }

  void RenderFooter( const char* hint ) {
    if( !LanView ) return;

    zCOLOR gray( 160, 160, 160, 255 );
    LanView->SetFontColor( gray );
    LanView->SetFont( "FONT_OLD_10_WHITE.TGA" );

    int sX, sY;
    LanView->GetSize( sX, sY );
    int footerY = sY - MarginY - 100;
    LanView->PrintCX( footerY, zSTRING( hint ) );
  }

  void RenderSlotColumn( int startX, int startY, const char* header,
    const SaveSync::SaveSlotInfo* slots, int count, int cursor, bool isSel ) {
    if( !LanView ) return;

    // Column header
    zCOLOR hdrColor( 255, 215, 0, 255 );
    LanView->SetFontColor( hdrColor );
    LanView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    LanView->Print( startX, startY, zSTRING( header ) );

    // Divider
    zCOLOR divColor( 100, 100, 100, 255 );
    LanView->SetFontColor( divColor );
    LanView->SetFont( "FONT_OLD_10_WHITE.TGA" );
    LanView->Print( startX, startY + LineH, zSTRING( "--------------------" ) );

    int y = startY + LineH * 2;

    if( count == 0 ) {
      zCOLOR dim( 120, 120, 120, 255 );
      LanView->SetFontColor( dim );
      LanView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      LanView->Print( startX, y, zSTRING( "(sin partidas)" ) );
      return;
    }

    for( int i = 0; i < count; ++i ) {
      bool cur = isSel && (i == cursor);

      if( cur ) {
        zCOLOR sel( 255, 215, 0, 255 );
        LanView->SetFontColor( sel );
        LanView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
      } else {
        zCOLOR norm( 200, 200, 200, 255 );
        LanView->SetFontColor( norm );
        LanView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      }

      char label[128];
      if( cur )
        _snprintf_s( label, sizeof(label), _TRUNCATE, ">> %s", slots[i].name );
      else
        _snprintf_s( label, sizeof(label), _TRUNCATE, "   %s", slots[i].name );
      LanView->Print( startX, y, zSTRING( label ) );
      y += LineH;

      // Date line (always dim)
      zCOLOR dateClr( 150, 150, 150, 255 );
      LanView->SetFontColor( dateClr );
      LanView->SetFont( "FONT_OLD_10_WHITE.TGA" );

      char dateLine[128];
      _snprintf_s( dateLine, sizeof(dateLine), _TRUNCATE, "   %s", slots[i].dateStr );
      LanView->Print( startX, y, zSTRING( dateLine ) );
      y += LineH;
    }
  }
}

// =========================================================================
// Public API
// =========================================================================

namespace LanSyncUi {

  void Open() {
    if( Active ) return;
    Active        = true;
    SelColumn     = 0;
    LocalCursor   = 0;
    RemoteCursor  = 0;
    ShowingResult = false;
    ResultTimer   = 0.0f;
    LastFrameTime = GetEngineTime();
    CreateLanPanel();
    SyncLog::Write( "LanSyncUi::Open" );
  }

  void Close() {
    if( !Active ) return;
    Active = false;
    DestroyLanPanel();
    LocalSync::Disconnect();
    SyncLog::Write( "LanSyncUi::Close" );
  }

  bool IsActive() {
    return Active;
  }

  void Poll() {
    if( !Active || !LanView ) return;

    // Delta time for result timer.
    float now = GetEngineTime();
    float dt  = now - LastFrameTime;
    if( dt < 0.0f ) dt = 0.0f;
    if( dt > 0.5f ) dt = 0.5f;
    LastFrameTime = now;

    LanView->ClrPrintwin();

    LocalSync::State state = LocalSync::GetState();
    const char* status     = LocalSync::GetStatusMessage();

    // ESC always closes.
    if( KeyToggled( VK_ESCAPE ) ) {
      Close();
      return;
    }

    switch( state ) {

      // ----- scanning for peers ------------------------------------------
      case LocalSync::STATE_SCANNING: {
        RenderHeader( "Sincronizacion LAN", "Buscando dispositivos en la red..." );
        RenderFooter( "[ESC = Cancelar]" );
        break;
      }

      // ----- error -------------------------------------------------------
      case LocalSync::STATE_ERROR: {
        RenderHeader( "Sincronizacion LAN", status );
        RenderFooter( "[ESC = Cerrar]" );
        break;
      }

      // ----- transferring ------------------------------------------------
      case LocalSync::STATE_TRANSFERRING: {
        RenderHeader( "Sincronizacion LAN", status );
        RenderFooter( "Espera por favor..." );
        break;
      }

      // ----- connected — two-column browser ------------------------------
      case LocalSync::STATE_CONNECTED: {
        // Transfer result auto-dismiss
        if( LocalSync::IsTransferComplete() ) {
          if( !ShowingResult ) {
            ShowingResult = true;
            ResultTimer   = 0.0f;
          }
          ResultTimer += dt;
          if( ResultTimer > 4.0f ) {
            ShowingResult = false;
            LocalSync::AcknowledgeTransfer();
          }
        }

        // Title
        char titleBuf[128];
        _snprintf_s( titleBuf, sizeof(titleBuf), _TRUNCATE,
          "Sincronizacion LAN - %s", LocalSync::GetPeerName() );
        const char* sub = ShowingResult
          ? status
          : "Selecciona una partida para enviar o recibir";
        RenderHeader( titleBuf, sub );

        // Slot data
        int localCount  = LocalSync::GetLocalSlotCount();
        int remoteCount = LocalSync::GetRemoteSlotCount();
        const SaveSync::SaveSlotInfo* localSlots  = LocalSync::GetLocalSlots();
        const SaveSync::SaveSlotInfo* remoteSlots = LocalSync::GetRemoteSlots();

        // Clamp cursors
        if( LocalCursor  >= localCount  ) LocalCursor  = localCount  > 0 ? localCount  - 1 : 0;
        if( RemoteCursor >= remoteCount ) RemoteCursor = remoteCount > 0 ? remoteCount - 1 : 0;

        int colY = MarginY + LineH * 3;

        // Left column: local saves
        RenderSlotColumn( MarginX, colY, "Tus partidas",
          localSlots, localCount, LocalCursor, SelColumn == 0 );

        // Right column: remote saves
        RenderSlotColumn( ColDivider + MarginX, colY, "Partidas remotas",
          remoteSlots, remoteCount, RemoteCursor, SelColumn == 1 );

        // Footer
        if( SelColumn == 0 )
          RenderFooter( "[Izq/Der: Columna]  [Arriba/Abajo: Navegar]  [ENTER: Enviar]  [ESC: Cerrar]" );
        else
          RenderFooter( "[Izq/Der: Columna]  [Arriba/Abajo: Navegar]  [ENTER: Recibir]  [ESC: Cerrar]" );

        // Input (disabled while showing transfer result)
        if( !ShowingResult ) {
          if( KeyToggled( VK_LEFT ) || KeyToggled( 'A' ) )
            SelColumn = 0;
          if( KeyToggled( VK_RIGHT ) || KeyToggled( 'D' ) )
            SelColumn = 1;

          if( SelColumn == 0 ) {
            if( KeyToggled( VK_UP ) || KeyToggled( 'W' ) ) {
              LocalCursor--;
              if( LocalCursor < 0 )
                LocalCursor = localCount > 0 ? localCount - 1 : 0;
            }
            if( KeyToggled( VK_DOWN ) || KeyToggled( 'S' ) ) {
              LocalCursor++;
              if( LocalCursor >= localCount )
                LocalCursor = 0;
            }
            if( KeyToggled( VK_RETURN ) && localCount > 0 )
              LocalSync::RequestSendSave( LocalCursor );

          } else {
            if( KeyToggled( VK_UP ) || KeyToggled( 'W' ) ) {
              RemoteCursor--;
              if( RemoteCursor < 0 )
                RemoteCursor = remoteCount > 0 ? remoteCount - 1 : 0;
            }
            if( KeyToggled( VK_DOWN ) || KeyToggled( 'S' ) ) {
              RemoteCursor++;
              if( RemoteCursor >= remoteCount )
                RemoteCursor = 0;
            }
            if( KeyToggled( VK_RETURN ) && remoteCount > 0 )
              LocalSync::RequestReceiveSave( RemoteCursor );
          }
        }
        break;
      }

      // ----- idle (shouldn't normally reach here while UI is open) -------
      default: {
        Close();
        return;
      }
    }

    // Block game input while the overlay is visible.
    if( zinput )
      zinput->ClearKeyBuffer();
  }
}
