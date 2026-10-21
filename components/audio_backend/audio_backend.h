#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Catalog limits for SD story scan */
#define AUDIO_BACKEND_MAX_STORIES      30
#define AUDIO_BACKEND_MAX_CHAPTERS     20
#define AUDIO_BACKEND_TITLE_MAX        64
#define AUDIO_BACKEND_PATH_MAX         160
#define AUDIO_BACKEND_WIFI_SCAN_MAX    12
#define AUDIO_BACKEND_WIFI_SSID_MAX    33
#define AUDIO_BACKEND_WIFI_PASSWORD_MAX 65

typedef struct {
    char title[AUDIO_BACKEND_TITLE_MAX];
    char file_name[AUDIO_BACKEND_TITLE_MAX];
    char file_path[AUDIO_BACKEND_PATH_MAX];
    uint32_t duration_sec;
} audio_backend_chapter_meta_t;

typedef struct {
    uint8_t id;
    char folder_name[AUDIO_BACKEND_TITLE_MAX];
    char title[AUDIO_BACKEND_TITLE_MAX];
    char album_art_file[AUDIO_BACKEND_TITLE_MAX];
    char album_art_path[AUDIO_BACKEND_PATH_MAX];
    char json_path[AUDIO_BACKEND_PATH_MAX];
    uint32_t play_count;
    uint32_t resume_time_sec;
    uint8_t resume_chapter_index;
    uint8_t chapter_count;
    audio_backend_chapter_meta_t chapters[AUDIO_BACKEND_MAX_CHAPTERS];
} audio_backend_story_meta_t;

typedef struct {
    char ssid[AUDIO_BACKEND_WIFI_SSID_MAX];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
} audio_backend_wifi_ap_t;

esp_err_t audio_backend_init(void);
bool audio_backend_is_ready(void);
bool audio_backend_is_decoder_ready(void);
bool audio_backend_is_playback_active(void);
bool audio_backend_consume_finished_event(void);

esp_err_t audio_backend_play_chapter(uint8_t story_id, uint8_t chapter_index);
esp_err_t audio_backend_play_chapter_from(uint8_t story_id, uint8_t chapter_index, uint32_t start_sec);
esp_err_t audio_backend_play_startup_tone(void);
esp_err_t audio_backend_play_shutdown_tone(void);
esp_err_t audio_backend_get_playback_progress(uint32_t *elapsed_sec, uint32_t *duration_sec);
esp_err_t audio_backend_get_vu_level(uint8_t *level_pct);
esp_err_t audio_backend_pause(void);
esp_err_t audio_backend_resume(void);
esp_err_t audio_backend_stop(void);
esp_err_t audio_backend_prepare_shutdown(void);
esp_err_t audio_backend_set_volume(uint8_t volume);
esp_err_t audio_backend_set_startup_volume(uint8_t volume);
uint8_t audio_backend_get_startup_volume(void);
esp_err_t audio_backend_set_tone_control(bool enabled, uint8_t bass, uint8_t treble);
esp_err_t audio_backend_get_tone_control(bool *enabled, uint8_t *bass, uint8_t *treble);
esp_err_t audio_backend_set_loudness(bool enabled);
esp_err_t audio_backend_get_loudness(bool *enabled);
esp_err_t audio_backend_set_sound_enricher(bool enabled, uint8_t amount);
esp_err_t audio_backend_get_sound_enricher(bool *enabled, uint8_t *amount);
esp_err_t audio_backend_set_mono_enabled(bool enabled);
esp_err_t audio_backend_get_mono_enabled(bool *enabled);
esp_err_t audio_backend_set_shutdown_tone_enabled(bool enabled);
bool audio_backend_get_shutdown_tone_enabled(void);
esp_err_t audio_backend_wifi_upload_start(void);
esp_err_t audio_backend_wifi_upload_stop(void);
bool audio_backend_wifi_upload_is_running(void);
bool audio_backend_wifi_upload_is_connected(void);
esp_err_t audio_backend_wifi_upload_get_url(char *out, size_t out_size);
const char *audio_backend_wifi_upload_get_status(void);
esp_err_t audio_backend_wifi_scan(audio_backend_wifi_ap_t *aps, size_t max_aps, size_t *out_count);
esp_err_t audio_backend_wifi_scan_summary(char *out, size_t out_size);
esp_err_t audio_backend_wifi_get_saved_network(char *ssid, size_t ssid_size, bool *out_has_saved);
esp_err_t audio_backend_wifi_forget_saved_network(void);
esp_err_t audio_backend_wifi_connect_network(const char *ssid, const char *password, bool save);

esp_err_t audio_backend_rescan_catalog(void);
uint8_t audio_backend_story_count(void);
const audio_backend_story_meta_t *audio_backend_story_get(uint8_t index);

#ifdef __cplusplus
}
#endif
