// -----------------------------------------------------------------------
// SyncHistoryUi.cpp — Scrollable overlay for the history browser.
// -----------------------------------------------------------------------

#include "SyncHistoryUi.h"
#include "SyncHistory.h"
#include "SyncLog.h"
#include "plugin.h"
#include "UnionAfx.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {
  static const int VMax        = 8192;
  static const int PanelW      = 5000;
  static const int PanelH      = 6000;
  static const int PanelX      = (VMax - PanelW) / 2;
  static const int PanelY      = (VMax - PanelH) / 2;
  static const int MarginX     = 300;
  static const int MarginY     = 200;
  static const int LineH       = 250;
  static const int VisibleRows = 12;

  bool     Active        = false;
  zCView*  HistoryView   = 0;
  int      Cursor        = 0;
  int      ScrollOffset  = 0;
  float    LastFrameTime = 0.0f;
  float    ResultTimer   = 0.0f;
  bool     ShowingResult = false;
  char     ResultMsg[128]= {};

  SyncHistory::HistoryEntry EntriesBuf[SyncHistory::MaxTotalEntries];
  int EntryCount = 0;

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

  void CreateHistoryPanel() {
    if( HistoryView && screen ) {
      screen->RemoveItem( HistoryView );
      delete HistoryView;
    }
    HistoryView = new zCView( PanelX, PanelY, PanelX + PanelW, PanelY + PanelH );
    HistoryView->InsertBack( "DLG_CONVERSATION.TGA" );
    HistoryView->SetAlphaBlendFunc( zRND_ALPHA_FUNC_BLEND );
    HistoryView->SetTransparency( 30 );
    HistoryView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    if( screen ) screen->InsertItem( HistoryView );
  }

  void DestroyHistoryPanel() {
    if( HistoryView && screen ) {
      screen->RemoveItem( HistoryView );
      delete HistoryView;
      HistoryView = 0;
    }
  }
}

namespace SyncHistoryUi {

  void Open() {
    if( Active ) return;
    Active = true;
    Cursor = 0;
    ScrollOffset = 0;
    ShowingResult = false;
    ResultTimer = 0.0f;
    EntryCount = SyncHistory::GetEntries( EntriesBuf, SyncHistory::MaxTotalEntries );
    LastFrameTime = GetEngineTime();
    CreateHistoryPanel();
    SyncLog::Write( "SyncHistoryUi::Open (loaded %d entries)", EntryCount );
  }

  void Close() {
    if( !Active ) return;
    Active = false;
    DestroyHistoryPanel();
    SyncLog::Write( "SyncHistoryUi::Close" );
  }

  bool IsActive() {
    return Active;
  }

  void Poll() {
    if( !Active || !HistoryView ) return;

    float now = GetEngineTime();
    float dt  = now - LastFrameTime;
    if( dt < 0.0f ) dt = 0.0f;
    if( dt > 0.5f ) dt = 0.5f;
    LastFrameTime = now;

    HistoryView->ClrPrintwin();

    if( KeyToggled( VK_ESCAPE ) ) {
      Close();
      return;
    }

    if( ShowingResult ) {
      ResultTimer += dt;
      if( ResultTimer > 3.0f ) {
        Close();
        return;
      }
      zCOLOR gold( 255, 215, 0, 255 );
      HistoryView->SetFontColor( gold );
      HistoryView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
      HistoryView->PrintCX( PanelH / 2, zSTRING( ResultMsg ) );
      return;
    }

    zCOLOR titleColor( 255, 215, 0, 255 );
    HistoryView->SetFontColor( titleColor );
    HistoryView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    HistoryView->PrintCX( MarginY, zSTRING( "Historial de Sincronizacion" ) );

    zCOLOR divColor( 100, 100, 100, 255 );
    HistoryView->SetFontColor( divColor );
    HistoryView->SetFont( "FONT_OLD_10_WHITE.TGA" );
    HistoryView->PrintCX( MarginY + LineH, zSTRING( "----------------------------------------" ) );

    int startY = MarginY + LineH * 3;
    int y = startY;

    if( EntryCount == 0 ) {
      zCOLOR dim( 120, 120, 120, 255 );
      HistoryView->SetFontColor( dim );
      HistoryView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      HistoryView->PrintCX( y, zSTRING( "(No hay historial disponible)" ) );
    } else {
      for( int i = ScrollOffset; i < EntryCount && i < ScrollOffset + VisibleRows; ++i ) {
        bool cur = (i == Cursor);

        if( cur ) {
          zCOLOR sel( 255, 215, 0, 255 );
          HistoryView->SetFontColor( sel );
          HistoryView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
        } else {
          zCOLOR norm( 200, 200, 200, 255 );
          HistoryView->SetFontColor( norm );
          HistoryView->SetFont( "FONT_OLD_10_WHITE.TGA" );
        }

        const SyncHistory::HistoryEntry& e = EntriesBuf[i];
        char label[128];
        if( cur )
          _snprintf_s( label, sizeof(label), _TRUNCATE, ">> %s [%s]", e.slotName, e.dateStr );
        else
          _snprintf_s( label, sizeof(label), _TRUNCATE, "   %s [%s]", e.slotName, e.dateStr );
        
        HistoryView->Print( MarginX, y, zSTRING( label ) );
        y += (LineH - 40);

        zCOLOR dim( 150, 150, 150, 255 );
        HistoryView->SetFontColor( dim );
        HistoryView->SetFont( "FONT_OLD_10_WHITE.TGA" );
        char detail[128];
        _snprintf_s( detail, sizeof(detail), _TRUNCATE, "     (origen: %s%s%s)",
          e.source, e.peerName[0] ? " - " : "", e.peerName );
        HistoryView->Print( MarginX, y, zSTRING( detail ) );
        y += (LineH + 40);
      }
    }

    zCOLOR gray( 160, 160, 160, 255 );
    HistoryView->SetFontColor( gray );
    HistoryView->SetFont( "FONT_OLD_10_WHITE.TGA" );
    int sX, sY; HistoryView->GetSize( sX, sY );
    HistoryView->PrintCX( sY - MarginY, zSTRING( "[Arriba/Abajo] Navegar  [ENTER] Restaurar  [ESC] Cerrar" ) );

    if( EntryCount > 0 ) {
      if( KeyToggled( VK_UP ) || KeyToggled( 'W' ) ) {
        Cursor--;
        if( Cursor < 0 ) {
          Cursor = EntryCount - 1;
          ScrollOffset = Cursor - VisibleRows + 1;
          if( ScrollOffset < 0 ) ScrollOffset = 0;
        } else if( Cursor < ScrollOffset ) {
          ScrollOffset = Cursor;
        }
      }
      if( KeyToggled( VK_DOWN ) || KeyToggled( 'S' ) ) {
        Cursor++;
        if( Cursor >= EntryCount ) {
          Cursor = 0;
          ScrollOffset = 0;
        } else if( Cursor >= ScrollOffset + VisibleRows ) {
          ScrollOffset = Cursor - VisibleRows + 1;
        }
      }
      if( KeyToggled( VK_RETURN ) ) {
        if( SyncHistory::RestoreEntry( EntriesBuf[Cursor] ) ) {
          _snprintf_s( ResultMsg, sizeof(ResultMsg), _TRUNCATE, "Partida '%s' restaurada.", EntriesBuf[Cursor].slotName );
          
          if( gameMan && gameMan->savegameManager ) {
            gameMan->savegameManager->Reinit();
            SyncLog::Write( "SyncHistoryUi: Savegame manager reinitialized" );
          }
        } else {
          _snprintf_s( ResultMsg, sizeof(ResultMsg), _TRUNCATE, "Error al restaurar." );
        }
        ShowingResult = true;
        ResultTimer = 0.0f;
      }
    }

    if( zinput ) zinput->ClearKeyBuffer();
  }
}
