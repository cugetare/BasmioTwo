/**
 * @file ui_screens.h
 * @brief sPod v2 - screen widget factory and refresh API.
 *
 * Each screen is created once at startup and kept alive for the entire
 * application lifetime.  Navigation is achieved by calling lv_scr_load_anim()
 * from the app layer; widgets are never destroyed and re-created.
 *
 * Refresh functions receive the current app state by const pointer and update
 * only the widget properties that can change at runtime (text, color, opacity,
 * slider value).  They are safe and cheap to call from any event callback.
 *
 * --------------------------------------------------------------------------
 * Required lv_conf.h settings:
 *   LV_FONT_MONTSERRAT_12  1
 *   LV_FONT_MONTSERRAT_14  1
 *   LV_FONT_MONTSERRAT_16  1
 *   LV_FONT_MONTSERRAT_18  1
 *   LV_FONT_MONTSERRAT_20  1
 *   LV_FONT_MONTSERRAT_22  1
 *   LV_FONT_MONTSERRAT_48  1
 *   LV_USE_LIST            1
 *   LV_USE_SLIDER          1
 *   LV_USE_BTN             1
 *   LV_USE_LABEL           1
 *   LV_USE_FLEX            1
 * --------------------------------------------------------------------------
 */

#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include "lvgl.h"
#include "ui_models.h"

/* ------------------------------------------------------------------ */
/*  Typography (scaled for 480x320 landscape)                                   */
/* ------------------------------------------------------------------ */

#define FONT_TITLE      &lv_font_montserrat_22
#define FONT_SUBTITLE   &lv_font_montserrat_20
#define FONT_STATUS_ICON &lv_font_montserrat_24
#define FONT_BODY       &lv_font_montserrat_18
#define FONT_SMALL      &lv_font_montserrat_16
#define FONT_TINY       &lv_font_montserrat_16
#define FONT_HERO       &lv_font_montserrat_48

/* ------------------------------------------------------------------ */
/*  Palette ??? warm, modern, premium                                     */
/* ------------------------------------------------------------------ */

#define COL_BG_DARK         0xFF1A1612u  /* near-black warm              */
#define COL_BG_CARD         0xFF2A2420u  /* card surface                 */
#define COL_BG_SHEET        0xFF242018u  /* bottom sheet surface         */
#define COL_ACCENT          0xFFE8A44Au  /* warm amber                   */
#define COL_ACCENT_DIM      0xFF9B6C2Au  /* dimmed amber for selection   */
#define COL_TEXT_PRIMARY    0xFFF5EFE6u  /* warm white                   */
#define COL_TEXT_SECONDARY  0xFF9E9187u  /* muted warm grey              */
#define COL_TEXT_HINT       0xFF5A534Du  /* very muted                   */
#define COL_BTN_PLAY        0xFFE8A44Au  /* amber fill for play button   */
#define COL_BTN_SECONDARY   0xFF3A342Eu  /* secondary button surface     */

/* ------------------------------------------------------------------ */
/*  Geometry                                                            */
/* ------------------------------------------------------------------ */

#define DISPLAY_W       480
#define DISPLAY_H       320
#define TOP_BAR_H       62

#define CARD_RADIUS     10
#define BTN_RADIUS      10
#define SHEET_RADIUS    14

#define CARD_COL_GAP    0
#define CARD_ROW_GAP    8
#define CARD_SIDE_PAD   12

/* List row geometry */
#define CARD_W  (DISPLAY_W - CARD_SIDE_PAD * 2)
#define CARD_H  64

/* ------------------------------------------------------------------ */
/*  Screen object handles                                               */
/*  Set once during creation; treat as read-only everywhere else.      */
/* ------------------------------------------------------------------ */

extern lv_obj_t *g_scr_library;
extern lv_obj_t *g_scr_detail;
extern lv_obj_t *g_scr_now_playing;
extern lv_obj_t *g_scr_settings;
extern lv_obj_t *g_scr_system_settings;
extern lv_obj_t *g_scr_display_settings;
extern lv_obj_t *g_scr_audio_settings;
extern lv_obj_t *g_scr_wifi_settings;

/* ------------------------------------------------------------------ */
/*  Library screen                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Create the Library screen and populate the 2-column story grid.
 *
 * Story cards are built from g_mock_stories[].  Each card registers a
 * tap callback that calls ui_app_select_story() then ui_app_navigate_to().
 *
 * @param state  Read-only at creation time; not stored.
 */
void ui_screen_library_create(const spod_app_state_t *state);

/* ------------------------------------------------------------------ */
/*  Story Detail screen                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Create the Story Detail screen (initially not displayed).
 * @param state  Read-only at creation time; not stored.
 */
void ui_screen_detail_create(const spod_app_state_t *state);

/**
 * @brief Refresh all mutable widgets on the Story Detail screen.
 *
 * Updates: accent bar colour, artwork initial letter, title, author,
 * duration, chapter count, and Play-vs-Continue button label.
 *
 * Must be called before navigating to this screen whenever
 * state->selected_story may have changed.
 *
 * @param state  Current app state (const; this function never mutates it).
 */
void ui_screen_detail_refresh(const spod_app_state_t *state);

/* ------------------------------------------------------------------ */
/*  Now Playing screen                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Create the Now Playing screen, including the Chapters sheet.
 *
 * The Chapters bottom-sheet is a persistent child of this screen;
 * it is shown/hidden via ui_chapters_sheet_set_visible().
 *
 * @param state  Read-only at creation time; not stored.
 */
void ui_screen_now_playing_create(const spod_app_state_t *state);

/**
 * @brief Refresh all mutable widgets on the Now Playing screen.
 *
 * Updates: story title, chapter label, play/pause icon, volume slider
 * and value label, sleep timer label, and chapters sheet visibility.
 *
 * @param state  Current app state (const).
 */
void ui_screen_now_playing_refresh(const spod_app_state_t *state);

/* ------------------------------------------------------------------ */
/*  Settings screen                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Create Settings screen.
 */
void ui_screen_settings_create(const spod_app_state_t *state);

/**
 * @brief Refresh mutable widgets on Settings screen.
 */
void ui_screen_settings_refresh(const spod_app_state_t *state);

void ui_screen_system_settings_create(const spod_app_state_t *state);
void ui_screen_system_settings_refresh(const spod_app_state_t *state);

void ui_screen_display_settings_create(const spod_app_state_t *state);
void ui_screen_display_settings_refresh(const spod_app_state_t *state);

/**
 * @brief Create Audio Settings screen.
 */
void ui_screen_audio_settings_create(const spod_app_state_t *state);

/**
 * @brief Refresh mutable widgets on Audio Settings screen.
 */
void ui_screen_audio_settings_refresh(const spod_app_state_t *state);

void ui_screen_wifi_settings_create(const spod_app_state_t *state);
void ui_screen_wifi_settings_refresh(const spod_app_state_t *state);

/* ------------------------------------------------------------------ */
/*  Chapters bottom-sheet                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Show or hide the chapters bottom-sheet overlay.
 *
 * When showing, the chapter list is rebuilt from state->selected_story
 * and the current chapter row is highlighted.
 * When hiding, the sheet is simply flagged hidden (no animation on hide
 * so it cannot block touch events underneath).
 *
 * @param visible  true = show with slide-up animation, false = hide instantly.
 * @param state    Current app state.
 */
void ui_chapters_sheet_set_visible(bool visible, const spod_app_state_t *state);

/**
 * @brief Reconnect to the saved network at boot when Wi-Fi is enabled.
 *
 * Runs on the Wi-Fi worker task; the portal starts once connected.
 */
void ui_wifi_autostart(const spod_app_state_t *state);

/* ------------------------------------------------------------------ */
/*  Toast                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Show a transient toast notification on the Now Playing screen.
 *
 * The toast fades out automatically ~2 s after being shown.  Safe to
 * call from any event callback executing on the LVGL task.
 *
 * @param msg  Null-terminated message string; lv_label_set_text() copies it.
 */
void ui_toast_show(const char *msg);

#endif /* UI_SCREENS_H */

