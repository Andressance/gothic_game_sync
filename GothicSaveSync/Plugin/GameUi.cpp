#include "GameUi.h"
#include "plugin.h"
#include "UnionAfx.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// In-game overlay UI rendered with the Gothic engine's own zCView system.
// All panels use the game's native fonts and are drawn as children of
// the global screen view, so they look like part of the game rather than
// Windows dialogs.
// ---------------------------------------------------------------------------

namespace {
  // ----- constants --------------------------------------------------------
  static const int   VirtualMax     = 8192;  // Gothic virtual coordinate max
  static const float InfoTimeout    = 6.0f;  // seconds before auto-dismiss
  static const float WarningTimeout = 10.0f;

  // Panel dimensions in virtual coords (8192 = full screen).
  static const int PanelWidth  = 5200;
  static const int PanelHeight = 2200;
  static const int PanelX      = (VirtualMax - PanelWidth) / 2;
  static const int PanelY      = (VirtualMax - PanelHeight) / 2;

  // Menu panel is taller to accommodate more items.
  static const int MenuPanelHeight = 3600;
  static const int MenuPanelY      = (VirtualMax - MenuPanelHeight) / 2;

  // Text margins inside the panel.
  static const int TextMarginX = 300;
  static const int TextMarginY = 200;
  static const int LineSpacing = 300;

  // ----- state ------------------------------------------------------------
  GameUi::OverlayState  CurrentState   = GameUi::OVERLAY_NONE;
  GameUi::QuestionResult QResult       = GameUi::QUESTION_PENDING;
  int                   MenuChoice     = -1;
  int                   MenuCursor     = 0;

  zCView*  OverlayView   = 0;

  char  PanelTitle[256]   = {};
  char  PanelMessage[512] = {};
  float DismissTimer      = 0.0f;
  float LastFrameTime     = 0.0f;

  // menu item labels (set at menu-open time)
  const char* MenuLabels[GameUi::MENUITEM_COUNT] = {};

  // ----- helpers ----------------------------------------------------------
  float GetEngineTime() {
    // ztimer is the global engine timer (milliseconds since game start).
    if( ztimer )
      return ztimer->totalTimeFloat / 1000.0f;
    return 0.0f;
  }

  void DestroyOverlay() {
    if( OverlayView && screen ) {
      screen->RemoveItem( OverlayView );
      delete OverlayView;
      OverlayView = 0;
    }
    CurrentState = GameUi::OVERLAY_NONE;
  }

  // Create the dark semi-transparent backdrop panel.
  void CreatePanel( int panelX, int panelY, int panelW, int panelH ) {
    DestroyOverlay();
    OverlayView = new zCView( panelX, panelY, panelX + panelW, panelY + panelH );
    OverlayView->InsertBack( "DLG_CONVERSATION.TGA" );
    OverlayView->SetAlphaBlendFunc( zRND_ALPHA_FUNC_BLEND );
    OverlayView->SetTransparency( 40 );
    OverlayView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    if( screen )
      screen->InsertItem( OverlayView );
  }

  // Print the title in highlight colour and the body text below it.
  void RenderTitleAndBody() {
    if( !OverlayView )
      return;

    OverlayView->ClrPrintwin();

    // Title (centred, highlighted colour)
    zCOLOR titleColor( 255, 215, 0, 255 );
    OverlayView->SetFontColor( titleColor );
    OverlayView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
    OverlayView->PrintCX( TextMarginY, zSTRING( PanelTitle ) );

    // Body (white)
    zCOLOR bodyColor( 220, 220, 220, 255 );
    OverlayView->SetFontColor( bodyColor );
    OverlayView->SetFont( "FONT_OLD_10_WHITE.TGA" );

    // Word-wrap: split message by newlines, print each line.
    int y = TextMarginY + LineSpacing + 100;
    const char* cursor = PanelMessage;
    while( *cursor ) {
      const char* lineEnd = strchr( cursor, '\n' );
      char lineBuf[256] = {};
      if( lineEnd ) {
        size_t len = (size_t)(lineEnd - cursor);
        if( len >= sizeof(lineBuf) ) len = sizeof(lineBuf) - 1;
        memcpy( lineBuf, cursor, len );
        lineBuf[len] = 0;
        cursor = lineEnd + 1;
      } else {
        strncpy_s( lineBuf, cursor, sizeof(lineBuf) - 1 );
        cursor += strlen( cursor );
      }
      OverlayView->PrintCX( y, zSTRING( lineBuf ) );
      y += LineSpacing;
    }
  }

  // Draw the footer hint at the bottom of the panel.
  void RenderFooter( const char* hint ) {
    if( !OverlayView )
      return;

    zCOLOR hintColor( 160, 160, 160, 255 );
    OverlayView->SetFontColor( hintColor );
    OverlayView->SetFont( "FONT_OLD_10_WHITE.TGA" );

    // Position at the bottom of the panel.
    int panelSizeX, panelSizeY;
    OverlayView->GetSize( panelSizeX, panelSizeY );
    int footerY = panelSizeY - TextMarginY - 100;
    if( footerY < 0 ) footerY = panelSizeY - 200;
    OverlayView->PrintCX( footerY, zSTRING( hint ) );
  }

  // Render menu items with cursor highlight.
  void RenderMenuItems() {
    if( !OverlayView )
      return;

    int baseY = TextMarginY + LineSpacing + 100;
    // Skip past any body text lines.
    baseY += LineSpacing * 2;

    for( int i = 0; i < GameUi::MENUITEM_COUNT; ++i ) {
      if( !MenuLabels[i] )
        continue;

      bool selected = (i == MenuCursor);

      if( selected ) {
        zCOLOR selColor( 255, 215, 0, 255 );
        OverlayView->SetFontColor( selColor );
        OverlayView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
      } else {
        zCOLOR normColor( 200, 200, 200, 255 );
        OverlayView->SetFontColor( normColor );
        OverlayView->SetFont( "FONT_OLD_10_WHITE.TGA" );
      }

      char label[128];
      if( selected )
        _snprintf_s( label, sizeof(label), _TRUNCATE, ">> %s <<", MenuLabels[i] );
      else
        _snprintf_s( label, sizeof(label), _TRUNCATE, "   %s", MenuLabels[i] );

      OverlayView->PrintCX( baseY + i * LineSpacing, zSTRING( label ) );
    }
  }

  // Check for a key-toggled event using robust Windows async input to bypass engine quirks.
  bool KeyToggled( int virtualKey ) {
    static bool keyState[256] = { false };
    if ( virtualKey < 0 || virtualKey > 255 ) return false;
    
    bool isDown = (GetAsyncKeyState( virtualKey ) & 0x8000) != 0;
    if ( isDown && !keyState[virtualKey] ) {
      keyState[virtualKey] = true;
      return true;
    }
    if ( !isDown ) {
      keyState[virtualKey] = false;
    }
    return false;
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace GameUi {

  void Init() {
    CurrentState = OVERLAY_NONE;
    OverlayView  = 0;
    QResult      = QUESTION_PENDING;
    MenuChoice   = -1;
    MenuCursor   = 0;
    LastFrameTime = GetEngineTime();
  }

  void Shutdown() {
    DestroyOverlay();
  }

  // --- Info ----------------------------------------------------------------
  void ShowInfo( const char* title, const char* message ) {
    strncpy_s( PanelTitle, title, sizeof(PanelTitle) - 1 );
    strncpy_s( PanelMessage, message, sizeof(PanelMessage) - 1 );
    CreatePanel( PanelX, PanelY, PanelWidth, PanelHeight );
    CurrentState = OVERLAY_INFO;
    DismissTimer = 0.0f;
  }

  // --- Warning -------------------------------------------------------------
  void ShowWarning( const char* title, const char* message ) {
    strncpy_s( PanelTitle, title, sizeof(PanelTitle) - 1 );
    strncpy_s( PanelMessage, message, sizeof(PanelMessage) - 1 );
    CreatePanel( PanelX, PanelY, PanelWidth, PanelHeight );
    CurrentState = OVERLAY_WARNING;
    DismissTimer = 0.0f;
  }

  // --- Question (yes / no) -------------------------------------------------
  void ShowQuestion( const char* title, const char* message ) {
    strncpy_s( PanelTitle, title, sizeof(PanelTitle) - 1 );
    strncpy_s( PanelMessage, message, sizeof(PanelMessage) - 1 );
    CreatePanel( PanelX, PanelY, PanelWidth, PanelHeight );
    CurrentState = OVERLAY_QUESTION;
    QResult = QUESTION_PENDING;
  }

  QuestionResult GetQuestionResult() {
    return QResult;
  }

  // --- Developer menu ------------------------------------------------------
  void ShowDeveloperMenu() {
    MenuLabels[MENUITEM_DOWNLOAD_REMOTE] = "Descargar partidas remotas";
    MenuLabels[MENUITEM_UPLOAD_SAVE]     = "Enviar ultimo guardado al servidor";
    MenuLabels[MENUITEM_RESTORE_BACKUP]  = "Restaurar backup del slot actual";
    MenuLabels[MENUITEM_MANUAL_SYNC]     = "Gestion manual de partidas";
    MenuLabels[MENUITEM_LAN_SYNC]        = "Sincronizacion LAN";
    MenuLabels[MENUITEM_HISTORY]         = "Historial de Sincronizacion";
    MenuLabels[MENUITEM_CLOSE]           = "Cerrar";

    strncpy_s( PanelTitle, "GothicSaveSync", sizeof(PanelTitle) - 1 );
    strncpy_s( PanelMessage, "Panel de desarrollador", sizeof(PanelMessage) - 1 );
    CreatePanel( PanelX, MenuPanelY, PanelWidth, MenuPanelHeight );
    CurrentState = OVERLAY_MENU;
    MenuChoice   = -1;
    MenuCursor   = 0;
  }

  int GetMenuResult() {
    return MenuChoice;
  }

  bool IsActive() {
    return CurrentState != OVERLAY_NONE;
  }

  // --- Per-frame update ----------------------------------------------------
  void Poll() {
    if( CurrentState == OVERLAY_NONE )
      return;

    // Delta time.
    float now = GetEngineTime();
    float dt  = now - LastFrameTime;
    if( dt < 0.0f ) dt = 0.0f;
    if( dt > 0.5f ) dt = 0.5f;  // clamp huge gaps
    LastFrameTime = now;

    // -- Render the panel contents every frame --
    if( OverlayView ) {
      OverlayView->ClrPrintwin();
      RenderTitleAndBody();
    }

    switch( CurrentState ) {
      // ------------------------------------------------------------------
      case OVERLAY_INFO: {
        RenderFooter( "[Pulsa cualquier tecla]" );
        DismissTimer += dt;
        if( DismissTimer >= InfoTimeout || KeyToggled( VK_RETURN ) ||
            KeyToggled( VK_ESCAPE ) || KeyToggled( VK_SPACE ) ) {
          DestroyOverlay();
        }
        break;
      }

      // ------------------------------------------------------------------
      case OVERLAY_WARNING: {
        RenderFooter( "[Pulsa ENTER o ESC]" );
        DismissTimer += dt;
        if( DismissTimer >= WarningTimeout || KeyToggled( VK_RETURN ) ||
            KeyToggled( VK_ESCAPE ) ) {
          DestroyOverlay();
        }
        break;
      }

      // ------------------------------------------------------------------
      case OVERLAY_QUESTION: {
        // Draw yes/no hint.
        if( OverlayView ) {
          int panelSX, panelSY;
          OverlayView->GetSize( panelSX, panelSY );
          int footerY = panelSY - TextMarginY - 100;
          if( footerY < 0 ) footerY = panelSY - 200;

          zCOLOR hintColor( 255, 215, 0, 255 );
          OverlayView->SetFontColor( hintColor );
          OverlayView->SetFont( "FONT_OLD_10_WHITE_HI.TGA" );
          OverlayView->PrintCX( footerY, zSTRING( "[ENTER = Si]   [ESC = No]" ) );
        }

        if( KeyToggled( VK_RETURN ) || KeyToggled( 'Y' ) ) {
          QResult = QUESTION_YES;
          DestroyOverlay();
        } else if( KeyToggled( VK_ESCAPE ) || KeyToggled( 'N' ) ) {
          QResult = QUESTION_NO;
          DestroyOverlay();
        }
        break;
      }

      // ------------------------------------------------------------------
      case OVERLAY_MENU: {
        RenderMenuItems();
        RenderFooter( "[Arriba/Abajo = Navegar]  [ENTER = Elegir]  [ESC = Cerrar]" );

        if( KeyToggled( VK_UP ) || KeyToggled( 'W' ) ) {
          MenuCursor--;
          if( MenuCursor < 0 )
            MenuCursor = MENUITEM_COUNT - 1;
        }
        if( KeyToggled( VK_DOWN ) || KeyToggled( 'S' ) ) {
          MenuCursor++;
          if( MenuCursor >= MENUITEM_COUNT )
            MenuCursor = 0;
        }

        if( KeyToggled( VK_RETURN ) || KeyToggled( VK_SPACE ) ) {
          MenuChoice = MenuCursor;
          DestroyOverlay();
        }
        if( KeyToggled( VK_ESCAPE ) ) {
          MenuChoice = MENUITEM_CLOSE;
          DestroyOverlay();
        }
        break;
      }

      default:
        break;
    }
    
    // Block the game from processing movement/actions while overlay is active.
    if( zinput )
      zinput->ClearKeyBuffer();
  }
}
