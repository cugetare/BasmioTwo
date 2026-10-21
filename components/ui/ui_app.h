/**
 * @file ui_app.h
 * @brief sPod v2 - application logic public API.
 *
 * The app layer owns:
 *   - The single global spod_app_state_t instance.
 *   - The mock story dataset (g_mock_stories / g_mock_story_count).
 *   - The navigation controller (ui_app_navigate_to).
 *   - All state-mutation helpers called by screen callbacks.
 *
 * Integration point:
 *   Call ui_app_init() exactly once from app_main(), after lv_init() and
 *   the display driver are fully initialised.
 *   No periodic tick function is required; all updates are event-driven.
 */

#ifndef UI_APP_H
#define UI_APP_H

#include <stdbool.h>
#include "ui_models.h"

typedef void (*ui_app_shutdown_cb_t)(void);

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
void ui_wakeup(void);
void ui_set_backlight_target(uint8_t pct);
void ui_set_user_brightness(uint8_t pct);
uint8_t ui_get_user_brightness(void);
void ui_set_min_brightness(uint8_t pct);
uint8_t ui_get_min_brightness(void);
void ui_set_dim_after_seconds(uint16_t seconds);
uint16_t ui_get_dim_after_seconds(void);
void ui_set_idle_off_minutes(uint16_t minutes);
uint16_t ui_get_idle_off_minutes(void);
/**
 * @brief Initialise the sPod UI.
 *
 * Creates all screens, loads the Library, and wires all callbacks.
 * Must be called once after LVGL and the display driver are ready.
 */
void ui_app_init(void);

/**
 * @brief Load persisted settings into app state before UI creation.
 *
 * Safe to call before ui_app_init(); app_main uses it so boot sounds and
 * brightness defaults can be applied early.
 */
void ui_app_load_persisted_settings(void);

/**
 * @brief Persist and apply all user settings from a draft state.
 */
void ui_app_apply_user_settings(const spod_app_state_t *draft);

/**
 * @brief Register callback invoked when app enters shutdown state.
 */
void ui_app_set_shutdown_callback(ui_app_shutdown_cb_t cb);

/**
 * @brief Enter shutdown state and invoke registered shutdown callback once.
 */
void ui_app_request_shutdown(void);

/**
 * @brief Navigate to a target screen.
 *
 * Refreshes the destination screen with the latest app state, then loads
 * it with a slide animation.
 *
 * FIX BUG-20: pass go_back=true when navigating toward the root so the
 * animation slides rightward (feels like going back), and false when going
 * deeper (slides leftward).
 *
 * FIX BUG-17: navigate_to no longer resets chapters_open.  The caller
 * controls that field before calling this function.
 *
 * @param target   The screen to navigate to.
 * @param go_back  true = slide right (back), false = slide left (forward).
 */
void ui_app_navigate_to(spod_screen_id_t target, bool go_back);

/* ------------------------------------------------------------------ */
/*  State accessor and mutation helpers                                 */
/*  (used by screen callbacks in ui_screens.c)                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Return a pointer to the global app state.
 *
 * Screen callbacks call this to read or mutate state before calling a
 * refresh function or navigate_to().  The pointer is stable for the
 * lifetime of the application.
 */
spod_app_state_t *ui_app_get_state(void);

/**
 * @brief Select a story by dataset index and reset playback position.
 * @param story_index  Index into active dataset [0, ui_app_story_count()).
 */
void ui_app_select_story(uint8_t story_index);

/**
 * @brief Return active story count (runtime SD catalog or mock fallback).
 */
uint8_t ui_app_story_count(void);

/**
 * @brief Return story by index from active dataset, or NULL on invalid index.
 */
const spod_story_t *ui_app_story_get(uint8_t story_index);

/**
 * @brief Toggle playback between PLAYING and PAUSED.
 *        Also refreshes the Now Playing screen widgets.
 */
void ui_app_toggle_playback(void);

/**
 * @brief Stop playback and reset backend transport state.
 */
void ui_app_stop_playback(void);

/**
 * @brief Jump to a chapter by index.
 *        Resets elapsed_sec, sets playback to PLAYING.
 * @param chapter_index  0-based index within the currently selected story.
 */
void ui_app_select_chapter(uint8_t chapter_index);
void ui_app_resume_selected_story(void);
bool ui_app_selected_story_resume(uint8_t *chapter_index, uint32_t *resume_sec);

/**
 * @brief Set volume in app state.
 * @param volume  Clamped to [0, 100].
 */
void ui_app_set_volume(uint8_t volume);

/**
 * @brief Set startup tone volume used on next boot sequence.
 * @param volume  Clamped to [0, 100].
 */
void ui_app_set_startup_volume(uint8_t volume);

/**
 * @brief Get startup tone volume setting.
 */
uint8_t ui_app_get_startup_volume(void);

/**
 * @brief Enable/disable Mini DSP tone control.
 */
void ui_app_set_tone_enabled(bool enabled);

/**
 * @brief Set Mini DSP bass level (0..100, 50 = flat).
 */
void ui_app_set_tone_bass(uint8_t bass);

/**
 * @brief Set Mini DSP treble level (0..100, 50 = flat).
 */
void ui_app_set_tone_treble(uint8_t treble);

/**
 * @brief Enable/disable small-speaker sound enrichment.
 */
void ui_app_set_sound_enricher_enabled(bool enabled);

/**
 * @brief Set sound enricher amount (0..100).
 */
void ui_app_set_sound_enricher_amount(uint8_t amount);

/**
 * @brief Enable/disable mono downmix in the audio backend.
 */
void ui_app_set_mono_enabled(bool enabled);

void ui_app_set_startup_sound_enabled(bool enabled);
bool ui_app_get_startup_sound_enabled(void);
void ui_app_set_shutdown_sound_enabled(bool enabled);
bool ui_app_get_shutdown_sound_enabled(void);
void ui_app_set_min_brightness_setting(uint8_t brightness);
void ui_app_set_dim_after_seconds_setting(uint16_t seconds);
void ui_app_set_idle_off_minutes_setting(uint16_t minutes);
void ui_app_set_language(uint8_t language);
void ui_app_set_wifi_enabled(bool enabled);

/**
 * @brief Set user brightness target in app state and backlight controller.
 * @param brightness  Clamped to [0, 100].
 */
void ui_app_set_brightness(uint8_t brightness);

/**
 * @brief Get user brightness setting.
 */
uint8_t ui_app_get_brightness(void);

/**
 * @brief Advance the sleep timer one step (cyclic: Off->15->30->45->60->Off).
 *        Shows a toast and refreshes the sleep label on Now Playing.
 */
void ui_app_cycle_sleep_timer(void);

/**
 * @brief Poll backend playback progress and update app/UI state when changed.
 */
void ui_app_poll_playback_progress(void);

#endif /* UI_APP_H */
