/**
 * @file ui_models.h
 * @brief sPod v2 – data models, enums, and mock dataset declaration.
 *
 * Intentionally free of LVGL headers so this file can be compiled and
 * unit-tested independently of the display stack.
 */

#ifndef UI_MODELS_H
#define UI_MODELS_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#define SPOD_MAX_CHAPTERS   12
#define SPOD_MAX_STORIES     30

/* ------------------------------------------------------------------ */
/*  Enums                                                               */
/* ------------------------------------------------------------------ */

/** Logical screen IDs used by the navigation controller. */
typedef enum {
    SCREEN_LIBRARY = 0,
    SCREEN_STORY_DETAIL,
    SCREEN_NOW_PLAYING,
    SCREEN_SETTINGS,
    SCREEN_SYSTEM_SETTINGS,
    SCREEN_DISPLAY_SETTINGS,
    SCREEN_AUDIO_SETTINGS,
    SCREEN_WIFI_SETTINGS,
    SCREEN_COUNT
} spod_screen_id_t;

/**
 * Cyclic sleep-timer states.
 * Order matters – the cycle wraps from SLEEP_60 back to SLEEP_OFF.
 */
typedef enum {
    SLEEP_OFF = 0,
    SLEEP_15,
    SLEEP_30,
    SLEEP_45,
    SLEEP_60,
    SLEEP_STATE_COUNT
} spod_sleep_state_t;

/** Playback state tracked in app state. */
typedef enum {
    PLAYBACK_STOPPED = 0,
    PLAYBACK_PLAYING,
    PLAYBACK_PAUSED,
    PLAYBACK_SHUTTING_DOWN
} spod_playback_state_t;

/* ------------------------------------------------------------------ */
/*  Data structs                                                        */
/* ------------------------------------------------------------------ */

/** A single chapter within a story. */
typedef struct {
    uint8_t     index;          /**< 0-based chapter index              */
    const char *title;          /**< Display title, e.g. "The Forest"   */
    uint16_t    duration_sec;   /**< Total chapter length in seconds     */
} spod_chapter_t;

/**
 * A story in the library.
 * All string pointers reference string literals (ROM / flash).
 * Replace with heap-allocated strings when loading from SD.
 */
typedef struct {
    uint8_t          id;
    const char      *title;
    const char      *author;
    const char      *duration_label;    /**< Human label, e.g. "42 min"  */
    const char      *album_art_path;    /**< SD path (e.g. /sdcard/... ) or NULL */
    uint32_t         accent_color;      /**< 0xAARRGGBB for lv_color_hex */
    uint8_t          chapter_count;
    spod_chapter_t   chapters[SPOD_MAX_CHAPTERS];
} spod_story_t;

/**
 * Complete application state.
 * One global instance lives in ui_app.c.
 * Screen code accesses it only via ui_app_get_state(); never copies it.
 */
typedef struct {
    /* Navigation */
    spod_screen_id_t      current_screen;

    /* Library selection – NULL when nothing is selected */
    const spod_story_t   *selected_story;

    /* Playback */
    spod_playback_state_t playback_state;
    uint8_t               current_chapter;      /**< 0-based index         */
    uint16_t              elapsed_sec;          /**< Position in chapter   */
    uint16_t              chapter_duration_sec; /**< Chapter total seconds */
    uint8_t               volume;               /**< 0-100                 */
    uint8_t               startup_volume;       /**< 0-100 (next boot)     */
    uint8_t               brightness;           /**< 0-100 user level      */
    uint8_t               min_brightness;       /**< 0-100 dim level       */
    uint16_t              dim_after_sec;         /**< display dim timeout   */
    uint16_t              idle_off_min;          /**< idle shutdown timeout */
    uint8_t               language;             /**< 0 = English, 1 = RO   */
    bool                  startup_sound_enabled;
    bool                  shutdown_sound_enabled;
    bool                  tone_enabled;         /**< Mini DSP on/off        */
    uint8_t               tone_bass;            /**< 0-100 (50 = flat)      */
    uint8_t               tone_treble;          /**< 0-100 (50 = flat)      */
    bool                  sound_enricher_enabled; /**< small speaker voicing */
    uint8_t               sound_enricher_amount;  /**< 0-100 intensity       */
    bool                  mono_enabled;         /**< downmix stereo to mono */
    bool                  wifi_enabled;         /**< upload Wi-Fi mode      */
    bool                  bluetooth_enabled;    /**< status icon only, future */
    bool                  airplay_enabled;      /**< status icon only, future */
    /* Sleep timer */
    spod_sleep_state_t    sleep_state;

    /* Whether the chapters bottom-sheet is currently shown */
    bool                  chapters_open;
} spod_app_state_t;

/* ------------------------------------------------------------------ */
/*  Mock dataset – defined in ui_app.c                                 */
/* ------------------------------------------------------------------ */

extern const spod_story_t g_mock_stories[SPOD_MAX_STORIES];
extern const uint8_t      g_mock_story_count;

/* ------------------------------------------------------------------ */
/*  Inline helpers                                                      */
/* ------------------------------------------------------------------ */

/** Return a human-readable label for a sleep timer state. */
static inline const char *spod_sleep_label(spod_sleep_state_t s)
{
    switch (s) {
        case SLEEP_OFF: return "Sleep: Off";
        case SLEEP_15:  return "Sleep: 15 min";
        case SLEEP_30:  return "Sleep: 30 min";
        case SLEEP_45:  return "Sleep: 45 min";
        case SLEEP_60:  return "Sleep: 60 min";
        default:        return "Sleep: Off";
    }
}

/** Minutes value for a sleep state (0 = disabled). */
static inline uint8_t spod_sleep_minutes(spod_sleep_state_t s)
{
    static const uint8_t lut[SLEEP_STATE_COUNT] = {0, 15, 30, 45, 60};
    return (s < SLEEP_STATE_COUNT) ? lut[s] : 0;
}

#endif /* UI_MODELS_H */

