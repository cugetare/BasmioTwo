/**
 * @file ui_screens.c
 * @brief sPod v2 - LVGL widget tree construction and refresh logic.
 *
 * Cumulative fix history
 * ----------------------
 * Pass 1 fixes (BUG-01..BUG-20):
 *   BUG-01/12  Dedicated s_det_art_lbl for artwork letter (was overwritten).
 *   BUG-02     sheet_anim_y_cb wrapper avoids UB cast of lv_obj_set_y.
 *   BUG-04     Chapter userdata uses &s_ch_idx_data[i], not value-as-pointer.
 *   BUG-05     lv_list_get_btn_label() replaces fragile child-index access.
 *   BUG-06     make_round_btn() label_out param replaces child-index access.
 *   BUG-14     Explicit pad_top/bottom/left/right replaces pad_ver/pad_hor.
 *   BUG-17     navigate_to() no longer clears chapters_open; callers own it.
 *   BUG-20     go_back flag drives MOVE_LEFT vs MOVE_RIGHT animation.
 *
 * Pass 2 fixes (ISSUE-A..ISSUE-K):
 *   ISSUE-A    rebuild_chapter_list() is now guarded by a dirty flag.
 *              It only runs when the selected story or current chapter
 *              changes ??? never on unrelated refresh() calls while the
 *              sheet is open (e.g. play/pause toggle while browsing).
 *   ISSUE-B    Detail screen info labels now use fixed y-offsets within
 *              the padded card instead of lv_obj_align_to() chains.
 *              align_to chains store the alignment relational to an
 *              object's position at creation time; they do not reflow
 *              when dynamic label text changes the reference object's
 *              height (e.g. a long title wrapping to two lines).
 *   ISSUE-D    Volume display consolidation: update_vol_display() is the
 *              single point of truth for setting slider + label together.
 *              Previously three callbacks each duplicated that logic.
 *   ISSUE-E    Chapter list numbering uses loop index `i + 1`, not
 *              `ch->index + 1`. ch->index is a field in the data struct
 *              that could diverge from the loop counter if data is loaded
 *              out of order from SD. The loop counter is always correct.
 *   ISSUE-K    Redundant `state->selected_story == s` pointer comparison
 *              in ui_screen_detail_refresh() removed. s IS selected_story
 *              by definition at that point; the check was always true.
 */

#include "ui_screens.h"
#include "ui_app.h"
#include "audio_backend.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
//#include "lv_conf.h"

static const char *TAG = "ui_screens";

static void ui_build_yield(void)
{
    vTaskDelay(pdMS_TO_TICKS(1));
}

/* ================================================================== */
/*  Screen handles (definitions)                                        */
/* ================================================================== */

lv_obj_t *g_scr_library     = NULL;
lv_obj_t *g_scr_detail      = NULL;
lv_obj_t *g_scr_now_playing = NULL;
lv_obj_t *g_scr_settings    = NULL;
lv_obj_t *g_scr_system_settings = NULL;
lv_obj_t *g_scr_display_settings = NULL;
lv_obj_t *g_scr_audio_settings = NULL;
lv_obj_t *g_scr_wifi_settings = NULL;

/* ================================================================== */
/*  Persistent widget references needed for refresh                    */
/* ================================================================== */

/* --- Detail screen --- */
static lv_obj_t *s_det_accent_bar = NULL;   /* coloured artwork bg band    */
static lv_obj_t *s_det_art_img    = NULL;   /* cover image (if available)  */
static lv_obj_t *s_det_art_lbl    = NULL;   /* large initial letter        */
static lv_obj_t *s_det_title_lbl  = NULL;   /* story title                 */
static lv_obj_t *s_det_author_lbl = NULL;
static lv_obj_t *s_det_dur_lbl    = NULL;
static lv_obj_t *s_det_ch_lbl     = NULL;   /* chapter count               */
static lv_obj_t *s_det_play_lbl   = NULL;   /* "Play" / "Continue"         */
static lv_obj_t *s_det_resume_btn = NULL;
static lv_obj_t *s_det_resume_lbl = NULL;

/* --- Now Playing screen --- */
static lv_obj_t *s_np_title_lbl     = NULL;
static lv_obj_t *s_np_art_box       = NULL;
static lv_obj_t *s_np_art_img       = NULL;
static lv_obj_t *s_np_art_lbl       = NULL;
static lv_obj_t *s_np_chapter_lbl   = NULL;
static lv_obj_t *s_np_prog_bar      = NULL;
static lv_obj_t *s_np_elapsed_lbl   = NULL;
static lv_obj_t *s_np_total_lbl     = NULL;
static lv_obj_t *s_np_playpause_lbl = NULL;
static lv_obj_t *s_np_vol_slider    = NULL;
static lv_obj_t *s_np_vol_lbl       = NULL;
static lv_obj_t *s_np_sleep_btn     = NULL;
static lv_obj_t *s_np_sleep_arc     = NULL;
static lv_obj_t *s_np_sleep_power_lbl = NULL;
static lv_obj_t *s_np_toast         = NULL;
static const spod_story_t *s_np_cached_story = NULL;
static uint8_t s_np_cached_chapter = 0xFF;
static uint8_t s_np_cached_volume = 0xFF;
static spod_sleep_state_t s_np_cached_sleep = SLEEP_STATE_COUNT;

/* --- Chapters bottom sheet --- */
/*
 * Was a fixed 480 px from the 800 px tall P4 layout; on the 320 px panel that
 * slid the header and close button off the top. Cover the full screen instead.
 */
#define CH_SHEET_H        DISPLAY_H
#define CH_SHEET_HEADER_H 68
static lv_obj_t *s_ch_sheet = NULL;
static lv_obj_t *s_ch_list  = NULL;

/* --- Settings screen --- */
static lv_obj_t *s_set_brightness_slider = NULL;
static lv_obj_t *s_set_brightness_lbl = NULL;
static lv_obj_t *s_set_volume_slider = NULL;
static lv_obj_t *s_set_volume_lbl = NULL;
static lv_obj_t *s_set_startup_vol_slider = NULL;
static lv_obj_t *s_set_startup_vol_lbl = NULL;
static lv_obj_t *s_set_language_lbl = NULL;
static lv_obj_t *s_set_idle_slider = NULL;
static lv_obj_t *s_set_idle_lbl = NULL;
static lv_obj_t *s_set_min_brightness_slider = NULL;
static lv_obj_t *s_set_min_brightness_lbl = NULL;
static lv_obj_t *s_set_dim_slider = NULL;
static lv_obj_t *s_set_dim_lbl = NULL;
static lv_obj_t *s_set_startup_sound_switch = NULL;
static lv_obj_t *s_set_shutdown_sound_switch = NULL;
static lv_obj_t *s_set_tone_switch = NULL;
static lv_obj_t *s_set_bass_slider = NULL;
static lv_obj_t *s_set_bass_lbl = NULL;
static lv_obj_t *s_set_treble_slider = NULL;
static lv_obj_t *s_set_treble_lbl = NULL;
static lv_obj_t *s_set_sound_enricher_switch = NULL;
static lv_obj_t *s_set_sound_enricher_slider = NULL;
static lv_obj_t *s_set_sound_enricher_lbl = NULL;
static lv_obj_t *s_set_mono_switch = NULL;
static lv_obj_t *s_set_wifi_switch = NULL;
static lv_obj_t *s_set_wifi_lbl = NULL;
static lv_obj_t *s_set_wifi_saved_lbl = NULL;
static lv_obj_t *s_set_wifi_scan_list = NULL;
static lv_obj_t *s_wifi_password_overlay = NULL;
static lv_obj_t *s_wifi_password_ta = NULL;
static spod_app_state_t s_settings_draft;
static bool s_settings_draft_valid = false;
static audio_backend_wifi_ap_t s_wifi_scan_results[AUDIO_BACKEND_WIFI_SCAN_MAX];
static uint8_t s_wifi_scan_indices[AUDIO_BACKEND_WIFI_SCAN_MAX];
static size_t s_wifi_scan_count = 0;
static char s_wifi_selected_ssid[AUDIO_BACKEND_WIFI_SSID_MAX] = "";

typedef enum {
    WIFI_UI_IDLE = 0,
    WIFI_UI_SCANNING,
    WIFI_UI_CONNECTING,
    WIFI_UI_APPLYING,
    WIFI_UI_FORGETTING,
} ui_wifi_state_t;

typedef enum {
    WIFI_JOB_SCAN = 0,
    WIFI_JOB_CONNECT,
    WIFI_JOB_FORGET,
    WIFI_JOB_APPLY_ENABLED,
} ui_wifi_job_type_t;

typedef struct {
    ui_wifi_job_type_t type;
    bool enable;
    bool save;
    char ssid[AUDIO_BACKEND_WIFI_SSID_MAX];
    char password[AUDIO_BACKEND_WIFI_PASSWORD_MAX];
} ui_wifi_job_t;

static portMUX_TYPE s_wifi_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static ui_wifi_state_t s_wifi_ui_state = WIFI_UI_IDLE;
static bool s_wifi_job_running = false;
static ui_wifi_job_t s_wifi_job;
static esp_err_t s_wifi_job_result = ESP_OK;
static char s_wifi_job_toast[72] = "";

typedef struct {
    lv_obj_t *title_lbl;
    lv_obj_t *clock_lbl;
    lv_obj_t *wifi_lbl;
    lv_obj_t *wifi_slash;
    lv_obj_t *bt_lbl;
    lv_obj_t *airplay_lbl;
} ui_status_bar_t;

static ui_status_bar_t s_status_bars[SCREEN_COUNT];
static uint8_t s_status_bar_count = 0;
static lv_timer_t *s_status_bar_timer = NULL;

/* ================================================================== */
/*  Internal state                                                      */
/* ================================================================== */

/* Toast animation handle ??? must be file-scope for lv_anim_del() lookup */
static lv_anim_t s_toast_anim;

/*
 * Chapter userdata: file-scope so pointer addresses are stable for the
 * lifetime of the app. s_ch_idx_data[i] always stores value i.
 * (ISSUE-A related: these never move even across rebuilds.)
 */
static uint8_t s_ch_idx_data[SPOD_MAX_CHAPTERS];

/*
 * ISSUE-A FIX: Chapter list dirty tracking.
 * The list is rebuilt only when the story or highlighted chapter changes.
 * Refresh calls that do not change either (e.g. play/pause, vol change)
 * will find the dirty flag clear and skip the expensive lv_obj_clean().
 */
static const spod_story_t *s_ch_built_for_story   = NULL;
static uint8_t             s_ch_built_for_chapter  = 0xFF; /* sentinel: "never built" */

/* ================================================================== */
/*  Forward declarations                                                */
/* ================================================================== */

/* Animation callback wrappers */
static void sheet_anim_y_cb(void *obj, int32_t y);
static void toast_anim_opa_cb(void *obj, int32_t opa);
static void toast_anim_ready_cb(lv_anim_t *a);

/* Widget factories */
static lv_obj_t *make_screen(void);
static lv_obj_t *make_top_bar(lv_obj_t *parent, const char *title,
                               bool show_back, lv_event_cb_t back_cb);
static lv_obj_t *make_round_btn(lv_obj_t *parent, const char *text,
                                lv_color_t bg, lv_color_t fg,
                                lv_coord_t w, lv_coord_t h,
                                lv_obj_t **label_out);
static lv_obj_t *make_icon_btn(lv_obj_t *parent, const char *symbol,
                                lv_coord_t size);
static void      set_label_style(lv_obj_t *lbl, const lv_font_t *font,
                                  uint32_t color_hex);

/* Volume display helper (ISSUE-D FIX) */
static void update_vol_display(uint8_t volume);
static void update_sleep_visual(spod_sleep_state_t state);
static void format_time_mmss(uint16_t sec, char *out, size_t out_size);
static bool make_lvgl_fs_path(const char *sd_path, char *out, size_t out_size);
static void normalize_ext_lower(const char *in, char *out, size_t out_size);
static bool can_open_image(const char *fs_path, lv_img_header_t *out_info);
static bool load_cover_image(lv_obj_t *img, lv_obj_t *frame, const char *sd_path);
static void status_bar_update_all(void);
static void status_bar_timer_cb(lv_timer_t *t);

/* Chapter list */
static void rebuild_chapter_list(const spod_app_state_t *state);

/* Library */
static void cb_card_tap(lv_event_t *e);
static void cb_library_settings_tap(lv_event_t *e);

/* Detail */
static void cb_detail_back(lv_event_t *e);
static void cb_detail_play(lv_event_t *e);
static void cb_detail_resume(lv_event_t *e);
static void cb_detail_chapters(lv_event_t *e);

/* Now Playing */
static void cb_np_back(lv_event_t *e);
static void cb_np_playpause(lv_event_t *e);
static void cb_np_prev(lv_event_t *e);
static void cb_np_next(lv_event_t *e);
static void cb_np_vol_slider_changed(lv_event_t *e);
static void cb_np_vol_minus(lv_event_t *e);
static void cb_np_vol_plus(lv_event_t *e);
static void cb_np_sleep(lv_event_t *e);
static void cb_np_chapters_open(lv_event_t *e);

/* Chapters sheet */
static void cb_ch_close(lv_event_t *e);
static void cb_ch_item(lv_event_t *e);

/* Settings */
static void settings_draft_reset(const spod_app_state_t *state);
static void cb_settings_back(lv_event_t *e);
static void cb_settings_system(lv_event_t *e);
static void cb_settings_display(lv_event_t *e);
static void cb_settings_audio(lv_event_t *e);
static void cb_settings_wifi_menu(lv_event_t *e);
static void cb_settings_sub_back(lv_event_t *e);
static void cb_settings_language(lv_event_t *e);
static void cb_settings_idle_changed(lv_event_t *e);
static void cb_settings_brightness_changed(lv_event_t *e);
static void cb_settings_min_brightness_changed(lv_event_t *e);
static void cb_settings_dim_changed(lv_event_t *e);
static void cb_settings_startup_volume_changed(lv_event_t *e);
static void cb_settings_startup_sound_toggle(lv_event_t *e);
static void cb_settings_shutdown_sound_toggle(lv_event_t *e);
static void cb_settings_volume_changed(lv_event_t *e);
static void cb_settings_tone_toggle(lv_event_t *e);
static void cb_settings_bass_changed(lv_event_t *e);
static void cb_settings_treble_changed(lv_event_t *e);
static void cb_settings_mono_toggle(lv_event_t *e);
static void cb_settings_sound_enricher_toggle(lv_event_t *e);
static void cb_settings_sound_enricher_changed(lv_event_t *e);
static void cb_settings_wifi(lv_event_t *e);
static void cb_settings_wifi_scan(lv_event_t *e);
static void cb_settings_wifi_ap(lv_event_t *e);
static void cb_settings_wifi_password_cancel(lv_event_t *e);
static void cb_settings_wifi_password_connect(lv_event_t *e);
static void cb_settings_wifi_forget(lv_event_t *e);
static bool wifi_async_start(const ui_wifi_job_t *job, ui_wifi_state_t busy_state, const char *busy_msg);
static void wifi_async_apply_enabled(bool enabled);

/* ================================================================== */
/*  Animation wrappers                                                  */
/* ================================================================== */

/*
 * BUG-02 FIX: lv_obj_set_y signature is void(lv_obj_t*, lv_coord_t).
 * lv_anim_exec_xcb_t is void(*)(void*, int32_t). When lv_coord_t is
 * int16_t (valid LVGL v8 config), a direct cast is undefined behaviour.
 * This wrapper has the correct signature and casts internally.
 */
static void sheet_anim_y_cb(void *obj, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)y);
}

static void toast_anim_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)opa,
                          LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void toast_anim_ready_cb(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

/* ================================================================== */
/*  Generic widget factories                                            */
/* ================================================================== */

static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    /*
     * lv_obj_create(NULL) creates a screen automatically sized to the
     * attached display. We do not call lv_obj_set_size() to avoid
     * conflicts if display resolution changes after lv_init().
     */
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG_DARK),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static void status_bar_format_clock(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    time_t now = 0;
    time(&now);
    struct tm tm_now;
    memset(&tm_now, 0, sizeof(tm_now));
    localtime_r(&now, &tm_now);

    if ((tm_now.tm_year + 1900) < 2024) {
        strlcpy(out, "--:--", out_size);
        return;
    }

    snprintf(out, out_size, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
}

static void status_bar_register(lv_obj_t *title_lbl,
                                lv_obj_t *clock_lbl,
                                lv_obj_t *wifi_lbl,
                                lv_obj_t *wifi_slash,
                                lv_obj_t *bt_lbl,
                                lv_obj_t *airplay_lbl)
{
    if (s_status_bar_count >= SCREEN_COUNT) {
        return;
    }

    s_status_bars[s_status_bar_count++] = (ui_status_bar_t) {
        .title_lbl = title_lbl,
        .clock_lbl = clock_lbl,
        .wifi_lbl = wifi_lbl,
        .wifi_slash = wifi_slash,
        .bt_lbl = bt_lbl,
        .airplay_lbl = airplay_lbl,
    };

    if (!s_status_bar_timer) {
        s_status_bar_timer = lv_timer_create(status_bar_timer_cb, 1000, NULL);
    }
    status_bar_update_all();
}

static void status_bar_set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void status_bar_update_all(void)
{
    const spod_app_state_t *state = ui_app_get_state();
    const bool wifi_active = (state && state->wifi_enabled) ||
                             audio_backend_wifi_upload_is_running() ||
                             audio_backend_wifi_upload_is_connected();
    const bool bt_active = state && state->bluetooth_enabled;
    const bool airplay_active = state && state->airplay_enabled;

    char clock_buf[8];
    status_bar_format_clock(clock_buf, sizeof(clock_buf));

    for (uint8_t i = 0; i < s_status_bar_count; ++i) {
        ui_status_bar_t *bar = &s_status_bars[i];
        if (bar->clock_lbl) {
            lv_label_set_text(bar->clock_lbl, clock_buf);
        }
        if (bar->wifi_lbl) {
            lv_obj_set_style_text_color(bar->wifi_lbl,
                                        lv_color_hex(wifi_active ? COL_TEXT_PRIMARY : COL_TEXT_SECONDARY),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        status_bar_set_hidden(bar->wifi_slash, wifi_active);
        status_bar_set_hidden(bar->bt_lbl, !bt_active);
        status_bar_set_hidden(bar->airplay_lbl, !airplay_active);
    }
}

static void status_bar_timer_cb(lv_timer_t *t)
{
    (void)t;
    status_bar_update_all();
}

static lv_obj_t *make_top_bar(lv_obj_t *parent, const char *title,
                               bool show_back, lv_event_cb_t back_cb)
{
    static const lv_point_t wifi_slash_pts[] = {
        {0, 18},
        {22, 0},
    };

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, DISPLAY_W, TOP_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x241A18),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    if (show_back && back_cb) {
        lv_obj_t *btn = lv_btn_create(bar);
        lv_obj_set_size(btn, 56, 56);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BTN_SECONDARY),
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(lbl, FONT_SUBTITLE,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_PRIMARY),
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(lbl);
    }
    else {
        lv_obj_t *settings_btn = lv_btn_create(bar);
        lv_obj_set_size(settings_btn, 46, 46);
        lv_obj_align(settings_btn, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_bg_opa(settings_btn, LV_OPA_TRANSP,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(settings_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(settings_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(settings_btn, cb_library_settings_tap, LV_EVENT_CLICKED, NULL);

        lv_obj_t *settings_lbl = lv_label_create(settings_btn);
        lv_label_set_text(settings_lbl, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_font(settings_lbl, FONT_STATUS_ICON,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(settings_lbl, lv_color_hex(COL_TEXT_PRIMARY),
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(settings_lbl);
    }

    lv_obj_t *title_lbl = lv_label_create(bar);
    lv_label_set_text(title_lbl, (title && title[0] != '\0') ? title : "Basmio");
    set_label_style(title_lbl, show_back ? FONT_SUBTITLE : FONT_TITLE, COL_TEXT_PRIMARY);
    lv_obj_set_width(title_lbl, show_back ? 134 : 90);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, show_back ? 76 : 100, 0);

    lv_obj_t *clock_lbl = lv_label_create(bar);
    lv_label_set_text(clock_lbl, "--:--");
    set_label_style(clock_lbl, FONT_SUBTITLE, COL_TEXT_SECONDARY);
    lv_obj_set_width(clock_lbl, 96);
    lv_obj_set_style_text_align(clock_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(clock_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *airplay_lbl = lv_label_create(bar);
    lv_label_set_text(airplay_lbl, "AP");
    set_label_style(airplay_lbl, FONT_SUBTITLE, COL_TEXT_PRIMARY);
    lv_obj_align(airplay_lbl, LV_ALIGN_RIGHT_MID, -92, 0);
    lv_obj_add_flag(airplay_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bt_lbl = lv_label_create(bar);
    lv_label_set_text(bt_lbl, LV_SYMBOL_BLUETOOTH);
    set_label_style(bt_lbl, FONT_SUBTITLE, COL_TEXT_PRIMARY);
    lv_obj_align(bt_lbl, LV_ALIGN_RIGHT_MID, -58, 0);
    lv_obj_add_flag(bt_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *wifi_lbl = lv_label_create(bar);
    lv_label_set_text(wifi_lbl, LV_SYMBOL_WIFI);
    set_label_style(wifi_lbl, show_back ? FONT_SUBTITLE : FONT_STATUS_ICON, COL_TEXT_SECONDARY);
    lv_obj_align(wifi_lbl, show_back ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID,
                 show_back ? -22 : 56, 0);

    lv_obj_t *wifi_slash = lv_line_create(bar);
    lv_line_set_points(wifi_slash, wifi_slash_pts, 2);
    lv_obj_set_size(wifi_slash, 24, 20);
    lv_obj_set_style_line_width(wifi_slash, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(wifi_slash, lv_color_hex(COL_TEXT_SECONDARY), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(wifi_slash, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align_to(wifi_slash, wifi_lbl, LV_ALIGN_CENTER, 1, 0);

    status_bar_register(title_lbl, clock_lbl, wifi_lbl, wifi_slash, bt_lbl, airplay_lbl);

    return bar;
}

/*
 * BUG-06 FIX: label_out receives the inner label pointer so callers never
 * need to query child indices. Pass NULL if the label reference is not needed.
 */
static lv_obj_t *make_round_btn(lv_obj_t *parent, const char *text,
                                 lv_color_t bg, lv_color_t fg,
                                 lv_coord_t w, lv_coord_t h,
                                 lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, BTN_RADIUS, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, fg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lbl, FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);

    if (label_out) *label_out = lbl;
    return btn;
}

static lv_obj_t *make_icon_btn(lv_obj_t *parent, const char *symbol,
                                lv_coord_t size)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2D2722),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x40372F),
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_PRIMARY), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lbl, FONT_SUBTITLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);
    return btn;
}

static void set_label_style(lv_obj_t *lbl, const lv_font_t *font,
                             uint32_t color_hex)
{
    if (font) {
        lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    lv_obj_set_style_text_color(lbl, lv_color_hex(color_hex),
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
}

/*
 * ISSUE-D FIX: single function updates both slider position and numeric
 * label. Previously three callback functions each duplicated this logic.
 */
static void update_vol_display(uint8_t volume)
{
    if (!s_np_vol_slider || !s_np_vol_lbl) return;
    lv_slider_set_value(s_np_vol_slider, (int32_t)volume, LV_ANIM_OFF);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)volume);
    lv_label_set_text(s_np_vol_lbl, buf);
}

static void update_sleep_visual(spod_sleep_state_t state)
{
    if (!s_np_sleep_btn || !s_np_sleep_arc || !s_np_sleep_power_lbl) {
        return;
    }

    if (state == SLEEP_OFF) {
        lv_obj_add_flag(s_np_sleep_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_np_sleep_power_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_np_sleep_power_lbl, lv_color_hex(COL_TEXT_HINT),
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        return;
    }

    int32_t remaining_pct = 0;
    switch (state) {
        case SLEEP_15: remaining_pct = 75; break; /* missing quarter */
        case SLEEP_30: remaining_pct = 50; break; /* missing half */
        case SLEEP_45: remaining_pct = 25; break; /* missing 3/4 */
        case SLEEP_60: remaining_pct = 0;  break; /* missing all */
        default: remaining_pct = 0; break;
    }

    lv_obj_add_flag(s_np_sleep_power_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_np_sleep_arc, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(s_np_sleep_arc, remaining_pct);
}

static void format_time_mmss(uint16_t sec, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    uint16_t m = (uint16_t)(sec / 60u);
    uint16_t s = (uint16_t)(sec % 60u);
    snprintf(out, out_size, "%u:%02u", (unsigned)m, (unsigned)s);
}

static bool make_lvgl_fs_path(const char *sd_path, char *out, size_t out_size)
{
    if (!sd_path || !out || out_size < 4) return false;
    if (strlen(sd_path) + 3 > out_size) return false;
    out[0] = 'S';
    out[1] = ':';
    strlcpy(out + 2, sd_path, out_size - 2);
    return true;
}

static void normalize_ext_lower(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) {
        return;
    }
    strlcpy(out, in, out_size);

    char *last_slash = strrchr(out, '/');
    char *dot = strrchr(out, '.');
    if (!dot || (last_slash && dot < last_slash)) {
        return;
    }
    for (char *p = dot + 1; *p != '\0'; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
}

static bool can_open_image(const char *fs_path, lv_img_header_t *out_info)
{
    if (!fs_path) {
        return false;
    }

    lv_fs_file_t f;
    if (lv_fs_open(&f, fs_path, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        ESP_LOGW(TAG, "Cover open failed: %s", fs_path);
        return false;
    }
    (void)lv_fs_close(&f);

    lv_img_header_t info;
    if (lv_img_decoder_get_info(fs_path, &info) != LV_RES_OK) {
        ESP_LOGW(TAG, "Cover decoder rejected: %s", fs_path);
        return false;
    }

    if (out_info) {
        *out_info = info;
    }
    return true;
}

static bool load_cover_image(lv_obj_t *img, lv_obj_t *frame, const char *sd_path)
{
    (void)img;
    (void)frame;
    (void)sd_path;
    return false;

    if (!img || !frame || !sd_path || sd_path[0] == '\0') {
        return false;
    }

    char fs_path_raw[196];
    char fs_path_ext_norm[196];
    if (!make_lvgl_fs_path(sd_path, fs_path_raw, sizeof(fs_path_raw))) {
        return false;
    }

    normalize_ext_lower(fs_path_raw, fs_path_ext_norm, sizeof(fs_path_ext_norm));

    const char *candidates[2] = { fs_path_raw, fs_path_ext_norm };
    for (size_t i = 0; i < 2; i++) {
        if (i > 0 && strcmp(candidates[i], candidates[0]) == 0) {
            continue;
        }
        lv_img_header_t info;
        if (!can_open_image(candidates[i], &info)) {
            continue;
        }

        lv_img_set_src(img, candidates[i]);
        lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);

        (void)info;
        /*
         * Keep zoom disabled for smoother UI and to avoid decoder-specific
         * transform artifacts (white frames observed on some JPG/PNG paths).
         */
        lv_img_set_zoom(img, LV_IMG_ZOOM_NONE);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        return true;
    }

    ESP_LOGW(TAG, "Cover decode failed for %s", sd_path);
    return false;
}

/* ================================================================== */
/*  LIBRARY SCREEN                                                      */
/* ================================================================== */

typedef struct {
    uint8_t story_index;
} card_cb_data_t;

/* File-scope: stable addresses for the lifetime of the application. */
static card_cb_data_t s_card_cb_data[SPOD_MAX_STORIES];

static void cb_card_tap(lv_event_t *e)
{
    const card_cb_data_t *d = (const card_cb_data_t *)lv_event_get_user_data(e);
    ui_app_select_story(d->story_index);
    ui_app_navigate_to(SCREEN_STORY_DETAIL, false);
}

static void cb_library_settings_tap(lv_event_t *e)
{
    (void)e;
    settings_draft_reset(ui_app_get_state());
    ui_app_navigate_to(SCREEN_SETTINGS, false);
}

void ui_screen_library_create(const spod_app_state_t *state)
{
    (void)state;

    g_scr_library = make_screen();
    lv_obj_set_style_bg_color(g_scr_library, lv_color_hex(0xF3F5F7),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_scr_library, LV_OPA_COVER,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    make_top_bar(g_scr_library, "Library", false, NULL);

    lv_obj_t *list = lv_obj_create(g_scr_library);
    lv_obj_set_size(list, DISPLAY_W, DISPLAY_H - TOP_BAR_H);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xF3F5F7), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(list, CARD_SIDE_PAD, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(list, CARD_SIDE_PAD, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(list, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(list, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(list, CARD_ROW_GAP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    uint8_t story_count = ui_app_story_count();
    for (uint8_t i = 0; i < story_count; i++) {
        const spod_story_t *story = ui_app_story_get(i);
        if (!story) {
            continue;
        }

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, CARD_W, CARD_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(row, lv_color_hex(0xD9DEE5), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(row, CARD_RADIUS, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *accent = lv_obj_create(row);
        lv_obj_set_size(accent, 5, CARD_H);
        lv_obj_align(accent, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(accent, lv_color_hex(story->accent_color), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(accent, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title_lbl = lv_label_create(row);
        lv_label_set_text(title_lbl, story->title);
        lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title_lbl, CARD_W - 150);
        set_label_style(title_lbl, FONT_SUBTITLE, 0xFF1A1612u);
        lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 22, 0);

        lv_obj_t *dur_lbl = lv_label_create(row);
        lv_label_set_text(dur_lbl, story->duration_label);
        set_label_style(dur_lbl, FONT_TINY, 0xFF6B7280u);
        lv_obj_align(dur_lbl, LV_ALIGN_RIGHT_MID, -34, -10);

        lv_obj_t *ch_lbl = lv_label_create(row);
        char ch_text[24];
        snprintf(ch_text, sizeof(ch_text), "%u ch", (unsigned)story->chapter_count);
        lv_label_set_text(ch_lbl, ch_text);
        set_label_style(ch_lbl, FONT_TINY, 0xFF374151u);
        lv_obj_align(ch_lbl, LV_ALIGN_RIGHT_MID, -34, 14);

        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        set_label_style(arrow, FONT_SMALL, 0xFF6B7280u);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);

        s_card_cb_data[i].story_index = i;
        lv_obj_add_event_cb(row, cb_card_tap, LV_EVENT_CLICKED, (void *)&s_card_cb_data[i]);
        ui_build_yield();
    }

}
/* ================================================================== */
/*  STORY DETAIL SCREEN                                                 */
/* ================================================================== */

/*
 * ISSUE-B FIX: Info labels within the detail card use fixed y offsets
 * from the card's top-left (after its 20px padding). Previously they
 * used lv_obj_align_to() chains: author aligned OUT_BOTTOM of title,
 * divider OUT_BOTTOM of author, etc. In LVGL v8 those stored alignments
 * do not reflow when dynamic text changes the reference object's height
 * (e.g. a long story title wrapping to 2 lines). Fixed y values are
 * immune to this because they do not depend on sibling dimensions.
 *
 * Layout map inside the 20px-padded card (card content y=0 is the inner top):
 *   y=  0  Story title (up to 2 lines, ~48px max)
 *   y= 54  Author
 *   y= 80  Divider (1px line)
 *   y= 98  "Length" key label
 *   y= 98  Duration value label (right-aligned)
 *   y=124  "Chapters" key label
 *   y=124  Chapter count value label (right-aligned)
 */
#define DET_Y_TITLE    0
#define DET_Y_AUTHOR  54
#define DET_Y_DIV     80
#define DET_Y_LEN     98
#define DET_Y_CH     124

static void cb_detail_back(lv_event_t *e)
{
    (void)e;
    ui_app_navigate_to(SCREEN_LIBRARY, true);
}

static void cb_detail_play(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    if (st->playback_state == PLAYBACK_STOPPED) {
        st->current_chapter = 0;
        st->elapsed_sec     = 0;
        ui_app_select_chapter(0);
    } else if (st->playback_state == PLAYBACK_PAUSED) {
        /* "Continue" picks up where it paused instead of restarting the chapter. */
        ui_app_toggle_playback();
    }
    st->chapters_open  = false;
    ui_app_navigate_to(SCREEN_NOW_PLAYING, false);
}

static void cb_detail_resume(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    if (!ui_app_selected_story_resume(NULL, NULL)) {
        return;
    }
    ui_app_resume_selected_story();
    st->chapters_open = false;
    ui_app_navigate_to(SCREEN_NOW_PLAYING, false);
}

static void cb_detail_chapters(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    if (st->playback_state == PLAYBACK_STOPPED) {
        ui_app_select_chapter(st->current_chapter);
    }
    /*
     * BUG-17 FIX: set chapters_open before navigate_to.
     * navigate_to() does not touch this field.
     */
    st->chapters_open = true;
    ui_app_navigate_to(SCREEN_NOW_PLAYING, false);
}

void ui_screen_detail_create(const spod_app_state_t *state)
{
    (void)state;

    g_scr_detail = make_screen();

    make_top_bar(g_scr_detail, "Story", true, cb_detail_back);

    /* ---------- Artwork band ---------- */
    s_det_accent_bar = lv_obj_create(g_scr_detail);
    lv_obj_set_size(s_det_accent_bar, DISPLAY_W, 200);
    lv_obj_align(s_det_accent_bar, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(s_det_accent_bar, lv_color_hex(COL_BG_CARD),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_det_accent_bar, LV_OPA_COVER,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_det_accent_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_det_accent_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(s_det_accent_bar, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_det_accent_bar,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_det_art_img = lv_img_create(s_det_accent_bar);
    lv_obj_center(s_det_art_img);
    lv_obj_add_flag(s_det_art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_det_art_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /*
     * BUG-01 FIX: dedicated static pointer s_det_art_lbl.
     * The first draft wrote s_det_title_label here then overwrote it below,
     * orphaning the artwork label and making it unupdatable from refresh().
     */
    s_det_art_lbl = lv_label_create(s_det_accent_bar);
    lv_label_set_text(s_det_art_lbl, "?");  /* refreshed on navigation */
    lv_obj_set_style_text_font(s_det_art_lbl, &lv_font_montserrat_48,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_det_art_lbl, lv_color_hex(COL_ACCENT),
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_det_art_lbl);

    /* ---------- Info card ---------- */
    /*
     * Card height: fills from y=272 to the button area.
     * Buttons float over the card as children of g_scr_detail at BOTTOM_MID.
     * Card does not need to contain the buttons; it is purely informational.
     */
    lv_obj_t *card = lv_obj_create(g_scr_detail);
    lv_obj_set_size(card, DISPLAY_W - 32, DISPLAY_H - 310);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_BG_CARD),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(card, CARD_RADIUS, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Card content width available = DISPLAY_W - 32 - 2*20 = DISPLAY_W - 72 */
    lv_coord_t content_w = DISPLAY_W - 72;

    /*
     * ISSUE-B FIX: All info labels use fixed y offsets from card inner-top.
     * This is immune to dynamic text reflowing the reference sibling's size.
     * The offsets are defined as DET_Y_* macros above this function.
     */

    /* Title */
    s_det_title_lbl = lv_label_create(card);
    lv_label_set_text(s_det_title_lbl, "\xe2\x80\x94"); /* em-dash placeholder */
    lv_label_set_long_mode(s_det_title_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_det_title_lbl, content_w);
    set_label_style(s_det_title_lbl, FONT_TITLE, COL_TEXT_PRIMARY);
    lv_obj_align(s_det_title_lbl, LV_ALIGN_TOP_LEFT, 0, DET_Y_TITLE);

    /* Author */
    s_det_author_lbl = lv_label_create(card);
    lv_label_set_text(s_det_author_lbl, "\xe2\x80\x94");
    set_label_style(s_det_author_lbl, FONT_BODY, COL_TEXT_SECONDARY);
    lv_obj_align(s_det_author_lbl, LV_ALIGN_TOP_LEFT, 0, DET_Y_AUTHOR);

    /* Divider */
    lv_obj_t *div = lv_obj_create(card);
    lv_obj_set_size(div, content_w, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(COL_BTN_SECONDARY),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(div, LV_ALIGN_TOP_LEFT, 0, DET_Y_DIV);

    /* Length row */
    lv_obj_t *len_key = lv_label_create(card);
    lv_label_set_text(len_key, "Length");
    set_label_style(len_key, FONT_SMALL, COL_TEXT_HINT);
    lv_obj_align(len_key, LV_ALIGN_TOP_LEFT, 0, DET_Y_LEN);

    s_det_dur_lbl = lv_label_create(card);
    lv_label_set_text(s_det_dur_lbl, "\xe2\x80\x94");
    set_label_style(s_det_dur_lbl, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(s_det_dur_lbl, LV_ALIGN_TOP_RIGHT, 0, DET_Y_LEN);

    /* Chapters row */
    lv_obj_t *ch_key = lv_label_create(card);
    lv_label_set_text(ch_key, "Chapters");
    set_label_style(ch_key, FONT_SMALL, COL_TEXT_HINT);
    lv_obj_align(ch_key, LV_ALIGN_TOP_LEFT, 0, DET_Y_CH);

    s_det_ch_lbl = lv_label_create(card);
    lv_label_set_text(s_det_ch_lbl, "\xe2\x80\x94");
    set_label_style(s_det_ch_lbl, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(s_det_ch_lbl, LV_ALIGN_TOP_RIGHT, 0, DET_Y_CH);

    /* ---------- Action buttons (children of screen, not card) ---------- */
    lv_obj_t *play_btn = make_round_btn(
        g_scr_detail, "Play",
        lv_color_hex(COL_BTN_PLAY), lv_color_hex(COL_BG_DARK),
        DISPLAY_W - 32, 58,
        &s_det_play_lbl);  /* BUG-06 FIX: label captured via out-param */
    lv_obj_align(play_btn, LV_ALIGN_BOTTOM_MID, 0, -144);
    lv_obj_add_event_cb(play_btn, cb_detail_play, LV_EVENT_CLICKED, NULL);

    s_det_resume_btn = make_round_btn(
        g_scr_detail, "Resume --:--",
        lv_color_hex(COL_BTN_SECONDARY), lv_color_hex(COL_TEXT_HINT),
        DISPLAY_W - 32, 52,
        &s_det_resume_lbl);
    lv_obj_align(s_det_resume_btn, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_add_event_cb(s_det_resume_btn, cb_detail_resume, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ch_btn = make_round_btn(
        g_scr_detail, LV_SYMBOL_LIST "  Chapters",
        lv_color_hex(COL_BTN_SECONDARY), lv_color_hex(COL_TEXT_PRIMARY),
        DISPLAY_W - 32, 52,
        NULL);
    lv_obj_align(ch_btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(ch_btn, cb_detail_chapters, LV_EVENT_CLICKED, NULL);
}

void ui_screen_detail_refresh(const spod_app_state_t *state)
{
    if (!state->selected_story) return;
    const spod_story_t *s = state->selected_story;

    /* Tint artwork band with story colour */
    lv_obj_set_style_bg_color(s_det_accent_bar, lv_color_hex(s->accent_color),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_det_accent_bar, 55, LV_PART_MAIN | LV_STATE_DEFAULT);

    bool detail_cover_loaded = false;
    if (s->album_art_path && s->album_art_path[0]) {
        detail_cover_loaded = load_cover_image(s_det_art_img, s_det_accent_bar, s->album_art_path);
        if (detail_cover_loaded) {
            lv_obj_clear_flag(s_det_art_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_det_art_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!detail_cover_loaded) {
        char initial[2] = { s->title[0], '\0' };
        lv_label_set_text(s_det_art_lbl, initial);
        lv_obj_set_style_text_color(s_det_art_lbl, lv_color_hex(s->accent_color),
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(s_det_art_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_det_art_img, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_det_title_lbl,  s->title);
    lv_label_set_text(s_det_author_lbl, s->author);
    lv_label_set_text(s_det_dur_lbl,    s->duration_label);

    char ch_buf[8];
    snprintf(ch_buf, sizeof(ch_buf), "%u", (unsigned)s->chapter_count);
    lv_label_set_text(s_det_ch_lbl, ch_buf);

    /*
     * ISSUE-K FIX: removed redundant `state->selected_story == s` test.
     * s IS state->selected_story by definition. The comparison was always
     * true and served no filtering purpose.
     */
    lv_label_set_text(s_det_play_lbl,
        (state->playback_state != PLAYBACK_STOPPED) ? "Continue" : "Play");

    uint8_t resume_chapter = 0;
    uint32_t resume_sec = 0;
    bool has_resume = ui_app_selected_story_resume(&resume_chapter, &resume_sec);
    if (has_resume) {
        uint32_t mm = resume_sec / 60u;
        uint32_t ss = resume_sec % 60u;
        char resume_buf[28];
        snprintf(resume_buf, sizeof(resume_buf), "Resume %u:%02u", (unsigned)mm, (unsigned)ss);
        lv_label_set_text(s_det_resume_lbl, resume_buf);
        lv_obj_set_style_bg_color(s_det_resume_btn, lv_color_hex(COL_BTN_SECONDARY),
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_det_resume_btn, LV_OPA_COVER,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_det_resume_lbl, lv_color_hex(COL_TEXT_PRIMARY),
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_flag(s_det_resume_btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_label_set_text(s_det_resume_lbl, "Resume --:--");
        lv_obj_set_style_bg_color(s_det_resume_btn, lv_color_hex(0xFF2B3138),
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_det_resume_btn, LV_OPA_70,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_det_resume_lbl, lv_color_hex(COL_TEXT_HINT),
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(s_det_resume_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ================================================================== */
/*  CHAPTERS BOTTOM SHEET                                               */
/* ================================================================== */

static void cb_ch_close(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    st->chapters_open = false;
    ui_chapters_sheet_set_visible(false, st);
}

static void cb_ch_item(lv_event_t *e)
{
    /*
     * BUG-04 FIX: user_data is &s_ch_idx_data[i] ??? a real pointer to a
     * file-scope uint8_t. Dereference it; never cast pointer-as-integer.
     */
    const uint8_t *idx_ptr = (const uint8_t *)lv_event_get_user_data(e);
    uint8_t idx = *idx_ptr;

    spod_app_state_t *st = ui_app_get_state();
    ui_app_select_chapter(idx);

    /*
     * Close the sheet before calling refresh() so that refresh() sees
     * chapters_open=false and does not call rebuild_chapter_list() again.
     */
    st->chapters_open = false;
    ui_chapters_sheet_set_visible(false, st);
    ui_screen_now_playing_refresh(st);
}

/*
 * ISSUE-A FIX: Chapter list dirty guard.
 *
 * rebuild_chapter_list() is called inside ui_chapters_sheet_set_visible(true),
 * which is in turn called by ui_screen_now_playing_refresh() whenever
 * chapters_open=true. Without a guard, operations like prev/next/playpause
 * while the sheet is visible would destroy and recreate the entire list
 * on every call ??? causing visual flicker and invalidating any held pointers.
 *
 * The guard tracks which story and chapter the list was last built for.
 * A rebuild is only triggered when either has changed, or on first open.
 * The story pointer comparison is safe because we never free stories;
 * they are either ROM literals or heap that persists until SD eject.
 */
static void rebuild_chapter_list(const spod_app_state_t *state)
{
    if (!s_ch_list || !state->selected_story) return;

    /* Skip rebuild if list already reflects the current story and chapter */
    if (s_ch_built_for_story   == state->selected_story &&
        s_ch_built_for_chapter == state->current_chapter) {
        return;
    }

    lv_obj_clean(s_ch_list);

    const spod_story_t *story = state->selected_story;
    uint8_t count = story->chapter_count;
    if (count > SPOD_MAX_CHAPTERS) count = SPOD_MAX_CHAPTERS;

    for (uint8_t i = 0; i < count; i++) {
        const spod_chapter_t *ch = &story->chapters[i];

        /*
         * ISSUE-E FIX: use loop index i for display numbering, not ch->index.
         * ch->index is data from the model and could diverge from the loop
         * counter if chapters are loaded out of order from SD. The loop
         * counter is always the correct display position.
         */
        char row_text[72];
        snprintf(row_text, sizeof(row_text), "%u  %s",
                 (unsigned)(i + 1), ch->title);

        lv_obj_t *btn = lv_list_add_btn(s_ch_list, NULL, row_text);
        lv_obj_set_style_bg_color(btn,
            (i == state->current_chapter)
                ? lv_color_hex(COL_ACCENT_DIM)
                : lv_color_hex(COL_BG_SHEET),
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_min_height(btn, 54, LV_PART_MAIN | LV_STATE_DEFAULT);

        /*
         * BUG-05 FIX: lv_list_get_btn_label() is the documented v8 API
         * for accessing a list button's text label. Child index 0 is an
         * undocumented implementation detail that varies with icon presence.
         */
        lv_obj_t *text_lbl = lv_obj_get_child(btn, 0);
        //lv_obj_t *text_lbl = lv_list_get_btn_label(btn);   /* NOT lv_obj_get_child(btn, 0) */
        if (text_lbl) {
            set_label_style(text_lbl, FONT_BODY,
                            (i == state->current_chapter)
                                ? COL_ACCENT : COL_TEXT_PRIMARY);
        }

        /* Right-aligned duration */
        uint16_t dur = ch->duration_sec;
        char dur_text[12];
        snprintf(dur_text, sizeof(dur_text), "%u:%02u",
                 (unsigned)(dur / 60u), (unsigned)(dur % 60u));
        lv_obj_t *dur_lbl = lv_label_create(btn);
        lv_label_set_text(dur_lbl, dur_text);
        set_label_style(dur_lbl, FONT_SMALL, COL_TEXT_HINT);
        lv_obj_align(dur_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

        /* BUG-04 FIX: stable file-scope pointer as userdata */
        s_ch_idx_data[i] = i;
        lv_obj_add_event_cb(btn, cb_ch_item, LV_EVENT_CLICKED,
                            (void *)&s_ch_idx_data[i]);
    }

    /* Record what we built so the dirty check above can short-circuit */
    s_ch_built_for_story   = state->selected_story;
    s_ch_built_for_chapter = state->current_chapter;
}

void ui_chapters_sheet_set_visible(bool visible, const spod_app_state_t *state)
{
    if (!s_ch_sheet) return;
    bool is_hidden = lv_obj_has_flag(s_ch_sheet, LV_OBJ_FLAG_HIDDEN);

    if (visible) {
        /*
         * rebuild_chapter_list() now has a dirty guard: it only rebuilds
         * when story or chapter has changed. If already built for the same
         * state, the lv_obj_clean() + recreation is skipped entirely.
         */
        rebuild_chapter_list(state);

        /* Already visible: keep sheet open and never restart bring-up animation. */
        if (!is_hidden) {
            return;
        }

        lv_anim_del(s_ch_sheet, sheet_anim_y_cb);
        lv_obj_clear_flag(s_ch_sheet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(s_ch_sheet, DISPLAY_H);

        /*
         * BUG-02 FIX: sheet_anim_y_cb wrapper avoids UB cast of lv_obj_set_y.
         * lv_anim_t is stack-local; LVGL v8 copies it internally in
         * lv_anim_start(), so this is safe.
         */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_ch_sheet);
        lv_anim_set_exec_cb(&a, sheet_anim_y_cb);
        lv_anim_set_values(&a, DISPLAY_H, DISPLAY_H - CH_SHEET_H);
        lv_anim_set_time(&a, 280);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        if (is_hidden) {
            return;
        }

        lv_anim_del(s_ch_sheet, sheet_anim_y_cb);
        /* Hide immediately; do not animate - avoids blocking touch input */
        lv_obj_add_flag(s_ch_sheet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(s_ch_sheet, DISPLAY_H);
        /*
         * Invalidate the dirty flag so the next open always rebuilds with
         * fresh highlight state even if story/chapter have not changed.
         */
        s_ch_built_for_chapter = 0xFF;
    }
}
/* ================================================================== */
/*  NOW PLAYING SCREEN                                                  */
/* ================================================================== */

static void cb_np_back(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    if (st->chapters_open) {
        st->chapters_open = false;
        ui_chapters_sheet_set_visible(false, st);
        return;
    }
    /* Background play: leaving Now Playing keeps the story playing. */
    ui_app_navigate_to(SCREEN_LIBRARY, true);   /* BUG-20 FIX */
}

static void cb_np_playpause(lv_event_t *e)
{
    (void)e;
    ui_app_toggle_playback();
}

static void cb_np_prev(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    if (!st->selected_story) return;
    if (st->current_chapter > 0) {
        ui_app_select_chapter((uint8_t)(st->current_chapter - 1u));
        ui_screen_now_playing_refresh(st);
    }
}

static void cb_np_next(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    if (!st->selected_story) return;
    if (st->current_chapter + 1u < st->selected_story->chapter_count) {
        ui_app_select_chapter((uint8_t)(st->current_chapter + 1u));
        ui_screen_now_playing_refresh(st);
    }
}

static void cb_np_vol_slider_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    if (val < 0)   val = 0;
    if (val > 100) val = 100;
    ui_app_set_volume((uint8_t)val);
    /*
     * ISSUE-D FIX: update_vol_display() is the sole place that writes
     * both the slider position and the numeric label. The slider widget
     * already reflects val (user dragged it), but we call the helper for
     * the label so all three code paths converge here.
     */
    update_vol_display((uint8_t)val);
}

static void cb_np_vol_minus(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    uint8_t v = (st->volume >= 5u) ? (uint8_t)(st->volume - 5u) : 0u;
    ui_app_set_volume(v);
    update_vol_display(v);   /* ISSUE-D FIX */
}

static void cb_np_vol_plus(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    uint8_t v = (st->volume <= 95u) ? (uint8_t)(st->volume + 5u) : 100u;
    ui_app_set_volume(v);
    update_vol_display(v);   /* ISSUE-D FIX */
}

static void cb_np_sleep(lv_event_t *e)
{
    (void)e;
    ui_app_cycle_sleep_timer();
}

static void cb_np_chapters_open(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *st = ui_app_get_state();
    st->chapters_open = true;
    ui_chapters_sheet_set_visible(true, st);
}

void ui_screen_now_playing_create(const spod_app_state_t *state)
{
    (void)state;

    g_scr_now_playing = make_screen();
    lv_obj_set_style_bg_color(g_scr_now_playing, lv_color_hex(0x16120F),
                               LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Top bar */
    lv_obj_t *top_bar = make_top_bar(g_scr_now_playing, "Now Playing", true, cb_np_back);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x211917),
                               LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ---------- Artwork placeholder ---------- */
    s_np_art_box = lv_obj_create(g_scr_now_playing);
    lv_obj_set_size(s_np_art_box, 88, 88);
    lv_obj_align(s_np_art_box, LV_ALIGN_TOP_LEFT, 18, 74);
    lv_obj_set_style_bg_color(s_np_art_box, lv_color_hex(0x27211C),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_np_art_box, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_np_art_box, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_np_art_box, lv_color_hex(0x40372F),
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_np_art_box, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(s_np_art_box, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_np_art_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_np_art_img = lv_img_create(s_np_art_box);
    lv_obj_center(s_np_art_img);
    lv_obj_add_flag(s_np_art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_np_art_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_np_art_lbl = lv_label_create(s_np_art_box);
    lv_label_set_text(s_np_art_lbl, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(s_np_art_lbl, &lv_font_montserrat_32,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_np_art_lbl, lv_color_hex(0xDCA24A),
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_np_art_lbl);

    /* ---------- Story title ---------- */
    s_np_title_lbl = lv_label_create(g_scr_now_playing);
    lv_label_set_text(s_np_title_lbl, "\xe2\x80\x94");
    lv_label_set_long_mode(s_np_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_np_title_lbl, DISPLAY_W - 144);
    set_label_style(s_np_title_lbl, FONT_SUBTITLE, COL_TEXT_PRIMARY);
    lv_obj_align(s_np_title_lbl, LV_ALIGN_TOP_LEFT, 124, 74);

    /* ---------- Chapter label ---------- */
    s_np_chapter_lbl = lv_label_create(g_scr_now_playing);
    lv_label_set_text(s_np_chapter_lbl, "\xe2\x80\x94");
    lv_label_set_long_mode(s_np_chapter_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_np_chapter_lbl, DISPLAY_W - 144);
    set_label_style(s_np_chapter_lbl, FONT_BODY, COL_TEXT_SECONDARY);
    lv_obj_align(s_np_chapter_lbl, LV_ALIGN_TOP_LEFT, 124, 102);

    /* ---------- Playback progress ---------- */
    s_np_elapsed_lbl = lv_label_create(g_scr_now_playing);
    lv_label_set_text(s_np_elapsed_lbl, "0:00");
    set_label_style(s_np_elapsed_lbl, FONT_TINY, COL_TEXT_HINT);
    lv_obj_align(s_np_elapsed_lbl, LV_ALIGN_TOP_LEFT, 124, 132);

    s_np_total_lbl = lv_label_create(g_scr_now_playing);
    lv_label_set_text(s_np_total_lbl, "--:--");
    set_label_style(s_np_total_lbl, FONT_TINY, COL_TEXT_HINT);
    lv_obj_align(s_np_total_lbl, LV_ALIGN_TOP_RIGHT, -24, 132);

    s_np_prog_bar = lv_bar_create(g_scr_now_playing);
    lv_obj_set_size(s_np_prog_bar, DISPLAY_W - 162, 8);
    lv_obj_align(s_np_prog_bar, LV_ALIGN_TOP_LEFT, 124, 154);
    lv_bar_set_range(s_np_prog_bar, 0, 100);
    lv_bar_set_value(s_np_prog_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_np_prog_bar, lv_color_hex(0x332B25),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_np_prog_bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_np_prog_bar, lv_color_hex(0xDCA24A),
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_np_prog_bar, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_np_prog_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_np_prog_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    /* ---------- Transport: PREV | PLAY/PAUSE | NEXT ---------- */
    lv_obj_t *prev_btn = make_icon_btn(g_scr_now_playing, LV_SYMBOL_PREV, 50);
    lv_obj_align(prev_btn, LV_ALIGN_TOP_MID, -84, 172);
    lv_obj_add_event_cb(prev_btn, cb_np_prev, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pp_btn = lv_btn_create(g_scr_now_playing);
    lv_obj_set_size(pp_btn, 62, 62);
    lv_obj_align(pp_btn, LV_ALIGN_TOP_MID, 0, 166);
    lv_obj_set_style_bg_color(pp_btn, lv_color_hex(0xDCA24A),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(pp_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(pp_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(pp_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(pp_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(pp_btn, cb_np_playpause, LV_EVENT_CLICKED, NULL);

    s_np_playpause_lbl = lv_label_create(pp_btn);
    lv_label_set_text(s_np_playpause_lbl, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(s_np_playpause_lbl, lv_color_hex(0x16120F),
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_np_playpause_lbl, &lv_font_montserrat_20,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_np_playpause_lbl);

    lv_obj_t *next_btn = make_icon_btn(g_scr_now_playing, LV_SYMBOL_NEXT, 50);
    lv_obj_align(next_btn, LV_ALIGN_TOP_MID, 84, 172);
    lv_obj_add_event_cb(next_btn, cb_np_next, LV_EVENT_CLICKED, NULL);

    /* ---------- Chapters + Sleep row ---------- */
    lv_obj_t *ch_icon_btn = lv_btn_create(g_scr_now_playing);
    lv_obj_set_size(ch_icon_btn, 150, 38);
    lv_obj_align(ch_icon_btn, LV_ALIGN_TOP_MID, -82, 236);
    lv_obj_set_style_bg_color(ch_icon_btn, lv_color_hex(0x2D2722),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ch_icon_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ch_icon_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ch_icon_btn, 26, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ch_icon_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ch_icon_btn, cb_np_chapters_open, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ch_icon_lbl = lv_label_create(ch_icon_btn);
    lv_label_set_text(ch_icon_lbl, LV_SYMBOL_LIST);
    set_label_style(ch_icon_lbl, FONT_SUBTITLE, COL_TEXT_PRIMARY);
    lv_obj_align(ch_icon_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *ch_text_lbl = lv_label_create(ch_icon_btn);
    lv_label_set_text(ch_text_lbl, "Chapters");
    set_label_style(ch_text_lbl, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(ch_text_lbl, LV_ALIGN_LEFT_MID, 42, 0);

    s_np_sleep_btn = lv_btn_create(g_scr_now_playing);
    lv_obj_set_size(s_np_sleep_btn, 150, 38);
    lv_obj_align(s_np_sleep_btn, LV_ALIGN_TOP_MID, 82, 236);
    lv_obj_set_style_bg_color(s_np_sleep_btn, lv_color_hex(0x2D2722),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_np_sleep_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_np_sleep_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_np_sleep_btn, 26, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_np_sleep_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_np_sleep_btn, cb_np_sleep, LV_EVENT_CLICKED, NULL);

    s_np_sleep_arc = lv_arc_create(s_np_sleep_btn);
    lv_obj_set_size(s_np_sleep_arc, 24, 24);
    lv_arc_set_rotation(s_np_sleep_arc, 270);
    lv_arc_set_bg_angles(s_np_sleep_arc, 0, 360);
    lv_arc_set_range(s_np_sleep_arc, 0, 100);
    lv_arc_set_mode(s_np_sleep_arc, LV_ARC_MODE_NORMAL);
    lv_obj_remove_style(s_np_sleep_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_np_sleep_arc, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(s_np_sleep_arc, 3, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(s_np_sleep_arc, lv_color_hex(COL_TEXT_HINT),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(s_np_sleep_arc, lv_color_hex(COL_TEXT_PRIMARY),
                                 LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_np_sleep_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_np_sleep_arc, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_flag(s_np_sleep_arc, LV_OBJ_FLAG_HIDDEN);

    s_np_sleep_power_lbl = lv_label_create(s_np_sleep_btn);
    lv_label_set_text(s_np_sleep_power_lbl, LV_SYMBOL_POWER);
    set_label_style(s_np_sleep_power_lbl, FONT_SUBTITLE, COL_TEXT_HINT);
    lv_obj_align(s_np_sleep_power_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *sleep_text_lbl = lv_label_create(s_np_sleep_btn);
    lv_label_set_text(sleep_text_lbl, "Sleep");
    set_label_style(sleep_text_lbl, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(sleep_text_lbl, LV_ALIGN_LEFT_MID, 42, 0);

    update_sleep_visual(SLEEP_OFF);

    /* ---------- Volume section ---------- */
    lv_obj_t *vol_key = lv_label_create(g_scr_now_playing);
    lv_label_set_text(vol_key, LV_SYMBOL_VOLUME_MAX);
    set_label_style(vol_key, FONT_BODY, COL_TEXT_HINT);
    lv_obj_align(vol_key, LV_ALIGN_TOP_LEFT, 24, 282);

    s_np_vol_lbl = lv_label_create(g_scr_now_playing);
    lv_label_set_text(s_np_vol_lbl, "70");
    set_label_style(s_np_vol_lbl, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(s_np_vol_lbl, LV_ALIGN_TOP_RIGHT, -24, 282);

    lv_obj_t *minus_btn = make_icon_btn(g_scr_now_playing, LV_SYMBOL_MINUS, 34);
    lv_obj_align(minus_btn, LV_ALIGN_TOP_LEFT, 64, 278);
    lv_obj_add_event_cb(minus_btn, cb_np_vol_minus, LV_EVENT_CLICKED, NULL);

    s_np_vol_slider = lv_slider_create(g_scr_now_playing);
    lv_obj_set_size(s_np_vol_slider, DISPLAY_W - 194, 10);
    lv_obj_align(s_np_vol_slider, LV_ALIGN_TOP_MID, 0, 290);
    lv_slider_set_range(s_np_vol_slider, 0, 100);
    lv_slider_set_value(s_np_vol_slider, 70, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_np_vol_slider, lv_color_hex(0x332B25),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_np_vol_slider, LV_OPA_COVER,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_np_vol_slider, lv_color_hex(0xDCA24A),
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_np_vol_slider, LV_OPA_COVER,
                             LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_np_vol_slider, lv_color_hex(0xDCA24A),
                               LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_np_vol_slider, LV_OPA_COVER,
                             LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_np_vol_slider, 7, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_np_vol_slider, cb_np_vol_slider_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *plus_btn = make_icon_btn(g_scr_now_playing, LV_SYMBOL_PLUS, 34);
    lv_obj_align(plus_btn, LV_ALIGN_TOP_RIGHT, -64, 278);
    lv_obj_add_event_cb(plus_btn, cb_np_vol_plus, LV_EVENT_CLICKED, NULL);

    /* ---------- Toast ---------- */
    s_np_toast = lv_label_create(g_scr_now_playing);
    lv_label_set_text(s_np_toast, "");
    lv_obj_set_style_text_color(s_np_toast, lv_color_hex(COL_BG_DARK),
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_np_toast, FONT_BODY,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_np_toast, lv_color_hex(COL_ACCENT),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_np_toast, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_np_toast, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    /*
     * BUG-14 FIX: lv_obj_set_style_pad_ver / _pad_hor do not exist in all
     * LVGL v8 patch releases. Explicit pad_top/bottom/left/right used instead.
     */
    lv_obj_set_style_pad_top(s_np_toast,    8,  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(s_np_toast, 8,  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(s_np_toast,   16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(s_np_toast,  16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_np_toast, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(s_np_toast, LV_OBJ_FLAG_HIDDEN);

    /* ---------- Chapters bottom sheet ---------- */
    /*
     * Created as a child of g_scr_now_playing so it renders above all siblings.
     * Starts hidden at y=DISPLAY_H (off-screen below). Shown by
     * ui_chapters_sheet_set_visible() with a slide-up animation.
     */
    s_ch_sheet = lv_obj_create(g_scr_now_playing);
    lv_obj_set_size(s_ch_sheet, DISPLAY_W, CH_SHEET_H);
    lv_obj_set_pos(s_ch_sheet, 0, DISPLAY_H);
    lv_obj_set_style_bg_color(s_ch_sheet, lv_color_hex(COL_BG_SHEET),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_ch_sheet, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_ch_sheet, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_ch_sheet, SHEET_RADIUS, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_ch_sheet, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(s_ch_sheet, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_ch_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ch_sheet, LV_OBJ_FLAG_HIDDEN);

    /* Drag handle */
    lv_obj_t *handle = lv_obj_create(s_ch_sheet);
    lv_obj_set_size(handle, 44, 5);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(handle, lv_color_hex(COL_TEXT_HINT),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(handle, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(handle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Sheet header */
    lv_obj_t *sh_title = lv_label_create(s_ch_sheet);
    lv_label_set_text(sh_title, "Chapters");
    set_label_style(sh_title, &lv_font_montserrat_18, COL_TEXT_PRIMARY);
    lv_obj_align(sh_title, LV_ALIGN_TOP_LEFT, 20, 26);

    /* Close button */
    lv_obj_t *close_btn = lv_btn_create(s_ch_sheet);
    lv_obj_set_size(close_btn, 44, 44);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -8, 14);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(COL_BTN_SECONDARY),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(close_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(close_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(close_btn, cb_ch_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    set_label_style(close_lbl, FONT_BODY, COL_TEXT_SECONDARY);
    lv_obj_center(close_lbl);

    /* Chapter list ??? populated lazily by rebuild_chapter_list() */
    s_ch_list = lv_list_create(s_ch_sheet);
    lv_obj_set_size(s_ch_list, DISPLAY_W, CH_SHEET_H - CH_SHEET_HEADER_H);
    lv_obj_align(s_ch_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_ch_list, lv_color_hex(COL_BG_SHEET),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_ch_list, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_ch_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_ch_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_ch_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(s_ch_list, LV_SCROLLBAR_MODE_OFF);
}

void ui_screen_now_playing_refresh(const spod_app_state_t *state)
{
    if (!state->selected_story) return;
    const spod_story_t *story = state->selected_story;

    bool story_changed = (s_np_cached_story != story);
    bool chapter_changed = story_changed || (s_np_cached_chapter != state->current_chapter);

    if (story_changed) {
        lv_label_set_text(s_np_title_lbl, story->title);

        bool np_cover_loaded = false;
        if (story->album_art_path && story->album_art_path[0]) {
            np_cover_loaded = load_cover_image(s_np_art_img, s_np_art_box, story->album_art_path);
            if (np_cover_loaded) {
                lv_obj_clear_flag(s_np_art_img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_np_art_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (!np_cover_loaded) {
            lv_obj_set_style_text_color(s_np_art_lbl, lv_color_hex(story->accent_color),
                                         LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_clear_flag(s_np_art_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_np_art_img, LV_OBJ_FLAG_HIDDEN);
        }
        s_np_cached_story = story;
    }

    /*
     * Separator: the built-in Montserrat fonts only carry ASCII plus the LVGL
     * symbols, so U+00B7 rendered as a box. LV_SYMBOL_BULLET (U+2022) is in them.
     */
    const spod_chapter_t *ch = &story->chapters[state->current_chapter];
    if (chapter_changed) {
        char ch_buf[80];
        snprintf(ch_buf, sizeof(ch_buf), "Chapter %u  " LV_SYMBOL_BULLET "  %s",
                 (unsigned)(state->current_chapter + 1u), ch->title);
        lv_label_set_text(s_np_chapter_lbl, ch_buf);
        s_np_cached_chapter = state->current_chapter;
    }

    uint16_t elapsed = state->elapsed_sec;
    uint16_t total = state->chapter_duration_sec;
    if (total == 0u) {
        total = ch->duration_sec;
    }
    if (total > 0u && elapsed > total) {
        elapsed = total;
    }

    if (s_np_prog_bar) {
        if (total > 0u) {
            lv_bar_set_range(s_np_prog_bar, 0, (int32_t)total);
            lv_bar_set_value(s_np_prog_bar, (int32_t)elapsed, LV_ANIM_OFF);
        } else {
            lv_bar_set_range(s_np_prog_bar, 0, 100);
            lv_bar_set_value(s_np_prog_bar, 0, LV_ANIM_OFF);
        }
    }

    if (s_np_elapsed_lbl) {
        char elapsed_buf[12];
        format_time_mmss(elapsed, elapsed_buf, sizeof(elapsed_buf));
        lv_label_set_text(s_np_elapsed_lbl, elapsed_buf);
    }
    if (s_np_total_lbl) {
        if (total > 0u) {
            char total_buf[12];
            format_time_mmss(total, total_buf, sizeof(total_buf));
            lv_label_set_text(s_np_total_lbl, total_buf);
        } else {
            lv_label_set_text(s_np_total_lbl, "--:--");
        }
    }

    lv_label_set_text(s_np_playpause_lbl,
        (state->playback_state == PLAYBACK_PLAYING)
            ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (s_np_cached_volume != state->volume) {
        /* ISSUE-D FIX: consolidated volume display update */
        update_vol_display(state->volume);
        s_np_cached_volume = state->volume;
    }

    if (s_np_cached_sleep != state->sleep_state) {
        update_sleep_visual(state->sleep_state);
        s_np_cached_sleep = state->sleep_state;
    }

    /*
     * ISSUE-A FIX: ui_chapters_sheet_set_visible() is still called here
     * to keep visibility in sync with state. However, rebuild_chapter_list()
     * inside it is now guarded by the dirty flag ??? it only actually rebuilds
     * when story or current_chapter has changed.  Calls from playpause,
     * sleep timer, etc. skip the rebuild entirely.
     */
    ui_chapters_sheet_set_visible(state->chapters_open, state);
}

/* ================================================================== */
/*  SETTINGS SCREEN                                                     */
/* ================================================================== */

static spod_app_state_t *settings_draft(void)
{
    if (!s_settings_draft_valid) {
        const spod_app_state_t *state = ui_app_get_state();
        if (state) {
            s_settings_draft = *state;
        } else {
            memset(&s_settings_draft, 0, sizeof(s_settings_draft));
        }
        s_settings_draft_valid = true;
    }
    return &s_settings_draft;
}

static void settings_draft_reset(const spod_app_state_t *state)
{
    if (state) {
        s_settings_draft = *state;
    } else {
        const spod_app_state_t *app_state = ui_app_get_state();
        if (app_state) {
            s_settings_draft = *app_state;
        }
    }
    s_settings_draft_valid = true;
}

static void settings_draft_commit(void)
{
    if (!s_settings_draft_valid) {
        settings_draft_reset(ui_app_get_state());
    }
    const bool wifi_before = ui_app_get_state() ? ui_app_get_state()->wifi_enabled : false;
    const bool wifi_after = s_settings_draft.wifi_enabled;
    ui_app_apply_user_settings(&s_settings_draft);
    if (wifi_before != wifi_after) {
        wifi_async_apply_enabled(wifi_after);
    }
}

static void settings_switch_set(lv_obj_t *sw, bool enabled)
{
    if (!sw) return;
    if (enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
}

static lv_obj_t *settings_panel_create(lv_obj_t *screen)
{
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, DISPLAY_W - 32, DISPLAY_H - 96);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_BG_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(panel, CARD_RADIUS, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(panel, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(panel, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(panel, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(panel, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(panel, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    return panel;
}

static lv_obj_t *settings_card_base(lv_obj_t *panel, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(panel);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_BG_SHEET), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *settings_menu_btn(lv_obj_t *panel, const char *title, const char *sub, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(panel);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 92);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BG_SHEET), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title_lbl = lv_label_create(btn);
    lv_label_set_text(title_lbl, title);
    set_label_style(title_lbl, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 14, -13);

    lv_obj_t *sub_lbl = lv_label_create(btn);
    lv_label_set_text(sub_lbl, sub);
    set_label_style(sub_lbl, FONT_TINY, COL_TEXT_HINT);
    lv_obj_set_width(sub_lbl, DISPLAY_W - 120);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(sub_lbl, LV_ALIGN_LEFT_MID, 14, 17);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    set_label_style(arrow, FONT_BODY, COL_TEXT_SECONDARY);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -14, 0);
    return btn;
}

static lv_obj_t *settings_slider_card(lv_obj_t *panel, const char *title, const char *sub,
                                      int min, int max, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *card = settings_card_base(panel, 126);
    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    set_label_style(title_lbl, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *value_lbl = lv_label_create(card);
    lv_label_set_text(value_lbl, "-");
    set_label_style(value_lbl, FONT_BODY, COL_TEXT_SECONDARY);
    lv_obj_align(value_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);
    if (label_out) *label_out = value_lbl;

    lv_obj_t *sub_lbl = lv_label_create(card);
    lv_label_set_text(sub_lbl, sub);
    set_label_style(sub_lbl, FONT_TINY, COL_TEXT_HINT);
    lv_obj_align_to(sub_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_size(slider, LV_PCT(100), 10);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_slider_set_range(slider, min, max);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_BTN_SECONDARY), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_ACCENT), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return slider;
}

static lv_obj_t *settings_switch_card(lv_obj_t *panel, const char *title, const char *sub, lv_event_cb_t cb)
{
    lv_obj_t *card = settings_card_base(panel, 96);
    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    set_label_style(title_lbl, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *sub_lbl = lv_label_create(card);
    lv_label_set_text(sub_lbl, sub);
    set_label_style(sub_lbl, FONT_TINY, COL_TEXT_HINT);
    lv_obj_set_width(sub_lbl, DISPLAY_W - 140);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(sub_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_obj_t *sw = lv_switch_create(card);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, -4);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void wifi_password_dialog_close(void)
{
    if (s_wifi_password_overlay) {
        lv_obj_del(s_wifi_password_overlay);
        s_wifi_password_overlay = NULL;
        s_wifi_password_ta = NULL;
    }
}

static void wifi_password_dialog_show(const char *ssid)
{
    if (!g_scr_wifi_settings || !ssid || ssid[0] == '\0') {
        return;
    }

    wifi_password_dialog_close();
    strlcpy(s_wifi_selected_ssid, ssid, sizeof(s_wifi_selected_ssid));

    s_wifi_password_overlay = lv_obj_create(g_scr_wifi_settings);
    lv_obj_set_size(s_wifi_password_overlay, DISPLAY_W, DISPLAY_H);
    lv_obj_align(s_wifi_password_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_password_overlay, lv_color_hex(0x10151C), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_wifi_password_overlay, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_wifi_password_overlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_wifi_password_overlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_wifi_password_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_wifi_password_overlay);
    lv_obj_set_size(card, DISPLAY_W - 24, DISPLAY_H - 18);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_BG_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Connect to Wi-Fi");
    set_label_style(title, FONT_SUBTITLE, COL_TEXT_PRIMARY);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *ssid_lbl = lv_label_create(card);
    lv_label_set_text(ssid_lbl, ssid);
    set_label_style(ssid_lbl, FONT_SMALL, COL_TEXT_SECONDARY);
    lv_obj_set_width(ssid_lbl, DISPLAY_W - 64);
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align_to(ssid_lbl, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    s_wifi_password_ta = lv_textarea_create(card);
    lv_obj_set_size(s_wifi_password_ta, DISPLAY_W - 56, 42);
    lv_obj_align(s_wifi_password_ta, LV_ALIGN_TOP_MID, 0, 54);
    lv_textarea_set_one_line(s_wifi_password_ta, true);
    lv_textarea_set_password_mode(s_wifi_password_ta, true);
    lv_textarea_set_max_length(s_wifi_password_ta, AUDIO_BACKEND_WIFI_PASSWORD_MAX - 1);
    lv_textarea_set_placeholder_text(s_wifi_password_ta, "Password (leave empty for open network)");
    lv_obj_set_style_text_font(s_wifi_password_ta, FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_wifi_password_ta, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, 132, 40);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 0, 104);
    lv_obj_set_style_radius(cancel, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(COL_BTN_SECONDARY), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(cancel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(cancel, cb_settings_wifi_password_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    set_label_style(cancel_lbl, FONT_SMALL, COL_TEXT_PRIMARY);
    lv_obj_center(cancel_lbl);

    lv_obj_t *connect = lv_btn_create(card);
    lv_obj_set_size(connect, 172, 40);
    lv_obj_align(connect, LV_ALIGN_TOP_RIGHT, 0, 104);
    lv_obj_set_style_radius(connect, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(connect, lv_color_hex(COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(connect, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(connect, cb_settings_wifi_password_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_lbl = lv_label_create(connect);
    lv_label_set_text(connect_lbl, "Connect & save");
    set_label_style(connect_lbl, FONT_SMALL, COL_TEXT_PRIMARY);
    lv_obj_center(connect_lbl);

    lv_obj_t *kb = lv_keyboard_create(card);
    lv_obj_set_size(kb, DISPLAY_W - 48, 132);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, FONT_TINY, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_keyboard_set_textarea(kb, s_wifi_password_ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void wifi_scan_list_rebuild(void)
{
    if (!s_set_wifi_scan_list) {
        return;
    }

    lv_obj_clean(s_set_wifi_scan_list);
    lv_obj_set_height(s_set_wifi_scan_list, s_wifi_scan_count ? (lv_coord_t)(s_wifi_scan_count * 64 + 14) : 64);

    if (s_wifi_scan_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_set_wifi_scan_list);
        lv_label_set_text(lbl, "Tap Search for networks");
        set_label_style(lbl, FONT_TINY, COL_TEXT_HINT);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (size_t i = 0; i < s_wifi_scan_count; ++i) {
        s_wifi_scan_indices[i] = (uint8_t)i;
        lv_obj_t *btn = lv_btn_create(s_set_wifi_scan_list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 56);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BG_SHEET), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(btn, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, cb_settings_wifi_ap, LV_EVENT_CLICKED, &s_wifi_scan_indices[i]);

        lv_obj_t *ssid = lv_label_create(btn);
        lv_label_set_text(ssid, s_wifi_scan_results[i].ssid);
        set_label_style(ssid, FONT_SMALL, COL_TEXT_PRIMARY);
        lv_obj_set_width(ssid, DISPLAY_W - 148);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
        lv_obj_align(ssid, LV_ALIGN_LEFT_MID, 12, -8);

        char info[48];
        snprintf(info, sizeof(info), "%ddBm  ch%u", (int)s_wifi_scan_results[i].rssi, (unsigned)s_wifi_scan_results[i].channel);
        lv_obj_t *sub = lv_label_create(btn);
        lv_label_set_text(sub, info);
        set_label_style(sub, FONT_TINY, COL_TEXT_HINT);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 14);

        lv_obj_t *arrow = lv_label_create(btn);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        set_label_style(arrow, FONT_BODY, COL_TEXT_SECONDARY);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);
    }
}

static const char *wifi_busy_label(void)
{
    ui_wifi_state_t state;
    taskENTER_CRITICAL(&s_wifi_ui_mux);
    state = s_wifi_ui_state;
    taskEXIT_CRITICAL(&s_wifi_ui_mux);

    switch (state) {
        case WIFI_UI_SCANNING:
            return "Wi-Fi: scanning...";
        case WIFI_UI_CONNECTING:
            return "Wi-Fi: connecting...";
        case WIFI_UI_APPLYING:
            return "Wi-Fi: updating...";
        case WIFI_UI_FORGETTING:
            return "Wi-Fi: forgetting...";
        case WIFI_UI_IDLE:
        default:
            return NULL;
    }
}

static void wifi_async_complete_cb(void *arg)
{
    (void)arg;

    ui_wifi_job_type_t type = s_wifi_job.type;
    esp_err_t result = s_wifi_job_result;

    taskENTER_CRITICAL(&s_wifi_ui_mux);
    s_wifi_job_running = false;
    s_wifi_ui_state = WIFI_UI_IDLE;
    taskEXIT_CRITICAL(&s_wifi_ui_mux);

    if (type == WIFI_JOB_SCAN && result == ESP_OK) {
        wifi_scan_list_rebuild();
    }
    if (type == WIFI_JOB_CONNECT && result == ESP_OK) {
        wifi_password_dialog_close();
        /* Joining a network turns Wi-Fi on, so it reconnects on the next boot. */
        settings_draft()->wifi_enabled = true;
        settings_switch_set(s_set_wifi_switch, true);
        ui_app_set_wifi_enabled(true);
    }
    if (type == WIFI_JOB_FORGET && result == ESP_OK) {
        settings_draft()->wifi_enabled = false;
        settings_switch_set(s_set_wifi_switch, false);
        ui_app_set_wifi_enabled(false);
    }

    if (s_wifi_job_toast[0]) {
        ui_toast_show(s_wifi_job_toast);
    }
    status_bar_update_all();
    ui_screen_wifi_settings_refresh(settings_draft());
}

static void wifi_async_worker(void *arg)
{
    ui_wifi_job_t job = *(ui_wifi_job_t *)arg;
    esp_err_t err = ESP_OK;
    s_wifi_job_toast[0] = '\0';

    switch (job.type) {
        case WIFI_JOB_SCAN:
            s_wifi_scan_count = 0;
            err = audio_backend_wifi_scan(s_wifi_scan_results,
                                          AUDIO_BACKEND_WIFI_SCAN_MAX,
                                          &s_wifi_scan_count);
            if (err == ESP_OK) {
                snprintf(s_wifi_job_toast,
                         sizeof(s_wifi_job_toast),
                         "Scan: %u network%s",
                         (unsigned)s_wifi_scan_count,
                         (s_wifi_scan_count == 1u) ? "" : "s");
            } else if (err == ESP_ERR_INVALID_STATE) {
                strlcpy(s_wifi_job_toast, "Select saved Wi-Fi first", sizeof(s_wifi_job_toast));
            } else if (err == ESP_ERR_NOT_SUPPORTED) {
                strlcpy(s_wifi_job_toast, "Wi-Fi unsupported", sizeof(s_wifi_job_toast));
            } else {
                strlcpy(s_wifi_job_toast, "Wi-Fi scan failed", sizeof(s_wifi_job_toast));
            }
            break;

        case WIFI_JOB_CONNECT:
            err = audio_backend_wifi_connect_network(job.ssid, job.password, job.save);
            strlcpy(s_wifi_job_toast,
                    (err == ESP_OK) ? "Wi-Fi saved" : "Wi-Fi connect failed",
                    sizeof(s_wifi_job_toast));
            break;

        case WIFI_JOB_FORGET:
            err = audio_backend_wifi_forget_saved_network();
            strlcpy(s_wifi_job_toast,
                    (err == ESP_OK) ? "Wi-Fi forgotten" : "Forget failed",
                    sizeof(s_wifi_job_toast));
            break;

        case WIFI_JOB_APPLY_ENABLED:
            if (job.enable) {
                err = audio_backend_wifi_upload_start();
            } else {
                err = audio_backend_wifi_upload_stop();
            }
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                strlcpy(s_wifi_job_toast,
                        job.enable ? "Wi-Fi start failed" : "Wi-Fi stop failed",
                        sizeof(s_wifi_job_toast));
            }
            break;
    }

    s_wifi_job_result = err;
    lv_async_call(wifi_async_complete_cb, NULL);
    vTaskDelete(NULL);
}

static bool wifi_async_start(const ui_wifi_job_t *job, ui_wifi_state_t busy_state, const char *busy_msg)
{
    if (!job) {
        return false;
    }

    taskENTER_CRITICAL(&s_wifi_ui_mux);
    if (s_wifi_job_running) {
        taskEXIT_CRITICAL(&s_wifi_ui_mux);
        ui_toast_show("Wi-Fi busy");
        return false;
    }
    s_wifi_job = *job;
    s_wifi_job_running = true;
    s_wifi_ui_state = busy_state;
    taskEXIT_CRITICAL(&s_wifi_ui_mux);

    if (busy_msg) {
        ui_toast_show(busy_msg);
    }
    ui_screen_wifi_settings_refresh(settings_draft());

    BaseType_t ok = xTaskCreatePinnedToCore(
        wifi_async_worker,
        "wifi_ui",
        6144,
        &s_wifi_job,
        5,
        NULL,
        0
    );
    if (ok != pdPASS) {
        taskENTER_CRITICAL(&s_wifi_ui_mux);
        s_wifi_job_running = false;
        s_wifi_ui_state = WIFI_UI_IDLE;
        taskEXIT_CRITICAL(&s_wifi_ui_mux);
        ui_toast_show("Wi-Fi task failed");
        ui_screen_wifi_settings_refresh(settings_draft());
        return false;
    }

    return true;
}

static void wifi_async_apply_enabled(bool enabled)
{
    ui_wifi_job_t job = {
        .type = WIFI_JOB_APPLY_ENABLED,
        .enable = enabled,
    };
    (void)wifi_async_start(&job, WIFI_UI_APPLYING, enabled ? "Starting Wi-Fi..." : "Stopping Wi-Fi...");
}

void ui_wifi_autostart(const spod_app_state_t *state)
{
    char ssid[AUDIO_BACKEND_WIFI_SSID_MAX] = "";
    bool has_saved = false;
    if (!state || !state->wifi_enabled) {
        return;
    }
    if (audio_backend_wifi_get_saved_network(ssid, sizeof(ssid), &has_saved) != ESP_OK || !has_saved || ssid[0] == '\0') {
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi autostart: connecting to saved network \"%s\"", ssid);
    wifi_async_apply_enabled(true);
}

static void settings_label_percent(lv_obj_t *lbl, uint8_t v)
{
    if (!lbl) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)v);
    lv_label_set_text(lbl, buf);
}

static void settings_label_seconds(lv_obj_t *lbl, uint16_t v)
{
    if (!lbl) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%us", (unsigned)v);
    lv_label_set_text(lbl, buf);
}

static void settings_label_minutes(lv_obj_t *lbl, uint16_t v)
{
    if (!lbl) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u min", (unsigned)v);
    lv_label_set_text(lbl, buf);
}

static void cb_settings_back(lv_event_t *e)
{
    (void)e;
    settings_draft_commit();
    ui_app_navigate_to(SCREEN_LIBRARY, true);
}

static void cb_settings_sub_back(lv_event_t *e)
{
    (void)e;
    settings_draft_commit();
    ui_app_navigate_to(SCREEN_SETTINGS, true);
}

static void cb_settings_system(lv_event_t *e)
{
    (void)e;
    ui_app_navigate_to(SCREEN_SYSTEM_SETTINGS, false);
}

static void cb_settings_display(lv_event_t *e)
{
    (void)e;
    ui_app_navigate_to(SCREEN_DISPLAY_SETTINGS, false);
}

static void cb_settings_audio(lv_event_t *e)
{
    (void)e;
    ui_app_navigate_to(SCREEN_AUDIO_SETTINGS, false);
}

static void cb_settings_wifi_menu(lv_event_t *e)
{
    (void)e;
    ui_app_navigate_to(SCREEN_WIFI_SETTINGS, false);
}

static void cb_settings_language(lv_event_t *e)
{
    (void)e;
    spod_app_state_t *draft = settings_draft();
    draft->language = draft->language ? 0u : 1u;
    if (s_set_language_lbl) {
        lv_label_set_text(s_set_language_lbl, draft->language ? "Romana" : "English");
    }
}

static void cb_settings_idle_changed(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 1) v = 1;
    if (v > 60) v = 60;
    settings_draft()->idle_off_min = (uint16_t)v;
    settings_label_minutes(s_set_idle_lbl, (uint16_t)v);
}

static void cb_settings_brightness_changed(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    settings_draft()->brightness = (uint8_t)v;
    ui_set_user_brightness((uint8_t)v);
    settings_label_percent(s_set_brightness_lbl, (uint8_t)v);
}

static void cb_settings_min_brightness_changed(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    settings_draft()->min_brightness = (uint8_t)v;
    settings_label_percent(s_set_min_brightness_lbl, (uint8_t)v);
}

static void cb_settings_dim_changed(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 5) v = 5;
    if (v > 120) v = 120;
    settings_draft()->dim_after_sec = (uint16_t)v;
    settings_label_seconds(s_set_dim_lbl, (uint16_t)v);
}

static void cb_settings_startup_volume_changed(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    settings_draft()->startup_volume = (uint8_t)v;
    settings_label_percent(s_set_startup_vol_lbl, (uint8_t)v);
}

static void cb_settings_volume_changed(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    settings_draft()->volume = (uint8_t)v;
    settings_label_percent(s_set_volume_lbl, (uint8_t)v);
}

static void cb_settings_startup_sound_toggle(lv_event_t *e)
{
    settings_draft()->startup_sound_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void cb_settings_shutdown_sound_toggle(lv_event_t *e)
{
    settings_draft()->shutdown_sound_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static int tone_slider_to_db(int32_t level)
{
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    int32_t centered = level - 50;
    return (int)((centered * 12 + (centered >= 0 ? 25 : -25)) / 50);
}

static void format_tone_db(int32_t level, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%+d dB", tone_slider_to_db(level));
}

static void settings_tone_controls_set_enabled(bool enabled)
{
    lv_obj_t *items[] = {
        s_set_bass_slider,
        s_set_treble_slider,
        s_set_sound_enricher_switch,
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        if (!items[i]) continue;
        if (enabled) lv_obj_clear_state(items[i], LV_STATE_DISABLED);
        else lv_obj_add_state(items[i], LV_STATE_DISABLED);
    }
    if (s_set_sound_enricher_slider) {
        if (enabled && settings_draft()->sound_enricher_enabled) {
            lv_obj_clear_state(s_set_sound_enricher_slider, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_set_sound_enricher_slider, LV_STATE_DISABLED);
        }
    }
}

static void cb_settings_tone_toggle(lv_event_t *e)
{
    bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_draft()->tone_enabled = enabled;
    settings_tone_controls_set_enabled(enabled);
}

static void cb_settings_bass_changed(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    settings_draft()->tone_bass = (uint8_t)v;
    if (s_set_bass_lbl) {
        char buf[16];
        format_tone_db(v, buf, sizeof(buf));
        lv_label_set_text(s_set_bass_lbl, buf);
    }
}

static void cb_settings_treble_changed(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    settings_draft()->tone_treble = (uint8_t)v;
    if (s_set_treble_lbl) {
        char buf[16];
        format_tone_db(v, buf, sizeof(buf));
        lv_label_set_text(s_set_treble_lbl, buf);
    }
}

static void cb_settings_sound_enricher_toggle(lv_event_t *e)
{
    bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_draft()->sound_enricher_enabled = enabled;
    if (s_set_sound_enricher_slider) {
        if (enabled) lv_obj_clear_state(s_set_sound_enricher_slider, LV_STATE_DISABLED);
        else lv_obj_add_state(s_set_sound_enricher_slider, LV_STATE_DISABLED);
    }
}

static void cb_settings_sound_enricher_changed(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    settings_draft()->sound_enricher_amount = (uint8_t)v;
    settings_label_percent(s_set_sound_enricher_lbl, (uint8_t)v);
}

static void cb_settings_mono_toggle(lv_event_t *e)
{
    settings_draft()->mono_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void cb_settings_wifi(lv_event_t *e)
{
    settings_draft()->wifi_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (s_set_wifi_lbl) {
        lv_label_set_text(s_set_wifi_lbl, settings_draft()->wifi_enabled ? "Will connect on Back" : "Will turn off on Back");
    }
    status_bar_update_all();
}

static void cb_settings_wifi_scan(lv_event_t *e)
{
    (void)e;
    s_wifi_scan_count = 0;
    wifi_scan_list_rebuild();
    ui_wifi_job_t job = {
        .type = WIFI_JOB_SCAN,
    };
    (void)wifi_async_start(&job, WIFI_UI_SCANNING, "Scanning Wi-Fi...");
}

static void cb_settings_wifi_ap(lv_event_t *e)
{
    uint8_t *idx = (uint8_t *)lv_event_get_user_data(e);
    if (!idx || *idx >= s_wifi_scan_count) {
        return;
    }
    const char *ssid = s_wifi_scan_results[*idx].ssid;
    if (!ssid || ssid[0] == '\0' || strcmp(ssid, "<hidden>") == 0) {
        ui_toast_show("Hidden SSID not supported yet");
        return;
    }
    wifi_password_dialog_show(ssid);
}

static void cb_settings_wifi_password_cancel(lv_event_t *e)
{
    (void)e;
    wifi_password_dialog_close();
}

static void cb_settings_wifi_password_connect(lv_event_t *e)
{
    (void)e;
    const char *password = s_wifi_password_ta ? lv_textarea_get_text(s_wifi_password_ta) : "";
    ui_wifi_job_t job = {
        .type = WIFI_JOB_CONNECT,
        .save = true,
    };
    strlcpy(job.ssid, s_wifi_selected_ssid, sizeof(job.ssid));
    strlcpy(job.password, password, sizeof(job.password));
    (void)wifi_async_start(&job, WIFI_UI_CONNECTING, "Connecting Wi-Fi...");
}

static void cb_settings_wifi_forget(lv_event_t *e)
{
    (void)e;
    ui_wifi_job_t job = {
        .type = WIFI_JOB_FORGET,
    };
    (void)wifi_async_start(&job, WIFI_UI_FORGETTING, "Forgetting Wi-Fi...");
}

void ui_screen_settings_create(const spod_app_state_t *state)
{
    g_scr_settings = make_screen();
    make_top_bar(g_scr_settings, "Settings", true, cb_settings_back);
    lv_obj_t *panel = settings_panel_create(g_scr_settings);
    settings_menu_btn(panel, LV_SYMBOL_SETTINGS "  System", "Language, date/time, idle off", cb_settings_system);
    settings_menu_btn(panel, "Display", "Brightness and dimming", cb_settings_display);
    settings_menu_btn(panel, LV_SYMBOL_AUDIO "  Audio", "Volume, sounds, DSP, mono", cb_settings_audio);
    settings_menu_btn(panel, LV_SYMBOL_WIFI "  Wi-Fi", "Upload mode and networks", cb_settings_wifi_menu);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    lv_obj_t *version_lbl = lv_label_create(g_scr_settings);
    lv_label_set_text_fmt(version_lbl,
                          "Firmware version %s",
                          (app_desc && app_desc->version[0]) ? app_desc->version : "unknown");
    set_label_style(version_lbl, FONT_BODY, COL_TEXT_HINT);
    lv_obj_align(version_lbl, LV_ALIGN_BOTTOM_MID, 0, -34);

    ui_screen_settings_refresh(state);
}

void ui_screen_settings_refresh(const spod_app_state_t *state)
{
    if (state && !s_settings_draft_valid) {
        settings_draft_reset(state);
    }
}

void ui_screen_system_settings_create(const spod_app_state_t *state)
{
    (void)state;
    g_scr_system_settings = make_screen();
    make_top_bar(g_scr_system_settings, "System", true, cb_settings_sub_back);
    lv_obj_t *panel = settings_panel_create(g_scr_system_settings);

    lv_obj_t *lang_btn = settings_menu_btn(panel, "Language", "Tap to cycle", cb_settings_language);
    s_set_language_lbl = lv_label_create(lang_btn);
    set_label_style(s_set_language_lbl, FONT_SMALL, COL_TEXT_SECONDARY);
    lv_obj_align(s_set_language_lbl, LV_ALIGN_RIGHT_MID, -44, 0);

    lv_obj_t *date_card = settings_card_base(panel, 86);
    lv_obj_t *date_title = lv_label_create(date_card);
    lv_label_set_text(date_title, "Date / time");
    set_label_style(date_title, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(date_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *date_sub = lv_label_create(date_card);
    lv_label_set_text(date_sub, "Auto-sync when Wi-Fi is connected");
    set_label_style(date_sub, FONT_TINY, COL_TEXT_HINT);
    lv_obj_align_to(date_sub, date_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    s_set_idle_slider = settings_slider_card(panel, "Idle off time", "Shutdown if nothing is playing", 1, 60, cb_settings_idle_changed, &s_set_idle_lbl);
    ui_screen_system_settings_refresh(settings_draft());
}

void ui_screen_system_settings_refresh(const spod_app_state_t *state)
{
    const spod_app_state_t *draft = s_settings_draft_valid ? &s_settings_draft : state;
    if (!draft) draft = ui_app_get_state();
    if (s_set_language_lbl) lv_label_set_text(s_set_language_lbl, draft->language ? "Romana" : "English");
    if (s_set_idle_slider) lv_slider_set_value(s_set_idle_slider, draft->idle_off_min, LV_ANIM_OFF);
    settings_label_minutes(s_set_idle_lbl, draft->idle_off_min);
}

void ui_screen_display_settings_create(const spod_app_state_t *state)
{
    (void)state;
    g_scr_display_settings = make_screen();
    make_top_bar(g_scr_display_settings, "Display", true, cb_settings_sub_back);
    lv_obj_t *panel = settings_panel_create(g_scr_display_settings);
    s_set_brightness_slider = settings_slider_card(panel, "Default brightness", "Normal screen brightness", 0, 100, cb_settings_brightness_changed, &s_set_brightness_lbl);
    s_set_min_brightness_slider = settings_slider_card(panel, "Min brightness", "Dimmed/screen saver brightness", 0, 100, cb_settings_min_brightness_changed, &s_set_min_brightness_lbl);
    s_set_dim_slider = settings_slider_card(panel, "Dim display after", "Seconds before dimming", 5, 120, cb_settings_dim_changed, &s_set_dim_lbl);
    ui_screen_display_settings_refresh(settings_draft());
}

void ui_screen_display_settings_refresh(const spod_app_state_t *state)
{
    const spod_app_state_t *draft = s_settings_draft_valid ? &s_settings_draft : state;
    if (!draft) draft = ui_app_get_state();
    if (s_set_brightness_slider) lv_slider_set_value(s_set_brightness_slider, draft->brightness, LV_ANIM_OFF);
    settings_label_percent(s_set_brightness_lbl, draft->brightness);
    if (s_set_min_brightness_slider) lv_slider_set_value(s_set_min_brightness_slider, draft->min_brightness, LV_ANIM_OFF);
    settings_label_percent(s_set_min_brightness_lbl, draft->min_brightness);
    if (s_set_dim_slider) lv_slider_set_value(s_set_dim_slider, draft->dim_after_sec, LV_ANIM_OFF);
    settings_label_seconds(s_set_dim_lbl, draft->dim_after_sec);
}

void ui_screen_audio_settings_create(const spod_app_state_t *state)
{
    (void)state;
    g_scr_audio_settings = make_screen();
    make_top_bar(g_scr_audio_settings, "Audio", true, cb_settings_sub_back);
    lv_obj_t *panel = settings_panel_create(g_scr_audio_settings);

    s_set_volume_slider = settings_slider_card(panel, "Playback volume", "Current listening volume", 0, 100, cb_settings_volume_changed, &s_set_volume_lbl);
    s_set_startup_vol_slider = settings_slider_card(panel, "Startup tone volume", "Used by start-up and power-off sounds", 0, 100, cb_settings_startup_volume_changed, &s_set_startup_vol_lbl);
    s_set_startup_sound_switch = settings_switch_card(panel, "Start-up sound", "Default tone or none", cb_settings_startup_sound_toggle);
    s_set_shutdown_sound_switch = settings_switch_card(panel, "Power-off sound", "Default tone or none", cb_settings_shutdown_sound_toggle);
    s_set_tone_switch = settings_switch_card(panel, "DSP", "Enable tone controls and sound enrichment", cb_settings_tone_toggle);
    s_set_bass_slider = settings_slider_card(panel, "Bass", "Low frequencies", 0, 100, cb_settings_bass_changed, &s_set_bass_lbl);
    s_set_treble_slider = settings_slider_card(panel, "Treble", "High frequencies", 0, 100, cb_settings_treble_changed, &s_set_treble_lbl);
    s_set_sound_enricher_switch = settings_switch_card(panel, "Sound enricher", "Fuller low-volume sound for small speakers", cb_settings_sound_enricher_toggle);
    s_set_sound_enricher_slider = settings_slider_card(panel, "Enricher amount", "Warmth, presence and protected bass", 0, 100, cb_settings_sound_enricher_changed, &s_set_sound_enricher_lbl);
    s_set_mono_switch = settings_switch_card(panel, "Mono", "Downmix stereo tracks to mono", cb_settings_mono_toggle);
    ui_screen_audio_settings_refresh(settings_draft());
}

void ui_screen_audio_settings_refresh(const spod_app_state_t *state)
{
    const spod_app_state_t *draft = s_settings_draft_valid ? &s_settings_draft : state;
    if (!draft) draft = ui_app_get_state();
    if (s_set_volume_slider) lv_slider_set_value(s_set_volume_slider, draft->volume, LV_ANIM_OFF);
    settings_label_percent(s_set_volume_lbl, draft->volume);
    if (s_set_startup_vol_slider) lv_slider_set_value(s_set_startup_vol_slider, draft->startup_volume, LV_ANIM_OFF);
    settings_label_percent(s_set_startup_vol_lbl, draft->startup_volume);
    settings_switch_set(s_set_startup_sound_switch, draft->startup_sound_enabled);
    settings_switch_set(s_set_shutdown_sound_switch, draft->shutdown_sound_enabled);
    settings_switch_set(s_set_tone_switch, draft->tone_enabled);
    if (s_set_bass_slider) lv_slider_set_value(s_set_bass_slider, draft->tone_bass, LV_ANIM_OFF);
    if (s_set_bass_lbl) {
        char buf[16];
        format_tone_db(draft->tone_bass, buf, sizeof(buf));
        lv_label_set_text(s_set_bass_lbl, buf);
    }
    if (s_set_treble_slider) lv_slider_set_value(s_set_treble_slider, draft->tone_treble, LV_ANIM_OFF);
    if (s_set_treble_lbl) {
        char buf[16];
        format_tone_db(draft->tone_treble, buf, sizeof(buf));
        lv_label_set_text(s_set_treble_lbl, buf);
    }
    settings_switch_set(s_set_sound_enricher_switch, draft->sound_enricher_enabled);
    if (s_set_sound_enricher_slider) lv_slider_set_value(s_set_sound_enricher_slider, draft->sound_enricher_amount, LV_ANIM_OFF);
    settings_label_percent(s_set_sound_enricher_lbl, draft->sound_enricher_amount);
    if (s_set_sound_enricher_slider) {
        if (draft->tone_enabled && draft->sound_enricher_enabled) {
            lv_obj_clear_state(s_set_sound_enricher_slider, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_set_sound_enricher_slider, LV_STATE_DISABLED);
        }
    }
    settings_switch_set(s_set_mono_switch, draft->mono_enabled);
    settings_tone_controls_set_enabled(draft->tone_enabled);
}

void ui_screen_wifi_settings_create(const spod_app_state_t *state)
{
    (void)state;
    g_scr_wifi_settings = make_screen();
    make_top_bar(g_scr_wifi_settings, "Wi-Fi", true, cb_settings_sub_back);
    lv_obj_t *panel = settings_panel_create(g_scr_wifi_settings);
    s_set_wifi_switch = settings_switch_card(panel, "Wi-Fi upload", "On/off for the upload web UI", cb_settings_wifi);

    lv_obj_t *saved = settings_card_base(panel, 86);
    lv_obj_t *saved_title = lv_label_create(saved);
    lv_label_set_text(saved_title, "Saved networks");
    set_label_style(saved_title, FONT_BODY, COL_TEXT_PRIMARY);
    lv_obj_align(saved_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *saved_sub = lv_label_create(saved);
    s_set_wifi_saved_lbl = saved_sub;
    lv_label_set_text(saved_sub, "No saved network");
    set_label_style(saved_sub, FONT_TINY, COL_TEXT_HINT);
    lv_obj_set_width(saved_sub, DISPLAY_W - 190);
    lv_label_set_long_mode(saved_sub, LV_LABEL_LONG_DOT);
    lv_obj_align_to(saved_sub, saved_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_obj_t *forget = lv_btn_create(saved);
    lv_obj_set_size(forget, 118, 44);
    lv_obj_align(forget, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(forget, lv_color_hex(COL_BTN_SECONDARY), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(forget, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(forget, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(forget, 22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(forget, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(forget, cb_settings_wifi_forget, LV_EVENT_CLICKED, NULL);
    lv_obj_t *forget_lbl = lv_label_create(forget);
    lv_label_set_text(forget_lbl, "Forget");
    set_label_style(forget_lbl, FONT_TINY, COL_TEXT_PRIMARY);
    lv_obj_center(forget_lbl);

    lv_obj_t *scan_btn = settings_menu_btn(panel, LV_SYMBOL_REFRESH "  Search for networks", "Scan nearby APs", cb_settings_wifi_scan);
    s_set_wifi_lbl = lv_label_create(scan_btn);
    set_label_style(s_set_wifi_lbl, FONT_TINY, COL_TEXT_HINT);
    lv_obj_set_width(s_set_wifi_lbl, DISPLAY_W - 120);
    lv_label_set_long_mode(s_set_wifi_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(s_set_wifi_lbl, LV_ALIGN_LEFT_MID, 14, 31);

    s_set_wifi_scan_list = lv_obj_create(panel);
    lv_obj_set_width(s_set_wifi_scan_list, LV_PCT(100));
    lv_obj_set_style_bg_opa(s_set_wifi_scan_list, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_set_wifi_scan_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_set_wifi_scan_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(s_set_wifi_scan_list, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(s_set_wifi_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_set_wifi_scan_list, LV_OBJ_FLAG_SCROLLABLE);
    wifi_scan_list_rebuild();

    ui_screen_wifi_settings_refresh(settings_draft());
}

void ui_screen_wifi_settings_refresh(const spod_app_state_t *state)
{
    const spod_app_state_t *draft = s_settings_draft_valid ? &s_settings_draft : state;
    if (!draft) draft = ui_app_get_state();
    settings_switch_set(s_set_wifi_switch, draft->wifi_enabled);
    if (s_set_wifi_saved_lbl) {
        char ssid[AUDIO_BACKEND_WIFI_SSID_MAX];
        bool has_saved = false;
        if (audio_backend_wifi_get_saved_network(ssid, sizeof(ssid), &has_saved) == ESP_OK && has_saved && ssid[0] != '\0') {
            char buf[80];
            snprintf(buf, sizeof(buf), "Saved: %s", ssid);
            lv_label_set_text(s_set_wifi_saved_lbl, buf);
        } else {
            lv_label_set_text(s_set_wifi_saved_lbl, "No saved network");
        }
    }
    if (s_set_wifi_lbl) {
        const char *busy = wifi_busy_label();
        lv_label_set_text(s_set_wifi_lbl, busy ? busy : audio_backend_wifi_upload_get_status());
    }
}
/* ================================================================== */
/*  TOAST                                                               */
/* ================================================================== */

void ui_toast_show(const char *msg)
{
    if (!s_np_toast) return;

    lv_label_set_text(s_np_toast, msg);
    lv_obj_clear_flag(s_np_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_np_toast, LV_OPA_COVER,
                          LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Cancel any in-flight animation keyed to this object + exec_cb */
    lv_anim_del(s_np_toast, toast_anim_opa_cb);

    /*
     * s_toast_anim is file-scope so lv_anim_del() can find the animation
     * by (var, exec_cb) key.  LVGL v8 copies the struct in lv_anim_start()
     * so we reinitialise completely each time.
     */
    lv_anim_init(&s_toast_anim);
    lv_anim_set_var(&s_toast_anim, s_np_toast);
    lv_anim_set_exec_cb(&s_toast_anim, toast_anim_opa_cb);
    lv_anim_set_values(&s_toast_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&s_toast_anim, 1800);
    lv_anim_set_time(&s_toast_anim, 400);
    lv_anim_set_path_cb(&s_toast_anim, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&s_toast_anim, toast_anim_ready_cb);
    lv_anim_start(&s_toast_anim);
}


