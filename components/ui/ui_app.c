/**
 * @file ui_app.c
 * @brief sPod v2 - application logic, mock dataset, navigation controller.
 *
 * Fixes applied vs. first draft:
 *   BUG-17: navigate_to no longer unconditionally clears chapters_open.
 *           Callers set that flag before calling navigate_to if needed.
 *   BUG-20: navigate_to accepts a go_back flag and uses MOVE_RIGHT for
 *           back transitions instead of always using MOVE_LEFT.
 *   CLEAN:  playback state transition on navigate is removed from here;
 *           callers (callbacks in ui_screens.c) are responsible for setting
 *           playback_state before calling navigate_to.
 */

#include "ui_app.h"
#include "ui_screens.h"
#include "audio_backend.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_app";

/* ------------------------------------------------------------------ */
/*  Mock story dataset                                                  */
/* ------------------------------------------------------------------ */

const spod_story_t g_mock_stories[SPOD_MAX_STORIES] = {
    {
        .id             = 0,
        .title          = "The Midnight Fox",
        .author         = "Betsy Byars",
        .duration_label = "38 min",
        .accent_color   = 0xFFE06B2A,   /* deep orange */
        .chapter_count  = 5,
        .chapters = {
            { 0, "Into the Wild",    420 },
            { 1, "First Footprints", 390 },
            { 2, "The Den",          480 },
            { 3, "Moonlight Run",    360 },
            { 4, "Safe at Last",     450 },
        }
    },
    {
        .id             = 1,
        .title          = "Pippi Longstocking",
        .author         = "Astrid Lindgren",
        .duration_label = "52 min",
        .accent_color   = 0xFF2A7AE0,   /* sky blue */
        .chapter_count  = 7,
        .chapters = {
            { 0, "Villa Villekulla",      360 },
            { 1, "Strongest Girl",        400 },
            { 2, "The Circus",            420 },
            { 3, "At the Doctor's",       310 },
            { 4, "The Birthday Party",    380 },
            { 5, "Pippi Goes to School",  450 },
            { 6, "A Farewell",            300 },
        }
    },
    {
        .id             = 2,
        .title          = "The Selfish Giant",
        .author         = "Oscar Wilde",
        .duration_label = "18 min",
        .accent_color   = 0xFF6B2AE0,   /* violet */
        .chapter_count  = 3,
        .chapters = {
            { 0, "Winter Always",  380 },
            { 1, "Spring Returns", 290 },
            { 2, "The Garden",     400 },
        }
    },
    {
        .id             = 3,
        .title          = "Fantastic Mr Fox",
        .author         = "Roald Dahl",
        .duration_label = "44 min",
        .accent_color   = 0xFFD4A017,   /* golden */
        .chapter_count  = 6,
        .chapters = {
            { 0, "The Foxes",  300 },
            { 1, "The Farmers",340 },
            { 2, "The Dig",    380 },
            { 3, "Underground",420 },
            { 4, "The Feast",  360 },
            { 5, "Home Again", 290 },
        }
    },
    {
        .id             = 4,
        .title          = "James & Giant Peach",
        .author         = "Roald Dahl",
        .duration_label = "61 min",
        .accent_color   = 0xFF2ABA5A,   /* green */
        .chapter_count  = 8,
        .chapters = {
            { 0, "The Aunts",             360 },
            { 1, "The Crocodile Tongues", 300 },
            { 2, "Inside the Peach",      420 },
            { 3, "New Friends",           390 },
            { 4, "Across the Sea",        450 },
            { 5, "Sharks!",               380 },
            { 6, "Cloud City",            410 },
            { 7, "New York",              320 },
        }
    },
    {
        .id             = 5,
        .title          = "The Lion, the Witch...",
        .author         = "C.S. Lewis",
        .duration_label = "72 min",
        .accent_color   = 0xFFBA2A2A,   /* crimson */
        .chapter_count  = 6,
        .chapters = {
            { 0, "The Wardrobe",    480 },
            { 1, "Narnia",          420 },
            { 2, "The White Witch", 390 },
            { 3, "Aslan's Table",   360 },
            { 4, "The Battle",      450 },
            { 5, "Kings & Queens",  320 },
        }
    },
    {
        .id             = 6,
        .title          = "Charlotte's Web",
        .author         = "E.B. White",
        .duration_label = "49 min",
        .accent_color   = 0xFF2ABAAE,   /* teal */
        .chapter_count  = 5,
        .chapters = {
            { 0, "Wilbur",       350 },
            { 1, "A New Friend", 400 },
            { 2, "The Word",     420 },
            { 3, "The Fair",     380 },
            { 4, "Farewell",     300 },
        }
    },
    {
        .id             = 7,
        .title          = "The Gruffalo",
        .author         = "Julia Donaldson",
        .duration_label = "12 min",
        .accent_color   = 0xFF8B5E3C,   /* earthy brown */
        .chapter_count  = 2,
        .chapters = {
            { 0, "Deep Dark Wood", 380 },
            { 1, "Face to Face",   340 },
        }
    },
};

const uint8_t g_mock_story_count = 8;

/* ------------------------------------------------------------------ */
/*  Runtime SD dataset                                                  */
/* ------------------------------------------------------------------ */

#define UI_TITLE_BUF_LEN  AUDIO_BACKEND_TITLE_MAX
#define UI_LABEL_BUF_LEN  24

static spod_story_t s_runtime_stories[SPOD_MAX_STORIES];
static uint8_t s_runtime_story_count = 0;
static bool s_using_runtime_catalog = false;
static TimerHandle_t s_sleep_timer = NULL;
static ui_app_shutdown_cb_t s_shutdown_cb = NULL;
static bool s_shutdown_in_progress = false;

static char s_story_title_buf[SPOD_MAX_STORIES][UI_TITLE_BUF_LEN];
static char s_story_author_buf[SPOD_MAX_STORIES][UI_TITLE_BUF_LEN];
static char s_story_duration_buf[SPOD_MAX_STORIES][UI_LABEL_BUF_LEN];
static char s_story_art_path_buf[SPOD_MAX_STORIES][AUDIO_BACKEND_PATH_MAX];
static char s_chapter_title_buf[SPOD_MAX_STORIES][SPOD_MAX_CHAPTERS][UI_TITLE_BUF_LEN];

/* ------------------------------------------------------------------ */
/*  Global app state                                                    */
/* ------------------------------------------------------------------ */

static spod_app_state_t s_state = {
    .current_screen  = SCREEN_LIBRARY,
    .selected_story  = NULL,
    .playback_state  = PLAYBACK_STOPPED,
    .current_chapter = 0,
    .elapsed_sec     = 0,
    .chapter_duration_sec = 0,
    .volume          = 40,
    .startup_volume  = 40,
    .brightness      = 100,
    .min_brightness  = 20,
    .dim_after_sec   = 20,
    .idle_off_min    = 10,
    .language        = 0,
    .startup_sound_enabled = true,
    .shutdown_sound_enabled = true,
    .tone_enabled    = false,
    .tone_bass       = 50,
    .tone_treble     = 50,
    .sound_enricher_enabled = false,
    .sound_enricher_amount = 55,
    .mono_enabled    = true,
    .wifi_enabled    = false,
    .bluetooth_enabled = false,
    .airplay_enabled = false,
    .sleep_state     = SLEEP_OFF,
    .chapters_open   = false,
};

static const uint32_t s_runtime_palette[] = {
    0xFFE06B2A, /* deep orange */
    0xFF2A7AE0, /* sky blue */
    0xFFD4A017, /* golden */
    0xFF2ABA5A, /* green */
    0xFFBA2A2A, /* crimson */
    0xFF2ABAAE, /* teal */
    0xFF6B2AE0, /* violet */
    0xFF8B5E3C, /* earthy brown */
};

static esp_err_t settings_nvs_open(nvs_handle_t *out, nvs_open_mode_t mode)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    return nvs_open("spod_ui", mode, out);
}

static void settings_load_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = settings_nvs_open(&nvs, NVS_READONLY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Settings load skipped (%s)", esp_err_to_name(err));
        return;
    }

    uint8_t u8 = 0;
    uint16_t u16 = 0;
    if (nvs_get_u8(nvs, "lang", &u8) == ESP_OK) s_state.language = u8;
    if (nvs_get_u8(nvs, "vol", &u8) == ESP_OK) s_state.volume = u8;
    if (nvs_get_u8(nvs, "startvol", &u8) == ESP_OK) s_state.startup_volume = u8;
    if (nvs_get_u8(nvs, "bright", &u8) == ESP_OK) s_state.brightness = u8;
    if (nvs_get_u8(nvs, "minbr", &u8) == ESP_OK) s_state.min_brightness = u8;
    if (nvs_get_u16(nvs, "dimsec", &u16) == ESP_OK) s_state.dim_after_sec = u16;
    if (nvs_get_u16(nvs, "idlemin", &u16) == ESP_OK) s_state.idle_off_min = u16;
    if (nvs_get_u8(nvs, "startsnd", &u8) == ESP_OK) s_state.startup_sound_enabled = (u8 != 0);
    if (nvs_get_u8(nvs, "offsnd", &u8) == ESP_OK) s_state.shutdown_sound_enabled = (u8 != 0);
    if (nvs_get_u8(nvs, "dsp", &u8) == ESP_OK) s_state.tone_enabled = (u8 != 0);
    if (nvs_get_u8(nvs, "bass", &u8) == ESP_OK) s_state.tone_bass = u8;
    if (nvs_get_u8(nvs, "treble", &u8) == ESP_OK) s_state.tone_treble = u8;
    if (nvs_get_u8(nvs, "senrich", &u8) == ESP_OK) s_state.sound_enricher_enabled = (u8 != 0);
    else if (nvs_get_u8(nvs, "bshift", &u8) == ESP_OK) s_state.sound_enricher_enabled = (u8 != 0);
    if (nvs_get_u8(nvs, "senamt", &u8) == ESP_OK) s_state.sound_enricher_amount = u8;
    else if (nvs_get_u8(nvs, "bshamt", &u8) == ESP_OK) s_state.sound_enricher_amount = u8;
    if (nvs_get_u8(nvs, "mono", &u8) == ESP_OK) s_state.mono_enabled = (u8 != 0);
    if (nvs_get_u8(nvs, "wifi", &u8) == ESP_OK) s_state.wifi_enabled = (u8 != 0);
    nvs_close(nvs);

    if (s_state.volume > 100) s_state.volume = 100;
    if (s_state.startup_volume > 100) s_state.startup_volume = 100;
    if (s_state.brightness > 100) s_state.brightness = 100;
    if (s_state.min_brightness > 100) s_state.min_brightness = 100;
    if (s_state.dim_after_sec < 5) s_state.dim_after_sec = 5;
    if (s_state.idle_off_min < 1) s_state.idle_off_min = 1;
    if (s_state.tone_bass > 100) s_state.tone_bass = 100;
    if (s_state.tone_treble > 100) s_state.tone_treble = 100;
    if (s_state.sound_enricher_amount > 100) s_state.sound_enricher_amount = 100;
}

static esp_err_t settings_save_to_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = settings_nvs_open(&nvs, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, "lang", s_state.language);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "vol", s_state.volume);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "startvol", s_state.startup_volume);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "bright", s_state.brightness);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "minbr", s_state.min_brightness);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "dimsec", s_state.dim_after_sec);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "idlemin", s_state.idle_off_min);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "startsnd", s_state.startup_sound_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "offsnd", s_state.shutdown_sound_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "dsp", s_state.tone_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "bass", s_state.tone_bass);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "treble", s_state.tone_treble);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "senrich", s_state.sound_enricher_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "senamt", s_state.sound_enricher_amount);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "mono", s_state.mono_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "wifi", s_state.wifi_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static uint32_t sleep_state_to_seconds(spod_sleep_state_t s)
{
    switch (s) {
        case SLEEP_15: return 15u * 60u;
        case SLEEP_30: return 30u * 60u;
        case SLEEP_45: return 45u * 60u;
        case SLEEP_60: return 60u * 60u;
        case SLEEP_OFF:
        default:
            return 0u;
    }
}

static void sleep_timeout_async_cb(void *arg)
{
    (void)arg;
    s_state.sleep_state = SLEEP_OFF;
    ui_toast_show("Sleep done");
    ui_app_request_shutdown();
}

static void sleep_timeout_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    lv_async_call(sleep_timeout_async_cb, NULL);
}

static void apply_sleep_timer(void)
{
    uint32_t sec = sleep_state_to_seconds(s_state.sleep_state);
    if (sec == 0u) {
        if (s_sleep_timer) {
            (void)xTimerStop(s_sleep_timer, 0);
        }
        return;
    }

    if (!s_sleep_timer) {
        s_sleep_timer = xTimerCreate("sleep", pdMS_TO_TICKS(1000), pdFALSE, NULL, sleep_timeout_timer_cb);
        if (!s_sleep_timer) {
            ESP_LOGW(TAG, "Sleep timer create failed");
            return;
        }
    }

    (void)xTimerStop(s_sleep_timer, 0);
    (void)xTimerChangePeriod(s_sleep_timer, pdMS_TO_TICKS(sec * 1000u), 0);
}

static uint32_t hash_djb2(const char *s)
{
    uint32_t h = 5381u;
    if (!s) {
        return h;
    }
    while (*s) {
        h = ((h << 5) + h) + (uint8_t)(*s++);
    }
    return h;
}

static uint32_t pick_accent_color(const char *seed)
{
    const size_t palette_len = sizeof(s_runtime_palette) / sizeof(s_runtime_palette[0]);
    if (palette_len == 0) {
        return 0xFFE06B2A;
    }
    return s_runtime_palette[hash_djb2(seed) % palette_len];
}

static void rebuild_runtime_catalog(void)
{
    memset(s_runtime_stories, 0, sizeof(s_runtime_stories));
    memset(s_story_title_buf, 0, sizeof(s_story_title_buf));
    memset(s_story_author_buf, 0, sizeof(s_story_author_buf));
    memset(s_story_duration_buf, 0, sizeof(s_story_duration_buf));
    memset(s_story_art_path_buf, 0, sizeof(s_story_art_path_buf));
    memset(s_chapter_title_buf, 0, sizeof(s_chapter_title_buf));

    s_runtime_story_count = 0;

    uint8_t backend_count = audio_backend_story_count();
    if (backend_count > SPOD_MAX_STORIES) {
        backend_count = SPOD_MAX_STORIES;
    }

    for (uint8_t i = 0; i < backend_count; i++) {
        const audio_backend_story_meta_t *meta = audio_backend_story_get(i);
        if (!meta) {
            continue;
        }

        spod_story_t *dst = &s_runtime_stories[s_runtime_story_count];

        const char *title_src = meta->title[0] ? meta->title : meta->folder_name;
        strlcpy(s_story_title_buf[s_runtime_story_count], title_src,
                sizeof(s_story_title_buf[s_runtime_story_count]));
        strlcpy(s_story_author_buf[s_runtime_story_count], meta->folder_name,
                sizeof(s_story_author_buf[s_runtime_story_count]));

        uint8_t chapter_count = meta->chapter_count;
        if (chapter_count > SPOD_MAX_CHAPTERS) {
            chapter_count = SPOD_MAX_CHAPTERS;
        }
        uint32_t total_sec = 0;

        dst->id = meta->id;
        dst->title = s_story_title_buf[s_runtime_story_count];
        dst->author = s_story_author_buf[s_runtime_story_count];
        if (meta->album_art_path[0]) {
            strlcpy(s_story_art_path_buf[s_runtime_story_count],
                    meta->album_art_path,
                    sizeof(s_story_art_path_buf[s_runtime_story_count]));
            dst->album_art_path = s_story_art_path_buf[s_runtime_story_count];
        } else {
            dst->album_art_path = NULL;
        }
        dst->accent_color = pick_accent_color(dst->title);
        dst->chapter_count = chapter_count;

        for (uint8_t ch = 0; ch < chapter_count; ch++) {
            const char *ch_title_src = meta->chapters[ch].title[0]
                                     ? meta->chapters[ch].title
                                     : meta->chapters[ch].file_name;
            strlcpy(s_chapter_title_buf[s_runtime_story_count][ch], ch_title_src,
                    sizeof(s_chapter_title_buf[s_runtime_story_count][ch]));

            dst->chapters[ch].index = ch;
            dst->chapters[ch].title = s_chapter_title_buf[s_runtime_story_count][ch];
            uint32_t ch_duration = meta->chapters[ch].duration_sec;
            if (ch_duration > UINT16_MAX) {
                ch_duration = UINT16_MAX;
            }
            dst->chapters[ch].duration_sec = (uint16_t)ch_duration;
            total_sec += ch_duration;
        }

        if (total_sec > 0) {
            uint32_t total_min = (total_sec + 59u) / 60u;
            snprintf(s_story_duration_buf[s_runtime_story_count],
                     sizeof(s_story_duration_buf[s_runtime_story_count]),
                     "%u min",
                     (unsigned)total_min);
        } else {
            snprintf(s_story_duration_buf[s_runtime_story_count],
                     sizeof(s_story_duration_buf[s_runtime_story_count]),
                     "%u chapter%s",
                     (unsigned)chapter_count,
                     (chapter_count == 1u) ? "" : "s");
        }
        dst->duration_label = s_story_duration_buf[s_runtime_story_count];

        s_runtime_story_count++;
    }

    s_using_runtime_catalog = (s_runtime_story_count > 0);
    if (s_using_runtime_catalog) {
        ESP_LOGI(TAG, "Using runtime SD catalog: %u storie(s)", (unsigned)s_runtime_story_count);
    } else {
        ESP_LOGW(TAG, "Runtime SD catalog is empty, fallback to mock stories");
    }
}

static void audio_error_toast(esp_err_t err)
{
    if (err == ESP_ERR_NOT_FOUND) {
        ui_toast_show("No MP3 files on SD");
        return;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ui_toast_show("MP3 decoder missing");
        return;
    }
    if (err != ESP_OK) {
        ui_toast_show("Audio backend error");
    }
}

static void apply_tone_settings(void)
{
    esp_err_t err = audio_backend_set_tone_control(s_state.tone_enabled,
                                                   s_state.tone_bass,
                                                   s_state.tone_treble);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set Mini DSP failed (%s)", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------ */
/*  State accessor                                                      */
/* ------------------------------------------------------------------ */

spod_app_state_t *ui_app_get_state(void)
{
    return &s_state;
}

void ui_app_load_persisted_settings(void)
{
    settings_load_from_nvs();
    ui_set_user_brightness(s_state.brightness);
    ui_set_min_brightness(s_state.min_brightness);
    ui_set_dim_after_seconds(s_state.dim_after_sec);
    ui_set_idle_off_minutes(s_state.idle_off_min);
    (void)audio_backend_set_startup_volume(s_state.startup_volume);
    (void)audio_backend_set_shutdown_tone_enabled(s_state.shutdown_sound_enabled);
    (void)audio_backend_set_sound_enricher(s_state.sound_enricher_enabled, s_state.sound_enricher_amount);
    (void)audio_backend_set_mono_enabled(s_state.mono_enabled);
}

void ui_app_apply_user_settings(const spod_app_state_t *draft)
{
    if (!draft) {
        return;
    }

    s_state.language = draft->language;
    ui_app_set_volume(draft->volume);
    ui_app_set_startup_volume(draft->startup_volume);
    ui_app_set_brightness(draft->brightness);
    ui_app_set_min_brightness_setting(draft->min_brightness);
    ui_app_set_dim_after_seconds_setting(draft->dim_after_sec);
    ui_app_set_idle_off_minutes_setting(draft->idle_off_min);
    ui_app_set_startup_sound_enabled(draft->startup_sound_enabled);
    ui_app_set_shutdown_sound_enabled(draft->shutdown_sound_enabled);
    ui_app_set_tone_enabled(draft->tone_enabled);
    ui_app_set_tone_bass(draft->tone_bass);
    ui_app_set_tone_treble(draft->tone_treble);
    ui_app_set_sound_enricher_enabled(draft->sound_enricher_enabled);
    ui_app_set_sound_enricher_amount(draft->sound_enricher_amount);
    ui_app_set_mono_enabled(draft->mono_enabled);
    s_state.wifi_enabled = draft->wifi_enabled;

    esp_err_t err = settings_save_to_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Settings save failed (%s)", esp_err_to_name(err));
        ui_toast_show("Settings save failed");
    } else {
        ui_toast_show("Settings saved");
    }
}

/* ------------------------------------------------------------------ */
/*  Navigation controller                                               */
/*                                                                      */
/*  FIX BUG-17: chapters_open is NOT touched here.  Callers that want  */
/*  to open or close the sheet set s_state.chapters_open first, then   */
/*  call navigate_to().  refresh functions read it from state.          */
/*                                                                      */
/*  FIX BUG-20: go_back drives animation direction.                    */
/* ------------------------------------------------------------------ */

void ui_app_navigate_to(spod_screen_id_t target, bool go_back)
{
    lv_obj_t *dest = NULL;
    lv_scr_load_anim_t anim = go_back
        ? LV_SCR_LOAD_ANIM_MOVE_RIGHT
        : LV_SCR_LOAD_ANIM_MOVE_LEFT;

    switch (target) {
        case SCREEN_LIBRARY:
            dest = g_scr_library;
            break;

        case SCREEN_STORY_DETAIL:
            if (!s_state.selected_story) return;
            ui_screen_detail_refresh(&s_state);
            dest = g_scr_detail;
            break;

        case SCREEN_NOW_PLAYING:
            if (!s_state.selected_story) return;
            ui_screen_now_playing_refresh(&s_state);
            dest = g_scr_now_playing;
            break;

        case SCREEN_SETTINGS:
            ui_screen_settings_refresh(&s_state);
            dest = g_scr_settings;
            break;

        case SCREEN_SYSTEM_SETTINGS:
            ui_screen_system_settings_refresh(&s_state);
            dest = g_scr_system_settings;
            break;

        case SCREEN_DISPLAY_SETTINGS:
            ui_screen_display_settings_refresh(&s_state);
            dest = g_scr_display_settings;
            break;

        case SCREEN_AUDIO_SETTINGS:
            ui_screen_audio_settings_refresh(&s_state);
            dest = g_scr_audio_settings;
            break;

        case SCREEN_WIFI_SETTINGS:
            ui_screen_wifi_settings_refresh(&s_state);
            dest = g_scr_wifi_settings;
            break;

        default:
            return;
    }

    s_state.current_screen = target;
    lv_scr_load_anim(dest, anim, 220, 0, false);
}

void ui_app_set_shutdown_callback(ui_app_shutdown_cb_t cb)
{
    s_shutdown_cb = cb;
}

void ui_app_request_shutdown(void)
{
    if (s_shutdown_in_progress) {
        return;
    }
    s_shutdown_in_progress = true;

    s_state.sleep_state = SLEEP_OFF;
    apply_sleep_timer();
    s_state.chapters_open = false;
    s_state.playback_state = PLAYBACK_SHUTTING_DOWN;

    if (s_state.current_screen == SCREEN_NOW_PLAYING) {
        ui_screen_now_playing_refresh(&s_state);
    }

    if (s_shutdown_cb) {
        s_shutdown_cb();
    }
}

/* ------------------------------------------------------------------ */
/*  Story selection                                                     */
/* ------------------------------------------------------------------ */

uint8_t ui_app_story_count(void)
{
    return s_using_runtime_catalog ? s_runtime_story_count : g_mock_story_count;
}

const spod_story_t *ui_app_story_get(uint8_t story_index)
{
    if (story_index >= ui_app_story_count()) {
        return NULL;
    }
    return s_using_runtime_catalog
        ? &s_runtime_stories[story_index]
        : &g_mock_stories[story_index];
}

/*
 * Background playback: browsing another story must not stop what is playing.
 * The playing story's context is parked here while s_state shows the browsed
 * story, and restored when the user (or the idle timer) returns to it.
 */
typedef struct {
    const spod_story_t   *story;     /* NULL: nothing parked */
    uint8_t               chapter;
    uint16_t              elapsed_sec;
    uint16_t              duration_sec;
    spod_playback_state_t playback_state;
} bg_playback_t;

static bg_playback_t s_bg;

static void bg_park_current(void)
{
    s_bg.story = s_state.selected_story;
    s_bg.chapter = s_state.current_chapter;
    s_bg.elapsed_sec = s_state.elapsed_sec;
    s_bg.duration_sec = s_state.chapter_duration_sec;
    s_bg.playback_state = s_state.playback_state;
}

static void bg_restore(void)
{
    s_state.selected_story = s_bg.story;
    s_state.current_chapter = s_bg.chapter;
    s_state.elapsed_sec = s_bg.elapsed_sec;
    s_state.chapter_duration_sec = s_bg.duration_sec;
    s_state.playback_state = s_bg.playback_state;
    s_state.chapters_open = false;
    s_bg.story = NULL;
}

bool ui_app_is_playback_active(void)
{
    return s_bg.story != NULL ||
           (s_state.selected_story && s_state.playback_state != PLAYBACK_STOPPED);
}

bool ui_app_return_to_now_playing(void)
{
    if (s_bg.story) {
        bg_restore();
    }
    if (!s_state.selected_story || s_state.playback_state == PLAYBACK_STOPPED) {
        return false;
    }
    if (s_state.current_screen != SCREEN_NOW_PLAYING) {
        s_state.chapters_open = false;
        ui_app_navigate_to(SCREEN_NOW_PLAYING, false);
    }
    return true;
}

void ui_app_select_story(uint8_t story_index)
{
    const spod_story_t *story = ui_app_story_get(story_index);
    if (!story) {
        return;
    }

    if (s_bg.story == story) {
        /* Back to the story that is playing in the background. */
        bg_restore();
        return;
    }
    if (s_state.selected_story == story && s_state.playback_state != PLAYBACK_STOPPED) {
        s_state.chapters_open = false;
        return;
    }
    if (s_state.selected_story && s_state.playback_state != PLAYBACK_STOPPED) {
        bg_park_current();
    }

    s_state.selected_story  = story;
    s_state.current_chapter = 0;
    s_state.elapsed_sec     = 0;
    s_state.chapter_duration_sec = (story->chapter_count > 0u)
        ? story->chapters[0].duration_sec
        : 0u;
    s_state.playback_state  = PLAYBACK_STOPPED;
    s_state.chapters_open   = false;
}

/* ------------------------------------------------------------------ */
/*  Playback toggle                                                     */
/* ------------------------------------------------------------------ */

void ui_app_toggle_playback(void)
{
    if (s_shutdown_in_progress) {
        return;
    }
    if (!s_state.selected_story) {
        return;
    }

    esp_err_t err = ESP_OK;
    if (s_state.playback_state == PLAYBACK_PLAYING) {
        err = audio_backend_pause();
        if (err == ESP_OK) {
            s_state.playback_state = PLAYBACK_PAUSED;
        }
    } else {
        if (s_state.playback_state == PLAYBACK_PAUSED) {
            err = audio_backend_resume();
        } else {
            err = audio_backend_play_chapter(s_state.selected_story->id, s_state.current_chapter);
            s_bg.story = NULL; /* the backend replaced the parked playback */
        }

        if (err == ESP_OK) {
            s_state.playback_state = PLAYBACK_PLAYING;
        } else {
            s_state.playback_state = PLAYBACK_STOPPED;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Playback toggle failed (%s)", esp_err_to_name(err));
        audio_error_toast(err);
    }

    ui_screen_now_playing_refresh(&s_state);
}

/* ------------------------------------------------------------------ */
/*  Chapter selection                                                   */
/* ------------------------------------------------------------------ */

void ui_app_select_chapter(uint8_t chapter_index)
{
    if (s_shutdown_in_progress) {
        return;
    }
    if (!s_state.selected_story) return;
    if (chapter_index >= s_state.selected_story->chapter_count) return;

    s_state.current_chapter = chapter_index;
    s_state.elapsed_sec     = 0;
    s_state.chapter_duration_sec = s_state.selected_story->chapters[chapter_index].duration_sec;
    esp_err_t err = audio_backend_play_chapter(s_state.selected_story->id, chapter_index);
    s_bg.story = NULL;
    if (err == ESP_OK) {
        s_state.playback_state = PLAYBACK_PLAYING;
    } else {
        s_state.playback_state = PLAYBACK_STOPPED;
        ESP_LOGW(TAG, "Select chapter failed (%s)", esp_err_to_name(err));
        audio_error_toast(err);
    }
}

bool ui_app_selected_story_resume(uint8_t *chapter_index, uint32_t *resume_sec)
{
    if (!s_state.selected_story || !s_using_runtime_catalog) {
        if (chapter_index) {
            *chapter_index = 0;
        }
        if (resume_sec) {
            *resume_sec = 0;
        }
        return false;
    }

    const audio_backend_story_meta_t *meta = audio_backend_story_get(s_state.selected_story->id);
    if (!meta || meta->resume_time_sec == 0) {
        if (chapter_index) {
            *chapter_index = 0;
        }
        if (resume_sec) {
            *resume_sec = 0;
        }
        return false;
    }

    uint8_t ch = meta->resume_chapter_index;
    if (ch >= s_state.selected_story->chapter_count) {
        ch = 0;
    }

    if (chapter_index) {
        *chapter_index = ch;
    }
    if (resume_sec) {
        *resume_sec = meta->resume_time_sec;
    }
    return true;
}

void ui_app_resume_selected_story(void)
{
    if (s_shutdown_in_progress) {
        return;
    }
    if (!s_state.selected_story) {
        return;
    }

    uint8_t resume_chapter = 0;
    uint32_t resume_sec = 0;
    if (!ui_app_selected_story_resume(&resume_chapter, &resume_sec)) {
        return;
    }

    if (resume_chapter >= s_state.selected_story->chapter_count) {
        resume_chapter = 0;
    }

    s_state.current_chapter = resume_chapter;
    if (resume_sec > UINT16_MAX) {
        resume_sec = UINT16_MAX;
    }
    s_state.elapsed_sec = (uint16_t)resume_sec;
    s_state.chapter_duration_sec = s_state.selected_story->chapters[resume_chapter].duration_sec;

    esp_err_t err = audio_backend_play_chapter_from(s_state.selected_story->id, resume_chapter, resume_sec);
    s_bg.story = NULL;
    if (err == ESP_OK) {
        s_state.playback_state = PLAYBACK_PLAYING;
    } else {
        s_state.playback_state = PLAYBACK_STOPPED;
        ESP_LOGW(TAG, "Resume story failed (%s)", esp_err_to_name(err));
        audio_error_toast(err);
    }
}

/* ------------------------------------------------------------------ */
/*  Volume                                                              */
/* ------------------------------------------------------------------ */

void ui_app_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_state.volume = volume;
    esp_err_t err = audio_backend_set_volume(volume);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set volume failed (%s)", esp_err_to_name(err));
    }
}

void ui_app_set_startup_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    s_state.startup_volume = volume;
    esp_err_t err = audio_backend_set_startup_volume(volume);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set startup volume failed (%s)", esp_err_to_name(err));
    }
}

uint8_t ui_app_get_startup_volume(void)
{
    return s_state.startup_volume;
}

void ui_app_set_tone_enabled(bool enabled)
{
    s_state.tone_enabled = enabled;
    apply_tone_settings();
}

void ui_app_set_tone_bass(uint8_t bass)
{
    if (bass > 100u) {
        bass = 100u;
    }
    s_state.tone_bass = bass;
    apply_tone_settings();
}

void ui_app_set_tone_treble(uint8_t treble)
{
    if (treble > 100u) {
        treble = 100u;
    }
    s_state.tone_treble = treble;
    apply_tone_settings();
}

void ui_app_set_sound_enricher_enabled(bool enabled)
{
    s_state.sound_enricher_enabled = enabled;
    esp_err_t err = audio_backend_set_sound_enricher(s_state.sound_enricher_enabled, s_state.sound_enricher_amount);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set sound enricher failed (%s)", esp_err_to_name(err));
    }
}

void ui_app_set_sound_enricher_amount(uint8_t amount)
{
    if (amount > 100u) {
        amount = 100u;
    }
    s_state.sound_enricher_amount = amount;
    esp_err_t err = audio_backend_set_sound_enricher(s_state.sound_enricher_enabled, s_state.sound_enricher_amount);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set sound enricher amount failed (%s)", esp_err_to_name(err));
    }
}

void ui_app_set_mono_enabled(bool enabled)
{
    s_state.mono_enabled = enabled;
    esp_err_t err = audio_backend_set_mono_enabled(enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set mono failed (%s)", esp_err_to_name(err));
    }
}

void ui_app_set_startup_sound_enabled(bool enabled)
{
    s_state.startup_sound_enabled = enabled;
}

bool ui_app_get_startup_sound_enabled(void)
{
    return s_state.startup_sound_enabled;
}

void ui_app_set_shutdown_sound_enabled(bool enabled)
{
    s_state.shutdown_sound_enabled = enabled;
    esp_err_t err = audio_backend_set_shutdown_tone_enabled(enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set shutdown sound failed (%s)", esp_err_to_name(err));
    }
}

bool ui_app_get_shutdown_sound_enabled(void)
{
    return s_state.shutdown_sound_enabled;
}

void ui_app_set_brightness(uint8_t brightness)
{
    if (brightness > 100) {
        brightness = 100;
    }
    s_state.brightness = brightness;
    ui_set_user_brightness(brightness);
}

uint8_t ui_app_get_brightness(void)
{
    return s_state.brightness;
}

void ui_app_set_min_brightness_setting(uint8_t brightness)
{
    if (brightness > 100) {
        brightness = 100;
    }
    s_state.min_brightness = brightness;
    ui_set_min_brightness(brightness);
}

void ui_app_set_dim_after_seconds_setting(uint16_t seconds)
{
    if (seconds < 5u) {
        seconds = 5u;
    }
    s_state.dim_after_sec = seconds;
    ui_set_dim_after_seconds(seconds);
}

void ui_app_set_idle_off_minutes_setting(uint16_t minutes)
{
    if (minutes < 1u) {
        minutes = 1u;
    }
    s_state.idle_off_min = minutes;
    ui_set_idle_off_minutes(minutes);
}

void ui_app_set_language(uint8_t language)
{
    s_state.language = language ? 1u : 0u;
}

void ui_app_set_wifi_enabled(bool enabled)
{
    if (s_state.wifi_enabled == enabled) {
        return;
    }
    s_state.wifi_enabled = enabled;
    esp_err_t err = settings_save_to_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi setting save failed (%s)", esp_err_to_name(err));
    }
}

void ui_app_stop_playback(void)
{
    if (s_shutdown_in_progress) {
        return;
    }
    esp_err_t err = audio_backend_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Stop playback failed (%s)", esp_err_to_name(err));
        audio_error_toast(err);
    }
    s_bg.story = NULL;
    s_state.playback_state = PLAYBACK_STOPPED;
    s_state.elapsed_sec = 0;
    if (s_state.selected_story && s_state.current_chapter < s_state.selected_story->chapter_count) {
        s_state.chapter_duration_sec = s_state.selected_story->chapters[s_state.current_chapter].duration_sec;
    } else {
        s_state.chapter_duration_sec = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Sleep timer                                                         */
/* ------------------------------------------------------------------ */

void ui_app_cycle_sleep_timer(void)
{
    s_state.sleep_state =
        (spod_sleep_state_t)((s_state.sleep_state + 1) % SLEEP_STATE_COUNT);

    apply_sleep_timer();

    if (s_state.sleep_state == SLEEP_OFF) {
        ui_toast_show("Sleep: Off");
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), "Sleep: %u min", (unsigned)(sleep_state_to_seconds(s_state.sleep_state) / 60u));
        ui_toast_show(buf);
    }
    ui_screen_now_playing_refresh(&s_state);
}

/* Keeps parked background playback moving: progress and auto-advance to the next chapter. */
static void bg_poll_playback_progress(void)
{
    if (audio_backend_consume_finished_event()) {
        const uint8_t next = (uint8_t)(s_bg.chapter + 1u);
        if (next < s_bg.story->chapter_count &&
            audio_backend_play_chapter(s_bg.story->id, next) == ESP_OK) {
            s_bg.chapter = next;
            s_bg.elapsed_sec = 0;
            s_bg.duration_sec = s_bg.story->chapters[next].duration_sec;
        } else {
            s_bg.story = NULL;
        }
        return;
    }

    uint32_t elapsed_sec = 0;
    uint32_t duration_sec = 0;
    if (audio_backend_get_playback_progress(&elapsed_sec, &duration_sec) == ESP_OK) {
        s_bg.elapsed_sec = (uint16_t)(elapsed_sec > UINT16_MAX ? UINT16_MAX : elapsed_sec);
        if (duration_sec > 0u) {
            s_bg.duration_sec = (uint16_t)(duration_sec > UINT16_MAX ? UINT16_MAX : duration_sec);
        }
    }
}

void ui_app_poll_playback_progress(void)
{
    if (s_shutdown_in_progress) {
        return;
    }
    if (s_bg.story) {
        bg_poll_playback_progress();
        return;
    }
    if (!s_state.selected_story || s_state.playback_state == PLAYBACK_STOPPED) {
        return;
    }

    if (audio_backend_consume_finished_event()) {
        bool moved_to_next = false;
        if (s_state.current_chapter + 1u < s_state.selected_story->chapter_count) {
            uint8_t next_chapter = (uint8_t)(s_state.current_chapter + 1u);
            ui_app_select_chapter(next_chapter);
            moved_to_next = (s_state.playback_state == PLAYBACK_PLAYING);
            if (moved_to_next) {
                ui_toast_show("Next chapter");
            }
        }

        if (!moved_to_next) {
            s_state.playback_state = PLAYBACK_STOPPED;
            s_state.elapsed_sec = 0;
            if (s_state.current_chapter < s_state.selected_story->chapter_count) {
                s_state.chapter_duration_sec = s_state.selected_story->chapters[s_state.current_chapter].duration_sec;
            } else {
                s_state.chapter_duration_sec = 0;
            }
        }

        if (s_state.current_screen == SCREEN_NOW_PLAYING) {
            ui_screen_now_playing_refresh(&s_state);
        }
        return;
    }

    uint32_t elapsed_sec = 0;
    uint32_t duration_sec = 0;
    if (audio_backend_get_playback_progress(&elapsed_sec, &duration_sec) != ESP_OK) {
        return;
    }

    if (elapsed_sec > UINT16_MAX) {
        elapsed_sec = UINT16_MAX;
    }
    if (duration_sec > UINT16_MAX) {
        duration_sec = UINT16_MAX;
    }

    bool changed = false;
    uint16_t elapsed16 = (uint16_t)elapsed_sec;
    uint16_t duration16 = (uint16_t)duration_sec;

    if (s_state.elapsed_sec != elapsed16) {
        s_state.elapsed_sec = elapsed16;
        changed = true;
    }
    if (duration16 > 0u && s_state.chapter_duration_sec != duration16) {
        s_state.chapter_duration_sec = duration16;
        changed = true;
    }

    if (changed && s_state.current_screen == SCREEN_NOW_PLAYING) {
        ui_screen_now_playing_refresh(&s_state);
    }
}

/* ------------------------------------------------------------------ */
/*  Init                                                                */
/* ------------------------------------------------------------------ */
//static lv_obj_t *s_root;
void ui_app_init(void)
{
#if !LV_USE_FS_STDIO
    ESP_LOGW(TAG, "Cover art disabled: LV_USE_FS_STDIO=0 (enable in menuconfig)");
#endif
#if !LV_USE_BMP
    ESP_LOGW(TAG, "BMP cover art disabled: LV_USE_BMP=0 (enable in menuconfig)");
#endif
#if !LV_USE_SJPG
    ESP_LOGW(TAG, "JPG cover art disabled: LV_USE_SJPG=0 (enable in menuconfig)");
#endif

    esp_err_t err = audio_backend_init();
    (void)audio_backend_set_startup_volume(s_state.startup_volume);
    (void)audio_backend_set_shutdown_tone_enabled(s_state.shutdown_sound_enabled);
    (void)audio_backend_set_mono_enabled(s_state.mono_enabled);
    ui_set_user_brightness(s_state.brightness);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio_backend_init failed (%s)", esp_err_to_name(err));
        s_using_runtime_catalog = false;
        s_runtime_story_count = 0;
    } else {
        esp_err_t vol_err = audio_backend_set_volume(s_state.volume);
        if (vol_err != ESP_OK) {
            ESP_LOGW(TAG, "Initial volume sync failed (%s)", esp_err_to_name(vol_err));
        }
        apply_tone_settings();
        rebuild_runtime_catalog();
    }

    /*
     * Creation order: Library has no forward references; Detail and
     * Now Playing are created while not yet visible.
     */

    ui_screen_library_create(&s_state);
    vTaskDelay(pdMS_TO_TICKS(1));
    ui_screen_detail_create(&s_state);
    vTaskDelay(pdMS_TO_TICKS(1));
    ui_screen_now_playing_create(&s_state);
    vTaskDelay(pdMS_TO_TICKS(1));
    ui_screen_settings_create(&s_state);
    vTaskDelay(pdMS_TO_TICKS(1));
    ui_screen_system_settings_create(&s_state);
    vTaskDelay(pdMS_TO_TICKS(1));
    ui_screen_display_settings_create(&s_state);
    vTaskDelay(pdMS_TO_TICKS(1));
    ui_screen_audio_settings_create(&s_state);
    vTaskDelay(pdMS_TO_TICKS(1));
    ui_screen_wifi_settings_create(&s_state);
    vTaskDelay(pdMS_TO_TICKS(1));

    lv_disp_load_scr(g_scr_library);
    s_state.current_screen = SCREEN_LIBRARY;

    ui_wifi_autostart(&s_state);
}
