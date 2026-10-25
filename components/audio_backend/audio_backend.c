#include "audio_backend.h"
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "sdmmc_cmd.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "audio_backend";

/*
 * Pin mapping provided from your board example:
 *
 * JC3248W535EN pin mapping:\n *\n * SD_MMC, 1-bit:\n *   D0  -> GPIO13\n *   CMD -> GPIO11\n *   CLK -> GPIO12\n *\n * I2S amplifier:\n *   BCLK -> GPIO42\n *   WS   -> GPIO2\n *   DOUT -> GPIO41\n *   MCLK unused
 */
#define SD_D0               GPIO_NUM_13
#define SD_D1               GPIO_NUM_NC
#define SD_D2               GPIO_NUM_NC
#define SD_D3               GPIO_NUM_NC
#define SD_CMD              GPIO_NUM_11
#define SD_CLK              GPIO_NUM_12

#define I2S_MCK_IO          GPIO_NUM_NC
#define I2S_BCK_IO          GPIO_NUM_42
#define I2S_DI_IO           GPIO_NUM_NC
#define I2S_WS_IO           GPIO_NUM_2
#define I2S_DO_IO           GPIO_NUM_41
#define ES8311_PA_GPIO      GPIO_NUM_NC
/* External amplifier mute line: active high = muted, inactive low = playing. */
#define AMP_MUTE_GPIO       GPIO_NUM_17
#define AMP_MUTE_ACTIVE_LEVEL   0
#define I2C_SDA_IO          GPIO_NUM_4
#define I2C_SCL_IO          GPIO_NUM_8
#define I2C_PORT_NUM        I2C_NUM_0

#define AUDIO_MOUNT_POINT   "/sdcard"
#define AUDIO_PATH_MAX      160
#define STORY_JSON_MAX_BYTES (8 * 1024)

#define AUDIO_DEFAULT_RATE_HZ       44100U
#define MP3_PLAY_TASK_STACK_BYTES   (32 * 1024)
#define MP3_PLAY_TASK_PRIORITY      11
/* Core 1 is otherwise idle; core 0 runs LVGL, Wi-Fi and lwIP, so decoding there starved the UI. */
#define MP3_PLAY_TASK_CORE          1
#define MP3_INPUT_BUFFER_SIZE       (16 * 1024)
#define MP3_OUTPUT_BUFFER_SIZE      (8 * 1024)
#define MP3_PAUSE_POLL_MS           20
#define MP3_WRITE_TIMEOUT_MS        500
#define MP3_RESUME_BACKOFF_SEC      3
#define MP3_PERSIST_PERIOD_SEC      30
#define ID3V2_MAX_TAG_BYTES         (2 * 1024 * 1024)
#define ID3V2_MAX_APIC_BYTES        (1024 * 1024)
#define ID3V2_COVER_BASENAME        "ID3COVR"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef CONFIG_AUDIO_BACKEND_SDMMC_BUS_WIDTH_4
#define CONFIG_AUDIO_BACKEND_SDMMC_BUS_WIDTH_4 0
#endif

#ifndef CONFIG_AUDIO_BACKEND_SDMMC_TRY_1BIT_FALLBACK
#define CONFIG_AUDIO_BACKEND_SDMMC_TRY_1BIT_FALLBACK 1
#endif

#ifndef CONFIG_AUDIO_BACKEND_SDMMC_MAX_FREQ_KHZ
#define CONFIG_AUDIO_BACKEND_SDMMC_MAX_FREQ_KHZ SDMMC_FREQ_DEFAULT
#endif

#ifndef CONFIG_AUDIO_BACKEND_SDMMC_FORMAT_IF_MOUNT_FAILED
#define CONFIG_AUDIO_BACKEND_SDMMC_FORMAT_IF_MOUNT_FAILED 0
#endif

#if SOC_SDMMC_IO_POWER_EXTERNAL
#ifndef CONFIG_AUDIO_BACKEND_SDMMC_USE_INTERNAL_LDO
#define CONFIG_AUDIO_BACKEND_SDMMC_USE_INTERNAL_LDO 1
#endif
#ifndef CONFIG_AUDIO_BACKEND_SDMMC_LDO_CHANNEL_ID
#define CONFIG_AUDIO_BACKEND_SDMMC_LDO_CHANNEL_ID 4
#endif
#endif

static bool s_initialized = false;
static bool s_sd_ready = false;
static bool s_i2s_ready = false;
static bool s_decoder_ready = false;
static bool s_tx_enabled = false;

static portMUX_TYPE s_playback_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_stop_requested = false;
static bool s_pause_requested = false;
static TaskHandle_t s_play_task = NULL;
static bool s_play_task_with_caps = false;
static bool s_playback_finished_event = false;

static uint8_t s_volume = 40;
static uint8_t s_startup_volume = 40;
static bool s_tone_enabled = false;
static uint8_t s_tone_bass = 50;
static uint8_t s_tone_treble = 50;
static bool s_sound_enricher_enabled = false;
static uint8_t s_sound_enricher_amount = 55;
static bool s_mono_enabled = true;
static bool s_shutdown_tone_enabled = true;
static volatile uint32_t s_tone_revision = 1;
static uint32_t s_i2s_sample_rate_hz = AUDIO_DEFAULT_RATE_HZ;
static char s_first_mp3[AUDIO_PATH_MAX];
static char s_play_path[AUDIO_PATH_MAX];
static uint8_t s_story_count = 0;
static EXT_RAM_BSS_ATTR audio_backend_story_meta_t s_stories[AUDIO_BACKEND_MAX_STORIES];

static uint64_t s_active_elapsed_us = 0;
static uint32_t s_active_duration_sec = 0;
static uint32_t s_start_offset_sec = 0;
static uint8_t s_active_vu_level = 0;
static int s_active_story_index = -1;
static int s_active_chapter_index = -1;

static sdmmc_card_t *s_card = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
#if SOC_SDMMC_IO_POWER_EXTERNAL
static sd_pwr_ctrl_handle_t s_sd_pwr_ctrl = NULL;
#endif

static void playback_set_stop_requested(bool requested)
{
    taskENTER_CRITICAL(&s_playback_mux);
    s_stop_requested = requested;
    taskEXIT_CRITICAL(&s_playback_mux);
}

static bool playback_stop_requested(void)
{
    bool requested;
    taskENTER_CRITICAL(&s_playback_mux);
    requested = s_stop_requested;
    taskEXIT_CRITICAL(&s_playback_mux);
    return requested;
}

static void playback_set_pause_requested(bool requested)
{
    taskENTER_CRITICAL(&s_playback_mux);
    s_pause_requested = requested;
    taskEXIT_CRITICAL(&s_playback_mux);
}

static bool playback_pause_requested(void)
{
    bool requested;
    taskENTER_CRITICAL(&s_playback_mux);
    requested = s_pause_requested;
    taskEXIT_CRITICAL(&s_playback_mux);
    return requested;
}

static void playback_set_task(TaskHandle_t task, bool with_caps)
{
    taskENTER_CRITICAL(&s_playback_mux);
    s_play_task = task;
    s_play_task_with_caps = with_caps;
    taskEXIT_CRITICAL(&s_playback_mux);
}

static TaskHandle_t playback_get_task(void)
{
    TaskHandle_t task;
    taskENTER_CRITICAL(&s_playback_mux);
    task = s_play_task;
    taskEXIT_CRITICAL(&s_playback_mux);
    return task;
}

static bool playback_clear_task_and_take_caps(void)
{
    bool with_caps;
    taskENTER_CRITICAL(&s_playback_mux);
    s_play_task = NULL;
    with_caps = s_play_task_with_caps;
    s_play_task_with_caps = false;
    taskEXIT_CRITICAL(&s_playback_mux);
    return with_caps;
}

static void playback_set_finished_event(bool finished)
{
    taskENTER_CRITICAL(&s_playback_mux);
    s_playback_finished_event = finished;
    taskEXIT_CRITICAL(&s_playback_mux);
}

static void playback_set_position(uint64_t elapsed_us, uint32_t duration_sec, uint32_t start_offset_sec)
{
    taskENTER_CRITICAL(&s_playback_mux);
    s_active_elapsed_us = elapsed_us;
    s_active_duration_sec = duration_sec;
    s_start_offset_sec = start_offset_sec;
    taskEXIT_CRITICAL(&s_playback_mux);
}

static void playback_add_elapsed(uint64_t frame_us)
{
    taskENTER_CRITICAL(&s_playback_mux);
    s_active_elapsed_us += frame_us;
    taskEXIT_CRITICAL(&s_playback_mux);
}

static void playback_get_position(uint32_t *elapsed_sec, uint32_t *duration_sec)
{
    taskENTER_CRITICAL(&s_playback_mux);
    if (elapsed_sec) {
        *elapsed_sec = (uint32_t)(s_active_elapsed_us / 1000000ULL);
    }
    if (duration_sec) {
        *duration_sec = s_active_duration_sec;
    }
    taskEXIT_CRITICAL(&s_playback_mux);
}

static uint32_t playback_elapsed_sec(void)
{
    uint32_t elapsed_sec;
    playback_get_position(&elapsed_sec, NULL);
    return elapsed_sec;
}

static uint32_t playback_start_offset_sec(void)
{
    uint32_t start_offset_sec;
    taskENTER_CRITICAL(&s_playback_mux);
    start_offset_sec = s_start_offset_sec;
    taskEXIT_CRITICAL(&s_playback_mux);
    return start_offset_sec;
}

static void playback_set_duration(uint32_t duration_sec)
{
    taskENTER_CRITICAL(&s_playback_mux);
    s_active_duration_sec = duration_sec;
    taskEXIT_CRITICAL(&s_playback_mux);
}

static void playback_set_vu_level(uint8_t level)
{
    taskENTER_CRITICAL(&s_playback_mux);
    s_active_vu_level = level;
    taskEXIT_CRITICAL(&s_playback_mux);
}

static uint8_t playback_get_vu_level(void)
{
    uint8_t level;
    taskENTER_CRITICAL(&s_playback_mux);
    level = s_active_vu_level;
    taskEXIT_CRITICAL(&s_playback_mux);
    return level;
}

static void playback_clear_runtime_state(void)
{
    taskENTER_CRITICAL(&s_playback_mux);
    s_active_elapsed_us = 0;
    s_active_duration_sec = 0;
    s_start_offset_sec = 0;
    s_active_vu_level = 0;
    s_stop_requested = false;
    s_pause_requested = false;
    taskEXIT_CRITICAL(&s_playback_mux);
}

static uint32_t estimate_mp3_duration_sec(const char *path);
static size_t get_id3v2_total_size(const uint8_t *hdr10);
static size_t skip_id3v2_tag(FILE *fp);
static bool extract_story_cover_from_id3(const char *mp3_path,
                                         const char *story_folder,
                                         char *out_file_name,
                                         size_t out_file_name_size,
                                         char *out_file_path,
                                         size_t out_file_path_size,
                                         const char **out_skip_reason);

static bool has_extension_ci(const char *name, const char *ext)
{
    if (!name || !ext) {
        return false;
    }
    size_t n = strlen(name);
    size_t e = strlen(ext);
    if (n < e || e == 0) {
        return false;
    }
    const char *tail = &name[n - e];
    for (size_t i = 0; i < e; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i])) {
            return false;
        }
    }
    return true;
}

static size_t get_id3v2_total_size(const uint8_t *hdr10)
{
    if (!hdr10) {
        return 0;
    }
    if (!(hdr10[0] == 'I' && hdr10[1] == 'D' && hdr10[2] == '3')) {
        return 0;
    }
    if ((hdr10[6] & 0x80u) || (hdr10[7] & 0x80u) || (hdr10[8] & 0x80u) || (hdr10[9] & 0x80u)) {
        return 0;
    }

    size_t payload = ((size_t)(hdr10[6] & 0x7Fu) << 21) |
                     ((size_t)(hdr10[7] & 0x7Fu) << 14) |
                     ((size_t)(hdr10[8] & 0x7Fu) << 7) |
                     ((size_t)(hdr10[9] & 0x7Fu));
    size_t total = 10u + payload;
    if (hdr10[5] & 0x10u) {
        total += 10u;
    }
    return total;
}

static size_t skip_id3v2_tag(FILE *fp)
{
    if (!fp) {
        return 0;
    }

    long pos = ftell(fp);
    if (pos < 0) {
        pos = 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        return 0;
    }

    uint8_t hdr[10];
    size_t n = fread(hdr, 1, sizeof(hdr), fp);
    if (n < sizeof(hdr)) {
        (void)fseek(fp, pos, SEEK_SET);
        return 0;
    }

    size_t skip = get_id3v2_total_size(hdr);
    if (skip == 0) {
        (void)fseek(fp, pos, SEEK_SET);
        return 0;
    }

    if (fseek(fp, (long)skip, SEEK_SET) != 0) {
        (void)fseek(fp, pos, SEEK_SET);
        return 0;
    }
    return skip;
}

static bool has_mp3_extension(const char *name)
{
    return has_extension_ci(name, ".mp3");
}

static bool has_jpg_extension(const char *name)
{
    return has_extension_ci(name, ".jpg");
}

static bool has_jpeg_extension(const char *name)
{
    return has_extension_ci(name, ".jpeg");
}

static bool has_bmp_extension(const char *name)
{
    return has_extension_ci(name, ".bmp");
}

static bool has_png_extension(const char *name)
{
    return has_extension_ci(name, ".png");
}

static bool has_supported_cover_extension(const char *name)
{
    return has_jpg_extension(name) || has_bmp_extension(name) || has_png_extension(name);
}

static bool is_dot_entry(const char *name)
{
    return name && ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0));
}

static bool is_dir_path(const char *path)
{
    struct stat st = {0};
    return (path && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static int ci_char_cmp(char a, char b)
{
    return (int)tolower((unsigned char)a) - (int)tolower((unsigned char)b);
}

static int ci_string_cmp(const char *a, const char *b)
{
    if (a == b) {
        return 0;
    }
    if (!a) {
        return -1;
    }
    if (!b) {
        return 1;
    }
    while (*a && *b) {
        int d = ci_char_cmp(*a, *b);
        if (d != 0) {
            return d;
        }
        a++;
        b++;
    }
    return ci_char_cmp(*a, *b);
}

static int ci_string_ncmp(const char *a, const char *b, size_t n)
{
    if (n == 0) {
        return 0;
    }
    if (a == b) {
        return 0;
    }
    if (!a) {
        return -1;
    }
    if (!b) {
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        int d = ci_char_cmp(a[i], b[i]);
        if (d != 0 || a[i] == '\0' || b[i] == '\0') {
            return d;
        }
    }
    return 0;
}

static bool has_cover_file_name(const char *name)
{
    if (!name) {
        return false;
    }
    static const char prefix[] = "cover.";
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) {
        if (name[i] == '\0' || ci_char_cmp(name[i], prefix[i]) != 0) {
            return false;
        }
    }
    return true;
}

static int cover_file_score(const char *name)
{
    if (!name) {
        return -1;
    }

    size_t generated_len = strlen(ID3V2_COVER_BASENAME);
    if (strlen(name) > generated_len &&
        name[generated_len] == '.' &&
        ci_string_ncmp(name, ID3V2_COVER_BASENAME, generated_len) == 0) {
        return -1;
    }

    const bool named_cover = has_cover_file_name(name);

    if (has_png_extension(name)) {
        return named_cover ? 400 : 300;
    }
    if (has_jpg_extension(name)) {
        return named_cover ? 250 : 200;
    }
    if (has_bmp_extension(name)) {
        return named_cover ? 150 : 100;
    }
    if (has_jpeg_extension(name)) {
        /* LVGL SJPG decoder commonly matches ".jpg", keep ".jpeg" last resort. */
        return named_cover ? 50 : 25;
    }
    return -1;
}

static int qsort_story_cmp(const void *lhs, const void *rhs)
{
    const audio_backend_story_meta_t *a = (const audio_backend_story_meta_t *)lhs;
    const audio_backend_story_meta_t *b = (const audio_backend_story_meta_t *)rhs;
    return ci_string_cmp(a->folder_name, b->folder_name);
}

static bool parse_leading_number(const char *name, uint32_t *out)
{
    if (!name || !out) {
        return false;
    }

    const char *p = name;
    while (*p == ' ' || *p == '_' || *p == '-' || *p == '.') {
        p++;
    }
    if (!isdigit((unsigned char)*p)) {
        return false;
    }

    uint32_t value = 0;
    while (isdigit((unsigned char)*p)) {
        value = (value * 10u) + (uint32_t)(*p - '0');
        p++;
    }
    *out = value;
    return true;
}

static int chapter_name_cmp(const char *a, const char *b)
{
    uint32_t na = 0;
    uint32_t nb = 0;
    bool has_na = parse_leading_number(a, &na);
    bool has_nb = parse_leading_number(b, &nb);

    if (has_na && has_nb && na != nb) {
        return (na < nb) ? -1 : 1;
    }
    if (has_na != has_nb) {
        return has_na ? -1 : 1;
    }
    return ci_string_cmp(a, b);
}

static bool path_join2(char *out, size_t out_size, const char *a, const char *b)
{
    if (!out || out_size == 0 || !a || !b) {
        return false;
    }
    size_t need = strlen(a) + 1 + strlen(b) + 1;
    if (need > out_size) {
        out[0] = '\0';
        return false;
    }
    strlcpy(out, a, out_size);
    strlcat(out, "/", out_size);
    strlcat(out, b, out_size);
    return true;
}

static bool path_join3(char *out, size_t out_size, const char *a, const char *b, const char *c)
{
    if (!out || out_size == 0 || !a || !b || !c) {
        return false;
    }
    size_t need = strlen(a) + 1 + strlen(b) + 1 + strlen(c) + 1;
    if (need > out_size) {
        out[0] = '\0';
        return false;
    }
    strlcpy(out, a, out_size);
    strlcat(out, "/", out_size);
    strlcat(out, b, out_size);
    strlcat(out, "/", out_size);
    strlcat(out, c, out_size);
    return true;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    char mime[32];
    uint8_t picture_type;
} id3_apic_frame_t;

static uint32_t id3v2_be32(const uint8_t *p)
{
    if (!p) {
        return 0;
    }
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint32_t id3v2_syncsafe32(const uint8_t *p)
{
    if (!p) {
        return 0;
    }
    if ((p[0] & 0x80u) || (p[1] & 0x80u) || (p[2] & 0x80u) || (p[3] & 0x80u)) {
        return 0;
    }
    return ((uint32_t)(p[0] & 0x7Fu) << 21) |
           ((uint32_t)(p[1] & 0x7Fu) << 14) |
           ((uint32_t)(p[2] & 0x7Fu) << 7) |
           (uint32_t)(p[3] & 0x7Fu);
}

static bool ci_contains(const char *text, const char *needle)
{
    if (!text || !needle || needle[0] == '\0') {
        return false;
    }

    size_t needle_len = strlen(needle);
    for (const char *p = text; *p != '\0'; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] != '\0' && ci_char_cmp(p[i], needle[i]) == 0) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static const char *detect_image_ext(const char *mime, const uint8_t *data, size_t data_size)
{
    if (mime && mime[0] != '\0') {
        if (ci_contains(mime, "png")) {
            return "png";
        }
        if (ci_contains(mime, "jpeg") || ci_contains(mime, "jpg")) {
            return "jpg";
        }
        if (ci_contains(mime, "bmp")) {
            return "bmp";
        }
    }

    if (data && data_size >= 8 &&
        data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
        return "png";
    }
    if (data && data_size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "jpg";
    }
    if (data && data_size >= 2 && data[0] == 'B' && data[1] == 'M') {
        return "bmp";
    }

    return NULL;
}

static bool parse_apic_frame_payload(const uint8_t *payload, size_t payload_size, id3_apic_frame_t *out)
{
    if (!payload || !out || payload_size < 4) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    uint8_t encoding = payload[pos++];

    size_t mime_start = pos;
    while (pos < payload_size && payload[pos] != '\0') {
        pos++;
    }
    if (pos >= payload_size) {
        return false;
    }

    size_t mime_len = pos - mime_start;
    if (mime_len >= sizeof(out->mime)) {
        mime_len = sizeof(out->mime) - 1;
    }
    if (mime_len > 0) {
        memcpy(out->mime, &payload[mime_start], mime_len);
    }
    out->mime[mime_len] = '\0';

    pos++; /* MIME terminator */
    if (pos >= payload_size) {
        return false;
    }

    out->picture_type = payload[pos++];

    bool desc_term_found = false;
    if (encoding == 0 || encoding == 3) {
        while (pos < payload_size) {
            if (payload[pos] == 0) {
                pos++;
                desc_term_found = true;
                break;
            }
            pos++;
        }
    } else if (encoding == 1 || encoding == 2) {
        while (pos + 1 < payload_size) {
            if (payload[pos] == 0 && payload[pos + 1] == 0) {
                pos += 2;
                desc_term_found = true;
                break;
            }
            pos += 2;
        }
    } else {
        return false;
    }

    if (!desc_term_found && pos >= payload_size) {
        return false;
    }
    if (pos >= payload_size) {
        return false;
    }

    out->data = &payload[pos];
    out->size = payload_size - pos;
    return out->size > 0 && out->size <= ID3V2_MAX_APIC_BYTES;
}

static bool is_id3_frame_id_char(uint8_t c)
{
    return ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
}

static bool is_valid_id3_frame_id4(const uint8_t *frame_id)
{
    if (!frame_id) {
        return false;
    }
    return is_id3_frame_id_char(frame_id[0]) &&
           is_id3_frame_id_char(frame_id[1]) &&
           is_id3_frame_id_char(frame_id[2]) &&
           is_id3_frame_id_char(frame_id[3]);
}

static bool build_story_id3_cover_paths(const char *story_folder,
                                        const char *ext,
                                        char *out_file_name,
                                        size_t out_file_name_size,
                                        char *out_file_path,
                                        size_t out_file_path_size)
{
    if (!story_folder || !ext || !out_file_name || !out_file_path ||
        out_file_name_size == 0 || out_file_path_size == 0) {
        return false;
    }

    int n = snprintf(out_file_name, out_file_name_size, "%s.%s", ID3V2_COVER_BASENAME, ext);
    if (n <= 0 || (size_t)n >= out_file_name_size) {
        out_file_name[0] = '\0';
        return false;
    }

    if (!path_join3(out_file_path, out_file_path_size, AUDIO_MOUNT_POINT, story_folder, out_file_name)) {
        out_file_name[0] = '\0';
        out_file_path[0] = '\0';
        return false;
    }

    return true;
}

static void remove_story_id3_cover_alternates(const char *story_folder, const char *keep_ext)
{
    if (!story_folder) {
        return;
    }

    static const char *k_exts[] = {"jpg", "png", "bmp"};
    for (size_t i = 0; i < sizeof(k_exts) / sizeof(k_exts[0]); i++) {
        if (keep_ext && ci_string_cmp(k_exts[i], keep_ext) == 0) {
            continue;
        }

        char file_name[24];
        int n = snprintf(file_name, sizeof(file_name), "%s.%s", ID3V2_COVER_BASENAME, k_exts[i]);
        if (n <= 0 || (size_t)n >= sizeof(file_name)) {
            continue;
        }

        char full_path[AUDIO_BACKEND_PATH_MAX];
        if (!path_join3(full_path, sizeof(full_path), AUDIO_MOUNT_POINT, story_folder, file_name)) {
            continue;
        }
        (void)remove(full_path);
    }
}

static bool extract_story_cover_from_id3(const char *mp3_path,
                                         const char *story_folder,
                                         char *out_file_name,
                                         size_t out_file_name_size,
                                         char *out_file_path,
                                         size_t out_file_path_size,
                                         const char **out_skip_reason)
{
    if (!mp3_path || !story_folder || !out_file_name || !out_file_path ||
        out_file_name_size == 0 || out_file_path_size == 0) {
        return false;
    }

    out_file_name[0] = '\0';
    out_file_path[0] = '\0';
    if (out_skip_reason) {
        *out_skip_reason = NULL;
    }

    FILE *fp = fopen(mp3_path, "rb");
    if (!fp) {
        return false;
    }

    uint8_t hdr[10];
    size_t hdr_read = fread(hdr, 1, sizeof(hdr), fp);
    if (hdr_read < sizeof(hdr)) {
        fclose(fp);
        return false;
    }

    size_t tag_total = get_id3v2_total_size(hdr);
    if (tag_total <= 10 || tag_total > ID3V2_MAX_TAG_BYTES) {
        fclose(fp);
        return false;
    }

    uint8_t version_major = hdr[3];
    if (version_major != 3 && version_major != 4) {
        fclose(fp);
        return false;
    }

    const bool global_unsync = (hdr[5] & 0x80u) != 0;
    const bool has_footer = (hdr[5] & 0x10u) != 0;
    size_t payload_size = tag_total - 10u - (has_footer ? 10u : 0u);
    if (payload_size == 0 || payload_size > ID3V2_MAX_TAG_BYTES) {
        fclose(fp);
        return false;
    }

    uint8_t *tag_buf = (uint8_t *)malloc(payload_size);
    if (!tag_buf) {
        fclose(fp);
        return false;
    }

    size_t payload_read = fread(tag_buf, 1, payload_size, fp);
    fclose(fp);
    if (payload_read != payload_size) {
        free(tag_buf);
        return false;
    }

    uint8_t *work_buf = tag_buf;
    size_t work_size = payload_size;

    if (global_unsync) {
        uint8_t *unsync_buf = (uint8_t *)malloc(payload_size);
        if (!unsync_buf) {
            free(tag_buf);
            return false;
        }
        size_t out_n = 0;
        for (size_t i = 0; i < payload_size; i++) {
            unsync_buf[out_n++] = tag_buf[i];
            if (tag_buf[i] == 0xFF && (i + 1) < payload_size && tag_buf[i + 1] == 0x00) {
                i++;
            }
        }
        free(tag_buf);
        work_buf = unsync_buf;
        work_size = out_n;
    }

    const uint8_t *best_img_data = NULL;
    size_t best_img_size = 0;
    const char *best_ext = NULL;
    int best_rank = -1;

    size_t off = 0;
    while (off + 10u <= work_size) {
        const uint8_t *frame_hdr = &work_buf[off];
        if (frame_hdr[0] == 0 && frame_hdr[1] == 0 && frame_hdr[2] == 0 && frame_hdr[3] == 0) {
            break; /* padding */
        }
        if (!is_valid_id3_frame_id4(frame_hdr)) {
            off++;
            continue;
        }

        uint32_t frame_size = (version_major == 4) ? id3v2_syncsafe32(&frame_hdr[4]) : id3v2_be32(&frame_hdr[4]);
        if (frame_size == 0) {
            off += 10u;
            continue;
        }

        size_t frame_total = 10u + (size_t)frame_size;
        if (frame_total > (work_size - off)) {
            off++;
            continue;
        }

        if (memcmp(frame_hdr, "APIC", 4) == 0) {
            id3_apic_frame_t apic = {0};
            if (parse_apic_frame_payload(&frame_hdr[10], frame_size, &apic)) {
                const char *ext = detect_image_ext(apic.mime, apic.data, apic.size);
                if (ext) {
                    int rank = (apic.picture_type == 3u) ? 2 : 1; /* prefer "front cover" */
                    if (rank > best_rank) {
                        best_rank = rank;
                        best_img_data = apic.data;
                        best_img_size = apic.size;
                        best_ext = ext;
                        if (best_rank >= 2) {
                            break;
                        }
                    }
                }
            }
        }

        off += frame_total;
    }

    if (!best_img_data || !best_ext || best_img_size == 0) {
        free(work_buf);
        return false;
    }

    char id3_cover_name[AUDIO_BACKEND_TITLE_MAX];
    char id3_cover_path[AUDIO_BACKEND_PATH_MAX];
    if (!build_story_id3_cover_paths(story_folder,
                                     best_ext,
                                     id3_cover_name,
                                     sizeof(id3_cover_name),
                                     id3_cover_path,
                                     sizeof(id3_cover_path))) {
        free(work_buf);
        return false;
    }

    remove_story_id3_cover_alternates(story_folder, best_ext);

    bool write_needed = true;
    struct stat st = {0};
    if (stat(id3_cover_path, &st) == 0 && S_ISREG(st.st_mode) && (size_t)st.st_size == best_img_size) {
        write_needed = false;
    }

    if (write_needed) {
        FILE *cover_fp = fopen(id3_cover_path, "wb");
        if (!cover_fp) {
            free(work_buf);
            return false;
        }
        size_t written = fwrite(best_img_data, 1, best_img_size, cover_fp);
        fclose(cover_fp);
        if (written != best_img_size) {
            (void)remove(id3_cover_path);
            free(work_buf);
            return false;
        }
    }

    strlcpy(out_file_name, id3_cover_name, out_file_name_size);
    strlcpy(out_file_path, id3_cover_path, out_file_path_size);
    free(work_buf);
    return true;
}

static bool build_meta_fallback_path(const audio_backend_story_meta_t *story, char *out, size_t out_size)
{
    if (!story || !out) {
        return false;
    }
    return path_join3(out, out_size, AUDIO_MOUNT_POINT, story->folder_name, "META.JSN");
}

static void sanitize_chapter_title(const char *file_name, char *out_title, size_t out_size)
{
    if (!out_title || out_size == 0) {
        return;
    }

    out_title[0] = '\0';
    if (!file_name || file_name[0] == '\0') {
        return;
    }

    char tmp[AUDIO_BACKEND_TITLE_MAX];
    strlcpy(tmp, file_name, sizeof(tmp));

    char *dot = strrchr(tmp, '.');
    if (dot) {
        *dot = '\0';
    }

    size_t start = 0;
    while (tmp[start] != '\0' &&
           (isdigit((unsigned char)tmp[start]) ||
            tmp[start] == ' ' || tmp[start] == '_' ||
            tmp[start] == '-' || tmp[start] == '.')) {
        start++;
    }

    const char *src = &tmp[start];
    size_t out_idx = 0;
    bool prev_space = false;
    while (*src != '\0' && out_idx + 1 < out_size) {
        char c = *src++;
        if (c == '_' || c == '-') {
            c = ' ';
        }
        if (c == ' ') {
            if (prev_space) {
                continue;
            }
            prev_space = true;
        } else {
            prev_space = false;
        }
        out_title[out_idx++] = c;
    }

    while (out_idx > 0 && out_title[out_idx - 1] == ' ') {
        out_idx--;
    }
    out_title[out_idx] = '\0';

    if (out_idx == 0) {
        strlcpy(out_title, file_name, out_size);
    }
}

static void sort_file_names(char names[AUDIO_BACKEND_MAX_CHAPTERS][AUDIO_BACKEND_TITLE_MAX], uint8_t count)
{
    for (uint8_t i = 1; i < count; i++) {
        char key[AUDIO_BACKEND_TITLE_MAX];
        strlcpy(key, names[i], sizeof(key));

        int j = (int)i - 1;
        while (j >= 0 && chapter_name_cmp(names[j], key) > 0) {
            strlcpy(names[j + 1], names[j], AUDIO_BACKEND_TITLE_MAX);
            j--;
        }
        strlcpy(names[j + 1], key, AUDIO_BACKEND_TITLE_MAX);
    }
}

static void story_json_load(audio_backend_story_meta_t *story)
{
    if (!story || story->json_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(story->json_path, "rb");
    if (!fp) {
        char fallback_path[AUDIO_BACKEND_PATH_MAX];
        if (!build_meta_fallback_path(story, fallback_path, sizeof(fallback_path))) {
            return;
        }
        fp = fopen(fallback_path, "rb");
        if (!fp) {
            return;
        }
        ESP_LOGI(TAG, "Using fallback metadata file: %s", fallback_path);
        strlcpy(story->json_path, fallback_path, sizeof(story->json_path));
    }

    char *buf = (char *)malloc(STORY_JSON_MAX_BYTES + 1);
    if (!buf) {
        fclose(fp);
        return;
    }

    size_t n = fread(buf, 1, STORY_JSON_MAX_BYTES, fp);
    fclose(fp);
    buf[n] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, n);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON metadata: %s", story->json_path);
        return;
    }

    cJSON *play = cJSON_GetObjectItemCaseSensitive(root, "play_count");
    if (cJSON_IsNumber(play) && play->valuedouble >= 0.0) {
        story->play_count = (uint32_t)play->valuedouble;
    }

    cJSON *resume = cJSON_GetObjectItemCaseSensitive(root, "resume_time_sec");
    if (cJSON_IsNumber(resume) && resume->valuedouble >= 0.0) {
        story->resume_time_sec = (uint32_t)resume->valuedouble;
    }
    cJSON *resume_chapter = cJSON_GetObjectItemCaseSensitive(root, "resume_chapter");
    if (cJSON_IsNumber(resume_chapter) && resume_chapter->valuedouble >= 0.0) {
        uint32_t chapter = (uint32_t)resume_chapter->valuedouble;
        if (chapter < story->chapter_count) {
            story->resume_chapter_index = (uint8_t)chapter;
        } else {
            story->resume_chapter_index = 0;
        }
    }

    cJSON *chapters = cJSON_GetObjectItemCaseSensitive(root, "chapters");
    if (cJSON_IsArray(chapters)) {
        cJSON *ch = NULL;
        cJSON_ArrayForEach(ch, chapters) {
            if (!cJSON_IsObject(ch)) {
                continue;
            }

            int chapter_idx = -1;
            cJSON *idx = cJSON_GetObjectItemCaseSensitive(ch, "index");
            if (cJSON_IsNumber(idx)) {
                int parsed = (int)idx->valuedouble;
                if (parsed >= 0 && parsed < story->chapter_count) {
                    chapter_idx = parsed;
                }
            }

            if (chapter_idx < 0) {
                cJSON *file = cJSON_GetObjectItemCaseSensitive(ch, "file");
                if (cJSON_IsString(file) && file->valuestring) {
                    for (uint8_t i = 0; i < story->chapter_count; i++) {
                        if (ci_string_cmp(story->chapters[i].file_name, file->valuestring) == 0) {
                            chapter_idx = i;
                            break;
                        }
                    }
                }
            }

            if (chapter_idx < 0 || chapter_idx >= story->chapter_count) {
                continue;
            }

            cJSON *title = cJSON_GetObjectItemCaseSensitive(ch, "title");
            if (cJSON_IsString(title) && title->valuestring && title->valuestring[0] != '\0') {
                strlcpy(story->chapters[chapter_idx].title,
                        title->valuestring,
                        sizeof(story->chapters[chapter_idx].title));
            }

            cJSON *duration = cJSON_GetObjectItemCaseSensitive(ch, "duration_sec");
            if (cJSON_IsNumber(duration) && duration->valuedouble > 0.0) {
                story->chapters[chapter_idx].duration_sec = (uint32_t)duration->valuedouble;
            }
        }
    }

    cJSON_Delete(root);
}

static void story_json_save(audio_backend_story_meta_t *story)
{
    if (!story || story->json_path[0] == '\0') {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "title", story->title);
    cJSON_AddStringToObject(root, "folder", story->folder_name);
    cJSON_AddStringToObject(root, "album_art", story->album_art_file);
    cJSON_AddNumberToObject(root, "play_count", story->play_count);
    cJSON_AddNumberToObject(root, "resume_time_sec", story->resume_time_sec);
    cJSON_AddNumberToObject(root, "resume_chapter", story->resume_chapter_index);

    cJSON *chapters = cJSON_AddArrayToObject(root, "chapters");
    if (chapters) {
        for (uint8_t i = 0; i < story->chapter_count; i++) {
            cJSON *ch = cJSON_CreateObject();
            if (!ch) {
                continue;
            }
            cJSON_AddNumberToObject(ch, "index", i);
            cJSON_AddStringToObject(ch, "file", story->chapters[i].file_name);
            cJSON_AddStringToObject(ch, "title", story->chapters[i].title);
            cJSON_AddNumberToObject(ch, "duration_sec", story->chapters[i].duration_sec);
            cJSON_AddItemToArray(chapters, ch);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        return;
    }

    FILE *fp = fopen(story->json_path, "wb");
    if (!fp) {
        int open_errno = errno;
        char fallback_path[AUDIO_BACKEND_PATH_MAX];
        if ((open_errno == EINVAL || open_errno == ENAMETOOLONG) &&
            build_meta_fallback_path(story, fallback_path, sizeof(fallback_path))) {
            fp = fopen(fallback_path, "wb");
            if (fp) {
                strlcpy(story->json_path, fallback_path, sizeof(story->json_path));
                ESP_LOGW(TAG, "Metadata filename fallback active: %s", story->json_path);
            }
        }
    }
    if (!fp) {
        ESP_LOGW(TAG, "Cannot write metadata %s (errno=%d)", story->json_path, errno);
        cJSON_free(json_str);
        return;
    }

    fwrite(json_str, 1, strlen(json_str), fp);
    fwrite("\n", 1, 1, fp);
    fclose(fp);
    cJSON_free(json_str);
}

static bool scan_story_folder(const char *folder_name, audio_backend_story_meta_t *out_story)
{
    if (!folder_name || !out_story) {
        return false;
    }

    char folder_path[AUDIO_BACKEND_PATH_MAX];
    if (!path_join2(folder_path, sizeof(folder_path), AUDIO_MOUNT_POINT, folder_name)) {
        ESP_LOGW(TAG, "Story folder path too long: %s", folder_name);
        return false;
    }
    if (!is_dir_path(folder_path)) {
        return false;
    }

    DIR *dir = opendir(folder_path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open story folder: %s", folder_path);
        return false;
    }

    if (strlen(folder_name) >= AUDIO_BACKEND_TITLE_MAX) {
        ESP_LOGW(TAG, "Skipping story folder with long name: %s", folder_name);
        closedir(dir);
        return false;
    }

    memset(out_story, 0, sizeof(*out_story));
    strlcpy(out_story->folder_name, folder_name, sizeof(out_story->folder_name));
    strlcpy(out_story->title, folder_name, sizeof(out_story->title));
    char json_name[AUDIO_BACKEND_TITLE_MAX + 6];
    strlcpy(json_name, out_story->folder_name, sizeof(json_name));
    strlcat(json_name, ".json", sizeof(json_name));
    if (!path_join3(out_story->json_path, sizeof(out_story->json_path),
                    AUDIO_MOUNT_POINT, out_story->folder_name, json_name)) {
        ESP_LOGW(TAG, "JSON metadata path too long for folder: %s", out_story->folder_name);
        out_story->json_path[0] = '\0';
    }

    char mp3_names[AUDIO_BACKEND_MAX_CHAPTERS][AUDIO_BACKEND_TITLE_MAX] = {{0}};
    uint8_t mp3_count = 0;
    char best_cover_name[AUDIO_BACKEND_TITLE_MAX] = {0};
    int best_cover_score = -1;

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (is_dot_entry(ent->d_name)) {
            continue;
        }

        if (has_mp3_extension(ent->d_name)) {
            if (strlen(ent->d_name) >= AUDIO_BACKEND_TITLE_MAX) {
                ESP_LOGW(TAG, "Skipping long MP3 file name in %s: %s", folder_name, ent->d_name);
                continue;
            }
            if (mp3_count < AUDIO_BACKEND_MAX_CHAPTERS) {
                strlcpy(mp3_names[mp3_count], ent->d_name, sizeof(mp3_names[mp3_count]));
                mp3_count++;
            }
            continue;
        }

        if (has_supported_cover_extension(ent->d_name) || has_jpeg_extension(ent->d_name)) {
            if (strlen(ent->d_name) >= AUDIO_BACKEND_TITLE_MAX) {
                continue;
            }
            int score = cover_file_score(ent->d_name);
            if (score > best_cover_score) {
                best_cover_score = score;
                strlcpy(best_cover_name, ent->d_name, sizeof(best_cover_name));
            }
        }
    }

    closedir(dir);

    if (mp3_count == 0) {
        return false;
    }

    sort_file_names(mp3_names, mp3_count);
    uint8_t valid_chapters = 0;
    for (uint8_t i = 0; i < mp3_count; i++) {
        if (valid_chapters >= AUDIO_BACKEND_MAX_CHAPTERS) {
            break;
        }
        if (!path_join3(out_story->chapters[valid_chapters].file_path,
                        sizeof(out_story->chapters[valid_chapters].file_path),
                        AUDIO_MOUNT_POINT, out_story->folder_name, mp3_names[i])) {
            continue;
        }
        strlcpy(out_story->chapters[valid_chapters].file_name,
                mp3_names[i],
                sizeof(out_story->chapters[valid_chapters].file_name));
        sanitize_chapter_title(mp3_names[i],
                               out_story->chapters[valid_chapters].title,
                               sizeof(out_story->chapters[valid_chapters].title));
        out_story->chapters[valid_chapters].duration_sec = 0;
        valid_chapters++;
    }

    out_story->chapter_count = valid_chapters;
    if (out_story->chapter_count == 0) {
        return false;
    }

    if (best_cover_score >= 0 && best_cover_name[0] != '\0') {
        strlcpy(out_story->album_art_file, best_cover_name, sizeof(out_story->album_art_file));
        if (!path_join3(out_story->album_art_path, sizeof(out_story->album_art_path),
                        AUDIO_MOUNT_POINT, out_story->folder_name, best_cover_name)) {
            out_story->album_art_path[0] = '\0';
        }
        if (has_jpeg_extension(best_cover_name)) {
            ESP_LOGW(TAG, "Cover source fallback uses .jpeg (less compatible), prefer .png or .jpg: %s/%s",
                     out_story->folder_name, best_cover_name);
        } else {
            ESP_LOGI(TAG, "Cover source: file fallback (%s)", best_cover_name);
        }
    } else {
        for (uint8_t ch = 0; ch < out_story->chapter_count; ch++) {
            if (extract_story_cover_from_id3(out_story->chapters[ch].file_path,
                                             out_story->folder_name,
                                             out_story->album_art_file,
                                             sizeof(out_story->album_art_file),
                                             out_story->album_art_path,
                                             sizeof(out_story->album_art_path),
                                             NULL)) {
                ESP_LOGI(TAG, "Cover source: ID3 APIC (%s)", out_story->chapters[ch].file_name);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    story_json_load(out_story);

    for (uint8_t ch = 0; ch < out_story->chapter_count; ch++) {
        if (out_story->chapters[ch].duration_sec != 0) {
            continue;
        }
        out_story->chapters[ch].duration_sec =
            estimate_mp3_duration_sec(out_story->chapters[ch].file_path);
    }

    story_json_save(out_story);

    return true;
}

static void scan_sd_catalog(void)
{
    memset(s_stories, 0, sizeof(s_stories));
    s_story_count = 0;
    s_first_mp3[0] = '\0';

    DIR *root = opendir(AUDIO_MOUNT_POINT);
    if (!root) {
        ESP_LOGW(TAG, "Cannot open %s for scan", AUDIO_MOUNT_POINT);
        return;
    }

    struct dirent *ent = NULL;
    uint32_t root_mp3_found = 0;
    while ((ent = readdir(root)) != NULL) {
        if (is_dot_entry(ent->d_name)) {
            continue;
        }

        char full_path[AUDIO_BACKEND_PATH_MAX];
        if (!path_join2(full_path, sizeof(full_path), AUDIO_MOUNT_POINT, ent->d_name)) {
            continue;
        }

        if (is_dir_path(full_path)) {
            if (s_story_count >= AUDIO_BACKEND_MAX_STORIES) {
                continue;
            }
            if (scan_story_folder(ent->d_name, &s_stories[s_story_count])) {
                s_story_count++;
            }
            continue;
        }

        if (has_mp3_extension(ent->d_name)) {
            root_mp3_found++;
            if (s_first_mp3[0] == '\0') {
                strlcpy(s_first_mp3, full_path, sizeof(s_first_mp3));
            }
        }
    }
    closedir(root);

    if (s_story_count > 1) {
        qsort(s_stories, s_story_count, sizeof(s_stories[0]), qsort_story_cmp);
    }
    for (uint8_t i = 0; i < s_story_count; i++) {
        s_stories[i].id = i;
        ESP_LOGI(TAG, "Story[%u] %s (%u chapter%s)%s",
                 (unsigned)i,
                 s_stories[i].title,
                 (unsigned)s_stories[i].chapter_count,
                 (s_stories[i].chapter_count == 1) ? "" : "s",
                 s_stories[i].album_art_file[0] ? " + cover" : "");
    }

    if (s_story_count == 0) {
        if (root_mp3_found > 0) {
            ESP_LOGW(TAG, "No story folders detected, using legacy root MP3 fallback");
        } else {
            ESP_LOGW(TAG, "No stories found on %s", AUDIO_MOUNT_POINT);
        }
    } else {
        ESP_LOGI(TAG, "Catalog scan complete: %u storie(s)", (unsigned)s_story_count);
    }
}

static void persist_active_story_progress(bool playback_finished)
{
    if (s_active_story_index < 0 || (uint8_t)s_active_story_index >= s_story_count) {
        return;
    }
    audio_backend_story_meta_t *story = &s_stories[s_active_story_index];
    if (playback_finished) {
        story->resume_time_sec = 0;
        story->resume_chapter_index = 0;
    } else {
        story->resume_time_sec = playback_elapsed_sec();
        if (s_active_chapter_index >= 0 &&
            (uint8_t)s_active_chapter_index < story->chapter_count) {
            story->resume_chapter_index = (uint8_t)s_active_chapter_index;
        } else {
            story->resume_chapter_index = 0;
        }
    }
    story_json_save(story);
}

/*
 * Perceived loudness grows roughly with amplitude^0.6, so a linear slider
 * jumps at the bottom and flattens out above the middle. Squaring the slider
 * position gives an approximately even loudness step across 0..100
 * (50 -> -12 dB, 25 -> -24 dB, 10 -> -40 dB). Returns Q15 gain.
 */
static int32_t volume_to_gain_q15(uint8_t volume)
{
    if (volume >= 100) {
        return 32768;
    }
    return ((int32_t)volume * (int32_t)volume * 32768) / 10000;
}

static int16_t apply_volume(int16_t sample, int32_t gain_q15)
{
    int32_t scaled = ((int32_t)sample * gain_q15) >> 15;
    if (scaled > INT16_MAX) {
        scaled = INT16_MAX;
    } else if (scaled < INT16_MIN) {
        scaled = INT16_MIN;
    }
    return (int16_t)scaled;
}

static int16_t downmix_stereo_to_mono(int16_t left, int16_t right)
{
    int32_t sum = (int32_t)left + (int32_t)right;
    int32_t mixed = (sum * 23170) / 32768; /* 1/sqrt(2), equal-power mono */
    if (mixed > INT16_MAX) {
        return INT16_MAX;
    }
    if (mixed < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)mixed;
}

typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1[2];
    float z2[2];
    bool bypass;
} tone_biquad_t;

typedef struct {
    tone_biquad_t bass;
    tone_biquad_t treble;
    bool active;
    uint32_t sample_rate_hz;
    uint32_t revision;
    uint8_t volume;
    bool global_enabled;
} tone_runtime_t;

typedef struct {
    tone_biquad_t protector;
    tone_biquad_t fullness;
    tone_biquad_t presence;
    bool active;
    bool enabled;
    uint8_t amount;
    uint8_t volume;
    uint32_t sample_rate_hz;
    uint32_t revision;
    float harmonic_lp_alpha;
    float harmonic_hp_alpha;
    float harmonic_low;
    float harmonic_prev_x;
    float harmonic_prev_y;
    float harmonic_mix;
    float post_gain;
} sound_enricher_runtime_t;

static void tone_biquad_reset_state(tone_biquad_t *bq)
{
    if (!bq) {
        return;
    }
    bq->z1[0] = 0.0f;
    bq->z1[1] = 0.0f;
    bq->z2[0] = 0.0f;
    bq->z2[1] = 0.0f;
}

static void tone_biquad_set_identity(tone_biquad_t *bq)
{
    if (!bq) {
        return;
    }
    bq->b0 = 1.0f;
    bq->b1 = 0.0f;
    bq->b2 = 0.0f;
    bq->a1 = 0.0f;
    bq->a2 = 0.0f;
    bq->bypass = true;
    tone_biquad_reset_state(bq);
}

static void tone_biquad_set_low_shelf(tone_biquad_t *bq, float fs_hz, float f0_hz, float gain_db)
{
    if (!bq || fs_hz <= 1000.0f || f0_hz <= 1.0f || f0_hz >= (fs_hz * 0.45f) || fabsf(gain_db) < 0.01f) {
        tone_biquad_set_identity(bq);
        return;
    }

    const float slope = 1.0f;
    const float A = powf(10.0f, gain_db / 40.0f);
    const float w0 = 2.0f * (float)M_PI * (f0_hz / fs_hz);
    const float sin_w0 = sinf(w0);
    const float cos_w0 = cosf(w0);
    const float alpha = (sin_w0 * 0.5f) * sqrtf((A + (1.0f / A)) * ((1.0f / slope) - 1.0f) + 2.0f);
    const float sqrt_A = sqrtf(A);

    const float b0 = A * ((A + 1.0f) - (A - 1.0f) * cos_w0 + 2.0f * sqrt_A * alpha);
    const float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w0);
    const float b2 = A * ((A + 1.0f) - (A - 1.0f) * cos_w0 - 2.0f * sqrt_A * alpha);
    const float a0 = (A + 1.0f) + (A - 1.0f) * cos_w0 + 2.0f * sqrt_A * alpha;
    const float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w0);
    const float a2 = (A + 1.0f) + (A - 1.0f) * cos_w0 - 2.0f * sqrt_A * alpha;

    if (fabsf(a0) < 1e-9f) {
        tone_biquad_set_identity(bq);
        return;
    }

    bq->b0 = b0 / a0;
    bq->b1 = b1 / a0;
    bq->b2 = b2 / a0;
    bq->a1 = a1 / a0;
    bq->a2 = a2 / a0;
    bq->bypass = false;
    tone_biquad_reset_state(bq);
}

static void tone_biquad_set_high_shelf(tone_biquad_t *bq, float fs_hz, float f0_hz, float gain_db)
{
    if (!bq || fs_hz <= 1000.0f || f0_hz <= 1.0f || f0_hz >= (fs_hz * 0.45f) || fabsf(gain_db) < 0.01f) {
        tone_biquad_set_identity(bq);
        return;
    }

    const float slope = 1.0f;
    const float A = powf(10.0f, gain_db / 40.0f);
    const float w0 = 2.0f * (float)M_PI * (f0_hz / fs_hz);
    const float sin_w0 = sinf(w0);
    const float cos_w0 = cosf(w0);
    const float alpha = (sin_w0 * 0.5f) * sqrtf((A + (1.0f / A)) * ((1.0f / slope) - 1.0f) + 2.0f);
    const float sqrt_A = sqrtf(A);

    const float b0 = A * ((A + 1.0f) + (A - 1.0f) * cos_w0 + 2.0f * sqrt_A * alpha);
    const float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0);
    const float b2 = A * ((A + 1.0f) + (A - 1.0f) * cos_w0 - 2.0f * sqrt_A * alpha);
    const float a0 = (A + 1.0f) - (A - 1.0f) * cos_w0 + 2.0f * sqrt_A * alpha;
    const float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0);
    const float a2 = (A + 1.0f) - (A - 1.0f) * cos_w0 - 2.0f * sqrt_A * alpha;

    if (fabsf(a0) < 1e-9f) {
        tone_biquad_set_identity(bq);
        return;
    }

    bq->b0 = b0 / a0;
    bq->b1 = b1 / a0;
    bq->b2 = b2 / a0;
    bq->a1 = a1 / a0;
    bq->a2 = a2 / a0;
    bq->bypass = false;
    tone_biquad_reset_state(bq);
}

static void tone_biquad_set_high_pass(tone_biquad_t *bq, float fs_hz, float f0_hz, float q)
{
    if (!bq || fs_hz <= 1000.0f || f0_hz <= 1.0f || f0_hz >= (fs_hz * 0.45f) || q <= 0.01f) {
        tone_biquad_set_identity(bq);
        return;
    }

    const float w0 = 2.0f * (float)M_PI * (f0_hz / fs_hz);
    const float sin_w0 = sinf(w0);
    const float cos_w0 = cosf(w0);
    const float alpha = sin_w0 / (2.0f * q);

    const float b0 = (1.0f + cos_w0) * 0.5f;
    const float b1 = -(1.0f + cos_w0);
    const float b2 = (1.0f + cos_w0) * 0.5f;
    const float a0 = 1.0f + alpha;
    const float a1 = -2.0f * cos_w0;
    const float a2 = 1.0f - alpha;

    if (fabsf(a0) < 1e-9f) {
        tone_biquad_set_identity(bq);
        return;
    }

    bq->b0 = b0 / a0;
    bq->b1 = b1 / a0;
    bq->b2 = b2 / a0;
    bq->a1 = a1 / a0;
    bq->a2 = a2 / a0;
    bq->bypass = false;
    tone_biquad_reset_state(bq);
}

static inline float tone_biquad_process_sample(tone_biquad_t *bq, float x, uint8_t ch)
{
    if (!bq || bq->bypass || ch > 1u) {
        return x;
    }
    float y = (bq->b0 * x) + bq->z1[ch];
    bq->z1[ch] = (bq->b1 * x) - (bq->a1 * y) + bq->z2[ch];
    bq->z2[ch] = (bq->b2 * x) - (bq->a2 * y);
    return y;
}

static inline int16_t tone_clamp_to_i16(float x)
{
    if (x > 32767.0f) {
        return INT16_MAX;
    }
    if (x < -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)x;
}

static float tone_level_to_db(uint8_t level)
{
    if (level > 100u) {
        level = 100u;
    }
    return ((float)level - 50.0f) * (12.0f / 50.0f);
}

static void tone_runtime_configure(tone_runtime_t *rt, uint32_t sample_rate_hz, uint8_t volume)
{
    if (!rt || sample_rate_hz == 0u) {
        return;
    }

    const bool global_enabled = s_tone_enabled;
    const uint8_t bass = s_tone_bass;
    const uint8_t treble = s_tone_treble;
    const uint32_t revision = s_tone_revision;
    const float bass_db = tone_level_to_db(bass);
    const float treble_db = tone_level_to_db(treble);
    const bool active = global_enabled &&
        ((fabsf(bass_db) >= 0.01f) || (fabsf(treble_db) >= 0.01f));

    if (rt->revision == revision &&
        rt->sample_rate_hz == sample_rate_hz &&
        rt->active == active &&
        rt->volume == volume &&
        rt->global_enabled == global_enabled) {
        return;
    }

    rt->revision = revision;
    rt->sample_rate_hz = sample_rate_hz;
    rt->active = active;
    rt->volume = volume;
    rt->global_enabled = global_enabled;

    if (!active) {
        tone_biquad_set_identity(&rt->bass);
        tone_biquad_set_identity(&rt->treble);
        return;
    }

    const float fs = (float)sample_rate_hz;
    tone_biquad_set_low_shelf(&rt->bass, fs, 120.0f, bass_db);
    tone_biquad_set_high_shelf(&rt->treble, fs, 6000.0f, treble_db);
}

static void tone_runtime_process_mono_dup(tone_runtime_t *rt, int16_t *pcm, size_t frames)
{
    if (!rt || !pcm || frames == 0u || !rt->active) {
        return;
    }

    for (size_t i = 0; i < frames; i++) {
        float m = (float)pcm[i * 2u];

        m = tone_biquad_process_sample(&rt->bass, m, 0u);
        m = tone_biquad_process_sample(&rt->treble, m, 0u);
        int16_t out = tone_clamp_to_i16(m);
        pcm[i * 2u] = out;
        pcm[i * 2u + 1u] = out;
    }
}

static void sound_enricher_runtime_configure(sound_enricher_runtime_t *rt, uint32_t sample_rate_hz, uint8_t volume)
{
    if (!rt || sample_rate_hz == 0u) {
        return;
    }

    const bool enabled = s_sound_enricher_enabled;
    const uint8_t amount = (s_sound_enricher_amount > 100u) ? 100u : s_sound_enricher_amount;
    const uint32_t revision = s_tone_revision;
    const bool active = s_tone_enabled && enabled && amount > 0u;

    if (rt->revision == revision &&
        rt->sample_rate_hz == sample_rate_hz &&
        rt->enabled == enabled &&
        rt->amount == amount &&
        rt->volume == volume &&
        rt->active == active) {
        return;
    }

    rt->revision = revision;
    rt->sample_rate_hz = sample_rate_hz;
    rt->enabled = enabled;
    rt->amount = amount;
    rt->volume = volume;
    rt->active = active;
    rt->harmonic_low = 0.0f;
    rt->harmonic_prev_x = 0.0f;
    rt->harmonic_prev_y = 0.0f;

    if (!active) {
        tone_biquad_set_identity(&rt->protector);
        tone_biquad_set_identity(&rt->fullness);
        tone_biquad_set_identity(&rt->presence);
        rt->harmonic_lp_alpha = 0.0f;
        rt->harmonic_hp_alpha = 0.0f;
        rt->harmonic_mix = 0.0f;
        rt->post_gain = 1.0f;
        return;
    }

    const float fs = (float)sample_rate_hz;
    float profile = 0.0f;
    if (amount <= 40u) {
        profile = ((float)amount / 40.0f) * 0.45f;          /* Warm */
    } else if (amount <= 75u) {
        profile = 0.45f + (((float)amount - 40.0f) / 35.0f) * 0.35f; /* Full */
    } else {
        profile = 0.80f + (((float)amount - 75.0f) / 25.0f) * 0.32f; /* Punch */
    }
    if (profile > 1.12f) {
        profile = 1.12f;
    }

    float low_volume = 0.0f;
    if (volume <= 45u) {
        low_volume = 1.0f;
    } else if (volume < 72u) {
        low_volume = (72.0f - (float)volume) / 27.0f;
    }

    const float strength = profile * low_volume;
    const float protect_hz = 56.0f + (16.0f * strength);
    const float fullness_db = 10.5f * strength;
    const float presence_db = 3.6f * strength;

    if (strength > 0.005f) {
        tone_biquad_set_high_pass(&rt->protector, fs, protect_hz, 0.707f);
        tone_biquad_set_low_shelf(&rt->fullness, fs, 175.0f, fullness_db);
        tone_biquad_set_high_shelf(&rt->presence, fs, 4800.0f, presence_db);
    } else {
        tone_biquad_set_identity(&rt->protector);
        tone_biquad_set_identity(&rt->fullness);
        tone_biquad_set_identity(&rt->presence);
    }

    rt->harmonic_lp_alpha = 1.0f - expf(-2.0f * (float)M_PI * 125.0f / fs);
    const float hp_rc = 1.0f / (2.0f * (float)M_PI * 78.0f);
    const float dt = 1.0f / fs;
    rt->harmonic_hp_alpha = hp_rc / (hp_rc + dt);
    rt->harmonic_mix = 0.38f * strength;
    rt->post_gain = 1.0f + (0.15f * strength);
}

static float sound_enricher_soft_limit(float x)
{
    const float threshold = 30000.0f;
    const float sign = (x < 0.0f) ? -1.0f : 1.0f;
    float ax = fabsf(x);
    if (ax <= threshold) {
        return x;
    }
    float over = (ax - threshold) / (32767.0f - threshold);
    ax = threshold + (32767.0f - threshold) * (over / (1.0f + over));
    return sign * ax;
}

static void sound_enricher_runtime_process(sound_enricher_runtime_t *rt, int16_t *pcm, size_t frames)
{
    if (!rt || !pcm || frames == 0u || !rt->active) {
        return;
    }

    for (size_t i = 0; i < frames; i++) {
        const size_t lidx = i * 2u;
        const size_t ridx = lidx + 1u;
        float l = (float)pcm[lidx];
        float r = (float)pcm[ridx];

        l = tone_biquad_process_sample(&rt->protector, l, 0u);
        r = tone_biquad_process_sample(&rt->protector, r, 1u);

        float mono = (l + r) * 0.5f;
        rt->harmonic_low += rt->harmonic_lp_alpha * (mono - rt->harmonic_low);
        float generated = 0.0f;
        if (fabsf(rt->harmonic_low) > 280.0f) {
            generated = fabsf(rt->harmonic_low) - 280.0f;
        }
        float harmonic = rt->harmonic_hp_alpha *
            (rt->harmonic_prev_y + generated - rt->harmonic_prev_x);
        rt->harmonic_prev_x = generated;
        rt->harmonic_prev_y = harmonic;

        l += harmonic * rt->harmonic_mix;
        r += harmonic * rt->harmonic_mix;

        l = tone_biquad_process_sample(&rt->fullness, l, 0u);
        r = tone_biquad_process_sample(&rt->fullness, r, 1u);
        l = tone_biquad_process_sample(&rt->presence, l, 0u);
        r = tone_biquad_process_sample(&rt->presence, r, 1u);

        l *= rt->post_gain;
        r *= rt->post_gain;

        pcm[lidx] = tone_clamp_to_i16(sound_enricher_soft_limit(l));
        pcm[ridx] = tone_clamp_to_i16(sound_enricher_soft_limit(r));
    }
}

static esp_err_t resolve_chapter_path(uint8_t story_id, uint8_t chapter_index, char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (story_id < s_story_count) {
        const audio_backend_story_meta_t *story = &s_stories[story_id];
        if (chapter_index < story->chapter_count) {
            strlcpy(out_path, story->chapters[chapter_index].file_path, out_size);
            return ESP_OK;
        }
    }

    struct stat st = {0};
    char chapter_path[AUDIO_PATH_MAX];
    char chapter_path_human[AUDIO_PATH_MAX];

    snprintf(chapter_path, sizeof(chapter_path), "%s/%u_%u.mp3",
             AUDIO_MOUNT_POINT, (unsigned)story_id, (unsigned)chapter_index);
    snprintf(chapter_path_human, sizeof(chapter_path_human), "%s/%u_%u.mp3",
             AUDIO_MOUNT_POINT, (unsigned)story_id, (unsigned)(chapter_index + 1u));

    if (stat(chapter_path, &st) == 0) {
        strlcpy(out_path, chapter_path, out_size);
        return ESP_OK;
    }
    if (stat(chapter_path_human, &st) == 0) {
        strlcpy(out_path, chapter_path_human, out_size);
        return ESP_OK;
    }
    if (s_first_mp3[0] != '\0') {
        strlcpy(out_path, s_first_mp3, out_size);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t ensure_i2s_enabled(void)
{
    if (!s_i2s_ready || !s_i2s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tx_enabled) {
        return ESP_OK;
    }
    esp_err_t ret = i2s_channel_enable(s_i2s_tx);
    if (ret == ESP_OK) {
        s_tx_enabled = true;
    }
    return ret;
}

static esp_err_t reconfigure_i2s_clock(uint32_t sample_rate_hz)
{
    if (!s_i2s_ready || !s_i2s_tx || sample_rate_hz == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate_hz == s_i2s_sample_rate_hz) {
        return ESP_OK;
    }

    bool was_enabled = s_tx_enabled;
    esp_err_t ret = ESP_OK;

    if (was_enabled) {
        ret = i2s_channel_disable(s_i2s_tx);
        if (ret != ESP_OK) {
            return ret;
        }
        s_tx_enabled = false;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ret = i2s_channel_reconfig_std_clock(s_i2s_tx, &clk_cfg);

    if (ret == ESP_OK) {
        s_i2s_sample_rate_hz = sample_rate_hz;
        ESP_LOGI(TAG, "I2S sample rate set to %lu Hz", (unsigned long)sample_rate_hz);
    } else {
        ESP_LOGW(TAG, "I2S clock reconfig failed (%s)", esp_err_to_name(ret));
    }

    if (was_enabled) {
        esp_err_t en_ret = i2s_channel_enable(s_i2s_tx);
        if (en_ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S re-enable failed (%s)", esp_err_to_name(en_ret));
            return en_ret;
        }
        s_tx_enabled = true;
    }

    return ret;
}

static esp_err_t flush_i2s_tx(void)
{
    if (!s_i2s_ready || !s_i2s_tx) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_tx_enabled) {
        esp_err_t ret = i2s_channel_disable(s_i2s_tx);
        if (ret != ESP_OK) {
            return ret;
        }
        s_tx_enabled = false;
    }

    esp_err_t ret = i2s_channel_enable(s_i2s_tx);
    if (ret != ESP_OK) {
        return ret;
    }

    s_tx_enabled = true;
    return ESP_OK;
}

static bool mp3_header_valid(const uint8_t *hdr)
{
    if (!hdr) {
        return false;
    }
    if (hdr[0] != 0xFF || (hdr[1] & 0xE0) != 0xE0) {
        return false;
    }

    uint8_t version_id = (hdr[1] >> 3) & 0x03;
    uint8_t layer = (hdr[1] >> 1) & 0x03;
    uint8_t bitrate_index = (hdr[2] >> 4) & 0x0F;
    uint8_t sample_rate_index = (hdr[2] >> 2) & 0x03;

    return version_id != 1u &&
           layer != 0u &&
           bitrate_index != 0u &&
           bitrate_index != 15u &&
           sample_rate_index != 3u;
}

static unsigned mp3_header_bitrate_kbps(const uint8_t *hdr)
{
    if (!mp3_header_valid(hdr)) {
        return 0;
    }

    static const unsigned mpeg1_l1[16]   = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0};
    static const unsigned mpeg1_l2[16]   = {0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    static const unsigned mpeg1_l3[16]   = {0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const unsigned mpeg2_l1[16]   = {0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256, 0};
    static const unsigned mpeg2_l23[16]  = {0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0};

    uint8_t version_id = (hdr[1] >> 3) & 0x03;
    uint8_t layer = (hdr[1] >> 1) & 0x03;
    uint8_t bitrate_index = (hdr[2] >> 4) & 0x0F;

    if (version_id == 3u) {
        if (layer == 3u) {
            return mpeg1_l1[bitrate_index];
        }
        if (layer == 2u) {
            return mpeg1_l2[bitrate_index];
        }
        return mpeg1_l3[bitrate_index];
    }

    if (layer == 3u) {
        return mpeg2_l1[bitrate_index];
    }
    return mpeg2_l23[bitrate_index];
}

static uint32_t estimate_mp3_duration_sec(const char *path)
{
    if (!path || path[0] == '\0') {
        return 0;
    }

    struct stat st = {0};
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return 0;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }

    size_t id3_skip = skip_id3v2_tag(fp);

    const size_t probe_size = 4096;
    uint8_t *probe = heap_caps_malloc(probe_size, MALLOC_CAP_8BIT);
    if (!probe) {
        fclose(fp);
        return 0;
    }

    size_t n = fread(probe, 1, probe_size, fp);
    fclose(fp);
    if (n < 4) {
        free(probe);
        return 0;
    }

    unsigned kbps = 0;
    for (size_t i = 0; i + 4 <= n; i++) {
        if (!mp3_header_valid(&probe[i])) {
            continue;
        }
        kbps = mp3_header_bitrate_kbps(&probe[i]);
        if (kbps > 0) {
            break;
        }
    }
    free(probe);

    if (kbps == 0) {
        return 0;
    }

    uint64_t audio_bytes = (uint64_t)st.st_size;
    if (id3_skip > 0 && (uint64_t)id3_skip < audio_bytes) {
        audio_bytes -= (uint64_t)id3_skip;
    }

    uint64_t bits = audio_bytes * 8ULL;
    uint64_t denom = (uint64_t)kbps * 1000ULL;
    uint64_t sec = (bits + denom - 1ULL) / denom;
    if (sec > UINT32_MAX) {
        sec = UINT32_MAX;
    }
    return (uint32_t)sec;
}

static esp_err_t stop_play_task_and_wait(TickType_t timeout_ticks)
{
    if (playback_get_task() == NULL) {
        return ESP_OK;
    }

    playback_set_stop_requested(true);
    playback_set_pause_requested(false);

    TickType_t start = xTaskGetTickCount();
    while (playback_get_task() != NULL) {
        if (timeout_ticks != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= timeout_ticks) {
                return ESP_ERR_TIMEOUT;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}

static void mp3_playback_task(void *arg)
{
    (void)arg;
    bool playback_finished = false;
    bool playback_error = false;
    uint32_t sd_read_count = 0;
    uint32_t sd_slow_read_count = 0;
    uint64_t sd_max_read_us = 0;
    uint32_t i2s_timeout_count = 0;

    char local_path[AUDIO_PATH_MAX];
    strlcpy(local_path, s_play_path, sizeof(local_path));

    ESP_LOGI(TAG, "MP3 playback start: %s", local_path);

    FILE *fp = fopen(local_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file: %s", local_path);
        goto cleanup;
    }

    size_t id3_skip = skip_id3v2_tag(fp);
    if (id3_skip > 0) {
        ESP_LOGI(TAG, "Skipped ID3v2 tag (%u bytes)", (unsigned)id3_skip);
    } else {
        (void)fseek(fp, 0, SEEK_SET);
    }
    ESP_LOGI(TAG, "Output mix mode: mono downmix (equal-power L+R)");

    uint8_t *input_buf = (uint8_t *)malloc(MP3_INPUT_BUFFER_SIZE);
    uint8_t *decode_buf = (uint8_t *)malloc(MP3_OUTPUT_BUFFER_SIZE);
    int16_t *i2s_buf = (int16_t *)malloc(MP3_OUTPUT_BUFFER_SIZE * 2u);
    size_t decode_buf_size = MP3_OUTPUT_BUFFER_SIZE;
    esp_audio_simple_dec_handle_t dec = NULL;
    bool audio_dec_registered = false;
    bool simple_dec_registered = false;
    if (!input_buf || !decode_buf || !i2s_buf) {
        ESP_LOGE(TAG, "MP3 buffers allocation failed");
        playback_error = true;
        goto playback_resources_cleanup;
    }

    esp_err_t ret = ensure_i2s_enabled();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S enable failed (%s)", esp_err_to_name(ret));
        playback_error = true;
        goto playback_resources_cleanup;
    }

    esp_audio_err_t codec_ret = esp_audio_dec_register_default();
    if (codec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Audio decoder register failed (%d)", (int)codec_ret);
        playback_error = true;
        goto playback_resources_cleanup;
    }
    audio_dec_registered = true;
    codec_ret = esp_audio_simple_dec_register_default();
    if (codec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Simple decoder register failed (%d)", (int)codec_ret);
        playback_error = true;
        goto playback_resources_cleanup;
    }
    simple_dec_registered = true;

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .use_frame_dec = false,
    };
    codec_ret = esp_audio_simple_dec_open(&dec_cfg, &dec);
    if (codec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "MP3 decoder open failed (%d)", (int)codec_ret);
        playback_error = true;
        goto playback_resources_cleanup;
    }

    tone_runtime_t tone_rt = {0};
    sound_enricher_runtime_t sound_enricher_rt = {0};
    esp_audio_simple_dec_info_t dec_info = {0};
    bool have_dec_info = false;
    size_t pending_len = 0;

    uint64_t remaining_skip_us = (uint64_t)playback_start_offset_sec() * 1000000ULL;

    /*
     * Resume: decoding and discarding everything up to the offset took tens
     * of seconds for long chapters and looked like resume did nothing. Seek
     * by average byte rate instead, a few seconds early; the decoder resyncs
     * on the next frame header. Falls back to decode-skip without a duration.
     */
    if (remaining_skip_us > 0) {
        uint32_t duration_sec = 0;
        playback_get_position(NULL, &duration_sec);
        const uint32_t start_sec = playback_start_offset_sec();
        long file_end = -1;
        if (fseek(fp, 0, SEEK_END) == 0) {
            file_end = ftell(fp);
        }
        if (duration_sec > 0 && file_end > (long)id3_skip && start_sec < duration_sec) {
            const uint32_t target_sec = (start_sec > MP3_RESUME_BACKOFF_SEC) ? (start_sec - MP3_RESUME_BACKOFF_SEC) : 0;
            const uint64_t audio_bytes = (uint64_t)(file_end - (long)id3_skip);
            const long target_pos = (long)id3_skip + (long)((audio_bytes * target_sec) / duration_sec);
            if (fseek(fp, target_pos, SEEK_SET) == 0) {
                playback_set_position((uint64_t)target_sec * 1000000ULL, duration_sec, target_sec);
                remaining_skip_us = 0;
                ESP_LOGI(TAG, "Resume seek: %us -> byte %ld (%us)", (unsigned)start_sec, target_pos, (unsigned)target_sec);
            }
        }
        if (remaining_skip_us > 0) {
            (void)fseek(fp, (long)id3_skip, SEEK_SET);
        }
    }
    uint32_t last_persist_sec = playback_elapsed_sec();

    while (!playback_stop_requested()) {
        if (playback_pause_requested()) {
            uint8_t vu = playback_get_vu_level();
            if (vu > 2) {
                playback_set_vu_level((uint8_t)(vu - 2));
            } else {
                playback_set_vu_level(0);
            }
            vTaskDelay(pdMS_TO_TICKS(MP3_PAUSE_POLL_MS));
            continue;
        }

        if (pending_len >= MP3_INPUT_BUFFER_SIZE) {
            ESP_LOGW(TAG, "MP3 decoder input tail overflow (%u bytes)", (unsigned)pending_len);
            pending_len = 0;
        }

        int64_t read_t0 = esp_timer_get_time();
        size_t read_now = fread(input_buf + pending_len, 1, MP3_INPUT_BUFFER_SIZE - pending_len, fp);
        uint64_t read_us = (uint64_t)(esp_timer_get_time() - read_t0);
        sd_read_count++;
        if (read_us > sd_max_read_us) sd_max_read_us = read_us;
        if (read_us > 8000u) sd_slow_read_count++;
        size_t data_len = pending_len + read_now;
        if (data_len == 0) {
            playback_finished = true;
            break;
        }
        pending_len = 0;

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input_buf,
            .len = (int)data_len,
            .eos = feof(fp),
        };

        while (!playback_stop_requested() && raw.len > 0) {
            esp_audio_simple_dec_out_t out = {
                .buffer = decode_buf,
                .len = (int)decode_buf_size,
            };
            codec_ret = esp_audio_simple_dec_process(dec, &raw, &out);
            if (codec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > 0) {
                uint8_t *new_decode_buf = (uint8_t *)malloc((size_t)out.needed_size);
                int16_t *new_i2s_buf = (int16_t *)malloc((size_t)out.needed_size * 2u);
                if (!new_decode_buf || !new_i2s_buf) {
                    ESP_LOGE(TAG, "MP3 output buffer grow failed (%d bytes)", (int)out.needed_size);
                    free(new_decode_buf);
                    free(new_i2s_buf);
                    playback_error = true;
                    goto playback_resources_cleanup;
                }
                free(decode_buf);
                free(i2s_buf);
                decode_buf = new_decode_buf;
                i2s_buf = new_i2s_buf;
                decode_buf_size = (size_t)out.needed_size;
                continue;
            }
            if (codec_ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "MP3 decode failed (%d)", (int)codec_ret);
                break;
            }

            if (out.decoded_size > 0) {
                esp_audio_simple_dec_info_t cur_info = {0};
                if (esp_audio_simple_dec_get_info(dec, &cur_info) == ESP_AUDIO_ERR_OK &&
                    cur_info.sample_rate > 0 && cur_info.channel > 0 && cur_info.bits_per_sample == 16) {
                    dec_info = cur_info;
                    if (!have_dec_info) {
                        have_dec_info = true;
                        ESP_LOGI(TAG,
                                 "MP3 stream info: %d Hz, %d channel(s), %d bit",
                                 (int)dec_info.sample_rate,
                                 (int)dec_info.channel,
                                 (int)dec_info.bits_per_sample);
                    }
                }
                if (!have_dec_info) {
                    if (raw.consumed <= 0) {
                        break;
                    }
                    if (raw.consumed > raw.len) {
                        raw.consumed = raw.len;
                    }
                    raw.len -= raw.consumed;
                    raw.buffer += raw.consumed;
                    continue;
                }

                uint32_t sample_rate = (uint32_t)dec_info.sample_rate;
                uint8_t channels = (uint8_t)dec_info.channel;
                size_t sample_bytes = ((size_t)dec_info.bits_per_sample + 7u) / 8u;
                size_t frames = (sample_bytes > 0 && channels > 0)
                                    ? ((size_t)out.decoded_size / (sample_bytes * channels))
                                    : 0;
                if (sample_rate == 0 || channels == 0 || sample_bytes != sizeof(int16_t) || frames == 0) {
                    if (raw.consumed <= 0) {
                        break;
                    }
                    if (raw.consumed > raw.len) {
                        raw.consumed = raw.len;
                    }
                    raw.len -= raw.consumed;
                    raw.buffer += raw.consumed;
                    continue;
                }

                uint64_t frame_us = ((uint64_t)frames * 1000000ULL) / sample_rate;
                if (remaining_skip_us > 0) {
                    if (frame_us >= remaining_skip_us) {
                        remaining_skip_us = 0;
                    } else {
                        remaining_skip_us -= frame_us;
                    }
                    if (raw.consumed <= 0) {
                        break;
                    }
                    if (raw.consumed > raw.len) {
                        raw.consumed = raw.len;
                    }
                    raw.len -= raw.consumed;
                    raw.buffer += raw.consumed;
                    continue;
                }

                playback_add_elapsed(frame_us);
                {
                    /* Save the position periodically so power-off or a crash mid-chapter can still resume. */
                    const uint32_t now_sec = playback_elapsed_sec();
                    if (now_sec >= last_persist_sec + MP3_PERSIST_PERIOD_SEC) {
                        last_persist_sec = now_sec;
                        persist_active_story_progress(false);
                    }
                }

                if (sample_rate != s_i2s_sample_rate_hz) {
                    (void)reconfigure_i2s_clock(sample_rate);
                }
                uint8_t volume = s_volume;
                if (volume > 100) {
                    volume = 100;
                }
                tone_runtime_configure(&tone_rt, sample_rate, volume);
                sound_enricher_runtime_configure(&sound_enricher_rt, sample_rate, volume);
                const bool tone_active = tone_rt.active;
                const int32_t gain_q15 = volume_to_gain_q15(volume);

                const int16_t *src = (const int16_t *)out.buffer;
                const bool mono_enabled = s_mono_enabled;
                if (mono_enabled || channels <= 1) {
                    for (size_t i = 0; i < frames; i++) {
                        int32_t mono = 0;
                        if (channels <= 1) {
                            mono = src[i];
                        } else {
                            mono = downmix_stereo_to_mono(src[i * channels], src[i * channels + 1u]);
                        }
                        int16_t smp = apply_volume((int16_t)mono, gain_q15);
                        i2s_buf[i * 2u] = smp;
                        i2s_buf[i * 2u + 1u] = smp;
                    }

                    if (tone_active) {
                        tone_runtime_process_mono_dup(&tone_rt, i2s_buf, frames);
                    }
                } else {
                    for (size_t i = 0; i < frames; i++) {
                        i2s_buf[i * 2u] = apply_volume(src[i * channels], gain_q15);
                        i2s_buf[i * 2u + 1u] = apply_volume(src[i * channels + 1u], gain_q15);
                    }
                }
                sound_enricher_runtime_process(&sound_enricher_rt, i2s_buf, frames);

                const int16_t *meter_src = i2s_buf;
                size_t meter_samples = frames * 2u;
                if (meter_src && meter_samples > 0) {
                    size_t step = (meter_samples > 384u) ? (meter_samples / 384u) : 1u;
                    uint64_t acc = 0;
                    size_t cnt = 0;
                    for (size_t i = 0; i < meter_samples; i += step) {
                        int32_t v = meter_src[i];
                        if (v < 0) {
                            v = -v;
                        }
                        acc += (uint32_t)v;
                        cnt++;
                    }
                    uint32_t mean_abs = (cnt > 0) ? (uint32_t)(acc / cnt) : 0u;
                    uint32_t level = (mean_abs * 120u) / 32767u;
                    if (level > 100u) {
                        level = 100u;
                    }
                    uint32_t prev = playback_get_vu_level();
                    playback_set_vu_level((uint8_t)((prev * 7u + level * 3u) / 10u));
                }

                const uint8_t *write_ptr = (const uint8_t *)i2s_buf;
                size_t write_len = frames * 2u * sizeof(int16_t);
                while (!playback_stop_requested() && write_len > 0) {
                    if (playback_pause_requested()) {
                        vTaskDelay(pdMS_TO_TICKS(MP3_PAUSE_POLL_MS));
                        continue;
                    }

                    size_t written = 0;
                    ret = i2s_channel_write(s_i2s_tx, write_ptr, write_len, &written, pdMS_TO_TICKS(MP3_WRITE_TIMEOUT_MS));
                    if (written > 0) {
                        write_ptr += written;
                        write_len -= written;
                    }

                    if (ret == ESP_OK) {
                        continue;
                    }
                    if (ret == ESP_ERR_TIMEOUT) {
                        i2s_timeout_count++;
                        if (written == 0) {
                            vTaskDelay(pdMS_TO_TICKS(1));
                        }
                        continue;
                    }

                    ESP_LOGW(TAG, "I2S write failed (%s)", esp_err_to_name(ret));
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            if (raw.consumed <= 0) {
                break;
            }
            if (raw.consumed > raw.len) {
                raw.consumed = raw.len;
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }

        if (raw.len > 0) {
            if (read_now == 0 && feof(fp)) {
                ESP_LOGW(TAG, "MP3 decoder left %d trailing byte(s) at EOF", raw.len);
                playback_finished = true;
                break;
            }
            pending_len = (size_t)raw.len;
            memmove(input_buf, raw.buffer, pending_len);
        } else if (read_now == 0 && feof(fp)) {
            playback_finished = true;
            break;
        }
    }

playback_resources_cleanup:
    if (dec) {
        esp_audio_simple_dec_close(dec);
    }
    if (simple_dec_registered) {
        esp_audio_simple_dec_unregister_default();
    }
    if (audio_dec_registered) {
        esp_audio_dec_unregister_default();
    }
    free(input_buf);
    free(decode_buf);
    free(i2s_buf);
    if (fp) {
        fclose(fp);
    }

    if (playback_error) {
        goto cleanup;
    }

    if (!playback_stop_requested()) {
        playback_finished = true;
        ESP_LOGI(TAG, "MP3 playback finished: %s", local_path);
    }
    if (sd_read_count > 0 || i2s_timeout_count > 0) {
        ESP_LOGI(TAG,
                 "MP3 timing: sd_reads=%u slow>%ums=%u max=%lluus i2s_timeouts=%u",
                 (unsigned)sd_read_count,
                 8u,
                 (unsigned)sd_slow_read_count,
                 (unsigned long long)sd_max_read_us,
                 (unsigned)i2s_timeout_count);
    }

cleanup:
    if (playback_finished && !playback_stop_requested()) {
        playback_set_finished_event(true);
    }
    persist_active_story_progress(playback_finished);
    s_active_story_index = -1;
    s_active_chapter_index = -1;
    playback_clear_runtime_state();
    bool delete_with_caps = playback_clear_task_and_take_caps();
    if (delete_with_caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

static esp_err_t init_sd(void)
{
    if (s_sd_ready) {
        return ESP_OK;
    }

    const uint8_t primary_width = CONFIG_AUDIO_BACKEND_SDMMC_BUS_WIDTH_4 ? 4 : 1;
    const uint8_t fallback_width = 1;
    const bool try_fallback_width = (primary_width == 4) && CONFIG_AUDIO_BACKEND_SDMMC_TRY_1BIT_FALLBACK;
#if SOC_SDMMC_IO_POWER_EXTERNAL
    const bool use_ldo_cfg = CONFIG_AUDIO_BACKEND_SDMMC_USE_INTERNAL_LDO;
#else
    const bool use_ldo_cfg = false;
#endif

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = CONFIG_AUDIO_BACKEND_SDMMC_FORMAT_IF_MOUNT_FAILED,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    esp_err_t last_err = ESP_FAIL;
    /* High speed (40 MHz) is tried first; cards or wiring that cannot keep up fall back to 20 MHz. */
    int freq_khz = CONFIG_AUDIO_BACKEND_SDMMC_MAX_FREQ_KHZ;
    for (int pass = 0; pass < 4; pass++) {
        uint8_t width = primary_width;
        bool use_ldo = false;

        if (pass == 0) {
            width = primary_width;
            use_ldo = false;
        } else if (pass == 1) {
            width = primary_width;
            use_ldo = use_ldo_cfg;
            if (!use_ldo) {
                continue;
            }
        } else if (pass == 2) {
            if (!try_fallback_width) {
                continue;
            }
            width = fallback_width;
            use_ldo = false;
        } else {
            if (!try_fallback_width || !use_ldo_cfg) {
                continue;
            }
            width = fallback_width;
            use_ldo = true;
        }

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        /*
         * ESP-Hosted uses SDMMC slot 1 for the on-board ESP32-C6.
         * Keep the removable SD card on slot 0, matching the vendor BSP,
         * so the two SDIO clients do not fight over the same host slot.
         */
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = freq_khz;
        host.flags &= ~SDMMC_HOST_FLAG_DDR;

        sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_cfg.width = width;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
        slot_cfg.clk = SD_CLK;
        slot_cfg.cmd = SD_CMD;
        slot_cfg.d0 = SD_D0;
        slot_cfg.d1 = SD_D1;
        slot_cfg.d2 = SD_D2;
        slot_cfg.d3 = SD_D3;
#endif
        slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

#if SOC_SDMMC_IO_POWER_EXTERNAL
        sd_pwr_ctrl_handle_t local_ldo_handle = NULL;
        if (use_ldo) {
            sd_pwr_ctrl_ldo_config_t ldo_cfg = {
                .ldo_chan_id = CONFIG_AUDIO_BACKEND_SDMMC_LDO_CHANNEL_ID,
            };
            esp_err_t ldo_err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &local_ldo_handle);
            if (ldo_err != ESP_OK) {
                ESP_LOGW(TAG, "LDO init failed (chan=%d): %s",
                         CONFIG_AUDIO_BACKEND_SDMMC_LDO_CHANNEL_ID, esp_err_to_name(ldo_err));
                last_err = ldo_err;
                continue;
            }
            host.pwr_ctrl_handle = local_ldo_handle;
        }
#endif

        ESP_LOGI(TAG, "Mounting SD (slot=%d, width=%u, max_freq=%u kHz, internal_ldo=%s)",
                 host.slot,
                 (unsigned)width,
                 (unsigned)host.max_freq_khz,
                 use_ldo ? "on" : "off");
        esp_err_t ret = esp_vfs_fat_sdmmc_mount(AUDIO_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
        if (ret == ESP_OK) {
            s_sd_ready = true;
#if SOC_SDMMC_IO_POWER_EXTERNAL
            if (use_ldo) {
                s_sd_pwr_ctrl = host.pwr_ctrl_handle;
            }
#endif
            ESP_LOGI(TAG, "SD mounted at %s", AUDIO_MOUNT_POINT);
            sdmmc_card_print_info(stdout, s_card);
            return ESP_OK;
        }

        last_err = ret;
        ESP_LOGW(TAG, "SD mount attempt failed: %s", esp_err_to_name(ret));
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "SD card responded but FAT mount failed.");
            ESP_LOGE(TAG, "Likely filesystem is not FAT/FAT32 (e.g. exFAT) or is corrupted.");
            ESP_LOGE(TAG, "Use FAT32 or enable: Audio Backend -> Format SD card if mount fails.");
#if SOC_SDMMC_IO_POWER_EXTERNAL
            if (host.pwr_ctrl_handle) {
                s_sd_pwr_ctrl = host.pwr_ctrl_handle;
            }
#endif
            return ret;
        }
#if SOC_SDMMC_IO_POWER_EXTERNAL
        if (host.pwr_ctrl_handle) {
            (void)sd_pwr_ctrl_del_on_chip_ldo(host.pwr_ctrl_handle);
            host.pwr_ctrl_handle = NULL;
        }
#endif
        (void)sdmmc_host_deinit();
        if (freq_khz > SDMMC_FREQ_DEFAULT) {
            ESP_LOGW(TAG, "Retrying SD mount at %d kHz", SDMMC_FREQ_DEFAULT);
            freq_khz = SDMMC_FREQ_DEFAULT;
            pass--;
        }
    }

    ESP_LOGE(TAG, "SD mount failed after retries (%s)", esp_err_to_name(last_err));
    ESP_LOGE(TAG, "Check menuconfig: Audio Backend SD settings and SD IO internal LDO (ESP32-P4).");
    return last_err;
}

static esp_err_t init_i2s(void);
static esp_err_t init_codec(void);

static inline int amp_mute_level(bool muted)
{
    return muted ? AMP_MUTE_ACTIVE_LEVEL : !AMP_MUTE_ACTIVE_LEVEL;
}

static esp_err_t init_amp_mute_gpio(void)
{
    gpio_num_t amp_gpio = AMP_MUTE_GPIO;
    if (amp_gpio == GPIO_NUM_NC) {
        return ESP_OK;
    }

    gpio_config_t amp_cfg = {
        .pin_bit_mask = 1ULL << amp_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amp_cfg), TAG, "AMP mute pin config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(amp_gpio, amp_mute_level(true)), TAG, "AMP initial mute failed");
    return ESP_OK;
}

static inline void amp_set_muted(bool muted)
{
    gpio_num_t amp_gpio = AMP_MUTE_GPIO;
    if (amp_gpio != GPIO_NUM_NC) {
        (void)gpio_set_level(amp_gpio, amp_mute_level(muted));
    }
}

static esp_err_t play_ui_chime(bool reverse, const char *name)
{
    static const uint32_t sample_rate_hz = 44100;
    static const uint32_t tone_ms = 900;
    static const uint32_t attack_ms = 35;
    static const uint32_t release_ms = 220;
    static const uint32_t seg1_ms = 320; /* A4 */
    static const uint32_t seg2_ms = 300; /* C5 */
    static const float f1 = 440.0f;
    static const float f2 = 523.25f;
    static const float f3 = 659.25f;     /* E5 */
    static const float amp = 0.16f;
    static const uint32_t frames_per_chunk = 256;

    esp_err_t ret = init_i2s();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = init_codec();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ensure_i2s_enabled();
    if (ret != ESP_OK) {
        return ret;
    }

    amp_set_muted(false);
    if (s_codec) {
        (void)esp_codec_dev_set_out_mute(s_codec, false);
    }

    ret = reconfigure_i2s_clock(sample_rate_hz);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint8_t prev_vol = s_volume;
    const uint8_t tone_vol = s_startup_volume;
    if (s_codec && tone_vol != prev_vol) {
        (void)esp_codec_dev_set_out_vol(s_codec, tone_vol);
    }

    const uint32_t total_frames = (sample_rate_hz * tone_ms) / 1000u;
    const uint32_t attack_frames = (sample_rate_hz * attack_ms) / 1000u;
    const uint32_t release_frames = (sample_rate_hz * release_ms) / 1000u;
    const uint32_t seg1_frames = (sample_rate_hz * seg1_ms) / 1000u;
    const uint32_t seg2_frames = (sample_rate_hz * seg2_ms) / 1000u;
    const size_t total_samples = (size_t)total_frames * 2u;

    int16_t *pcm_all = (int16_t *)malloc(total_samples * sizeof(int16_t));
    if (!pcm_all) {
        if (s_codec && tone_vol != prev_vol) {
            (void)esp_codec_dev_set_out_vol(s_codec, prev_vol);
        }
        return ESP_ERR_NO_MEM;
    }
    int16_t pcm_chunk[frames_per_chunk * 2u];

    for (uint32_t frame = 0; frame < total_frames; frame++) {
        float freq = f3;
        if (frame < seg1_frames) {
            freq = f1;
        } else if (frame < (seg1_frames + seg2_frames)) {
            freq = f2;
        }

        float env = 1.0f;
        if (attack_frames > 0 && frame < attack_frames) {
            env = (float)frame / (float)attack_frames;
        } else if (release_frames > 0 && frame > (total_frames - release_frames)) {
            uint32_t tail = total_frames - frame;
            env = (float)tail / (float)release_frames;
            if (env < 0.0f) {
                env = 0.0f;
            }
        }

        float t = (float)frame / (float)sample_rate_hz;
        float s = sinf(2.0f * (float)M_PI * freq * t);
        int16_t v = (int16_t)(s * env * amp * 32767.0f);
        pcm_all[(frame * 2u) + 0u] = v;
        pcm_all[(frame * 2u) + 1u] = v;
    }

    uint32_t out_frame = 0;
    while (out_frame < total_frames) {
        uint32_t batch = total_frames - out_frame;
        if (batch > frames_per_chunk) {
            batch = frames_per_chunk;
        }

        for (uint32_t i = 0; i < batch; i++) {
            uint32_t src_frame = reverse ? (total_frames - 1u - (out_frame + i)) : (out_frame + i);
            pcm_chunk[(i * 2u) + 0u] = pcm_all[(src_frame * 2u) + 0u];
            pcm_chunk[(i * 2u) + 1u] = pcm_all[(src_frame * 2u) + 1u];
        }

        const uint8_t *write_ptr = (const uint8_t *)pcm_chunk;
        size_t write_len = (size_t)batch * 2u * sizeof(int16_t);
        while (write_len > 0) {
            size_t written = 0;
            ret = i2s_channel_write(s_i2s_tx, write_ptr, write_len, &written, pdMS_TO_TICKS(80));
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                free(pcm_all);
                if (s_codec && tone_vol != prev_vol) {
                    (void)esp_codec_dev_set_out_vol(s_codec, prev_vol);
                }
                return ret;
            }
            if (written == 0) {
                continue;
            }
            write_ptr += written;
            write_len -= written;
        }

        out_frame += batch;
    }

    free(pcm_all);
    if (s_codec && tone_vol != prev_vol) {
        (void)esp_codec_dev_set_out_vol(s_codec, prev_vol);
    }
    ESP_LOGI(TAG, "%s tone played (%ums)", name ? name : "UI", (unsigned)tone_ms);
    return ESP_OK;
}

static esp_err_t init_i2s(void)
{
    if (s_i2s_ready) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /*
     * 16 x 256 frames = ~93 ms at 44.1 kHz, 16 KB of internal DMA RAM. 512 frames
     * doubled that; with decoding on its own core the shorter buffer has headroom.
     */
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.intr_priority = 2;
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_DEFAULT_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    esp_err_t ret = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed (%s)", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed (%s)", esp_err_to_name(ret));
        return ret;
    }

#if ES8311_PA_GPIO != GPIO_NUM_NC
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << ES8311_PA_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pa_cfg), TAG, "PA pin config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(ES8311_PA_GPIO, 0), TAG, "PA pin initial mute failed");
#endif
    ESP_RETURN_ON_ERROR(init_amp_mute_gpio(), TAG, "AMP mute pin init failed");

    s_i2s_sample_rate_hz = AUDIO_DEFAULT_RATE_HZ;
    s_tx_enabled = true;
    s_i2s_ready = true;
    ESP_LOGI(TAG, "I2S TX ready (MCK=%d, BCK=%d, WS=%d, DO=%d)", I2S_MCK_IO, I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO);
    return ESP_OK;
}

static esp_err_t init_codec(void)
{
    ESP_LOGI(TAG, "Using direct I2S amplifier output; external ES8311 codec init skipped");
    amp_set_muted(false);
    return ESP_OK;
}

esp_err_t audio_backend_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = init_sd();
    if (ret != ESP_OK) {
        return ret;
    }
    scan_sd_catalog();

    ret = init_i2s();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = init_codec();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Codec init failed (%s)", esp_err_to_name(ret));
        return ret;
    }

    s_decoder_ready = true;
    s_initialized = true;
    ESP_LOGI(TAG, "MP3 decoder ready (esp_audio_codec)");
    return ESP_OK;
}

esp_err_t audio_backend_play_startup_tone(void)
{
    return play_ui_chime(false, "Startup");
}

esp_err_t audio_backend_play_shutdown_tone(void)
{
    return play_ui_chime(true, "Shutdown");
}

bool audio_backend_is_ready(void)
{
    return s_sd_ready && s_i2s_ready;
}

bool audio_backend_is_decoder_ready(void)
{
    return s_decoder_ready;
}

bool audio_backend_is_playback_active(void)
{
    return playback_get_task() != NULL;
}

bool audio_backend_consume_finished_event(void)
{
    bool finished;
    taskENTER_CRITICAL(&s_playback_mux);
    finished = s_playback_finished_event;
    s_playback_finished_event = false;
    taskEXIT_CRITICAL(&s_playback_mux);
    return finished;
}

esp_err_t audio_backend_get_playback_progress(uint32_t *elapsed_sec, uint32_t *duration_sec)
{
    if (!elapsed_sec && !duration_sec) {
        return ESP_ERR_INVALID_ARG;
    }

    playback_get_position(elapsed_sec, duration_sec);
    return ESP_OK;
}

esp_err_t audio_backend_get_vu_level(uint8_t *level_pct)
{
    if (!level_pct) {
        return ESP_ERR_INVALID_ARG;
    }
    *level_pct = playback_get_vu_level();
    return ESP_OK;
}

static esp_err_t audio_backend_play_chapter_internal(uint8_t story_id, uint8_t chapter_index, uint32_t start_sec)
{
    if (!audio_backend_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    playback_set_finished_event(false);

    char target[AUDIO_PATH_MAX];
    esp_err_t ret = resolve_chapter_path(story_id, chapter_index, target, sizeof(target));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No MP3 found for story=%u chapter=%u", (unsigned)story_id, (unsigned)chapter_index);
        return ret;
    }

    ret = stop_play_task_and_wait(pdMS_TO_TICKS(4000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stopping previous playback timed out");
        return ret;
    }

    ret = ensure_i2s_enabled();
    if (ret != ESP_OK) {
        return ret;
    }

    playback_set_stop_requested(false);
    playback_set_pause_requested(false);
    playback_set_position((uint64_t)start_sec * 1000000ULL, 0, start_sec);
    playback_set_vu_level(0);

    uint32_t chapter_duration_sec = 0;
    if (story_id < s_story_count && chapter_index < s_stories[story_id].chapter_count) {
        s_active_story_index = story_id;
        s_active_chapter_index = chapter_index;
        chapter_duration_sec = s_stories[story_id].chapters[chapter_index].duration_sec;
    } else {
        s_active_story_index = -1;
        s_active_chapter_index = -1;
    }

    if (chapter_duration_sec == 0) {
        chapter_duration_sec = estimate_mp3_duration_sec(target);
        if (chapter_duration_sec > 0 &&
            s_active_story_index >= 0 &&
            (uint8_t)s_active_story_index < s_story_count &&
            chapter_index < s_stories[s_active_story_index].chapter_count) {
            s_stories[s_active_story_index].chapters[chapter_index].duration_sec = chapter_duration_sec;
        }
    }

    playback_set_duration(chapter_duration_sec);
    strlcpy(s_play_path, target, sizeof(s_play_path));

    TaskHandle_t task = NULL;
    bool task_with_caps = false;
    vTaskSuspendAll();
    BaseType_t created = pdFAIL;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM
    created = xTaskCreatePinnedToCoreWithCaps(mp3_playback_task,
                                              "mp3_play",
                                              MP3_PLAY_TASK_STACK_BYTES,
                                              NULL,
                                              MP3_PLAY_TASK_PRIORITY,
                                              &task,
                                              MP3_PLAY_TASK_CORE,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created == pdPASS && task != NULL) {
        task_with_caps = true;
    } else
#endif
    {
        task = NULL;
        created = xTaskCreatePinnedToCore(mp3_playback_task,
                                          "mp3_play",
                                          MP3_PLAY_TASK_STACK_BYTES,
                                          NULL,
                                          MP3_PLAY_TASK_PRIORITY,
                                          &task,
                                          MP3_PLAY_TASK_CORE);
    }
    if (created == pdPASS && task != NULL) {
        playback_set_task(task, task_with_caps);
    }
    (void)xTaskResumeAll();

    if (created != pdPASS || task == NULL) {
        s_active_story_index = -1;
        s_active_chapter_index = -1;
        playback_clear_runtime_state();
        ESP_LOGE(TAG,
                 "Failed to create MP3 playback task (stack=%u, free_internal=%u, largest_internal=%u, free_psram=%u, largest_psram=%u)",
                 (unsigned)MP3_PLAY_TASK_STACK_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    if (s_active_story_index >= 0 && (uint8_t)s_active_story_index < s_story_count) {
        audio_backend_story_meta_t *story = &s_stories[s_active_story_index];
        story->play_count++;
        story->resume_time_sec = 0;
        story->resume_chapter_index = 0;
    }

    ESP_LOGI(TAG, "Requested play story=%u chapter=%u start=%us file=%s",
             (unsigned)story_id, (unsigned)chapter_index, (unsigned)start_sec, target);
    return ESP_OK;
}

esp_err_t audio_backend_play_chapter(uint8_t story_id, uint8_t chapter_index)
{
    return audio_backend_play_chapter_internal(story_id, chapter_index, 0);
}

esp_err_t audio_backend_play_chapter_from(uint8_t story_id, uint8_t chapter_index, uint32_t start_sec)
{
    return audio_backend_play_chapter_internal(story_id, chapter_index, start_sec);
}

esp_err_t audio_backend_pause(void)
{
    if (!s_i2s_ready || !s_i2s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (playback_get_task() == NULL) {
        return ESP_OK;
    }

    playback_set_pause_requested(true);
    if (s_codec) {
        (void)esp_codec_dev_set_out_mute(s_codec, true);
    }
    persist_active_story_progress(false);
    return ESP_OK;
}

esp_err_t audio_backend_resume(void)
{
    if (!s_i2s_ready || !s_i2s_tx) {
        return ESP_ERR_INVALID_STATE;
    }

    playback_set_pause_requested(false);
    if (s_codec) {
        (void)esp_codec_dev_set_out_mute(s_codec, false);
    }
    return ensure_i2s_enabled();
}

esp_err_t audio_backend_stop(void)
{
    if (!s_i2s_ready || !s_i2s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    playback_set_finished_event(false);
    playback_set_vu_level(0);

    esp_err_t wait_ret = stop_play_task_and_wait(pdMS_TO_TICKS(4000));
    esp_err_t flush_ret = flush_i2s_tx();

    if (wait_ret != ESP_OK) {
        ESP_LOGW(TAG, "Stop wait failed (%s)", esp_err_to_name(wait_ret));
        return wait_ret;
    }

    playback_set_duration(0);
    return flush_ret;
}

esp_err_t audio_backend_prepare_shutdown(void)
{
    esp_err_t first_err = ESP_OK;
    esp_err_t ret = ESP_OK;

    ret = audio_backend_stop();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE && first_err == ESP_OK) {
        first_err = ret;
    }

    if (s_shutdown_tone_enabled) {
        ret = audio_backend_play_shutdown_tone();
        if (ret != ESP_OK && first_err == ESP_OK) {
            first_err = ret;
        }
    }

    if (s_codec) {
        if (esp_codec_dev_set_out_mute(s_codec, true) != ESP_CODEC_DEV_OK && first_err == ESP_OK) {
            first_err = ESP_FAIL;
        }
    }
    amp_set_muted(true);
    if (ES8311_PA_GPIO != GPIO_NUM_NC) {
        (void)gpio_set_level(ES8311_PA_GPIO, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    if (s_i2s_ready && s_i2s_tx && s_tx_enabled) {
        ret = i2s_channel_disable(s_i2s_tx);
        if (ret != ESP_OK && first_err == ESP_OK) {
            first_err = ret;
        } else if (ret == ESP_OK) {
            s_tx_enabled = false;
        }
    }

    if (s_sd_ready && s_card) {
        ret = esp_vfs_fat_sdcard_unmount(AUDIO_MOUNT_POINT, s_card);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD unmount failed (%s)", esp_err_to_name(ret));
            if (first_err == ESP_OK) {
                first_err = ret;
            }
        } else {
            s_sd_ready = false;
            s_card = NULL;
            ESP_LOGI(TAG, "SD unmounted");
        }
    }

    return first_err;
}

esp_err_t audio_backend_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
    if (s_codec) {
        if (esp_codec_dev_set_out_vol(s_codec, s_volume) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Codec volume update failed");
        }
    }
    ESP_LOGD(TAG, "Volume set to %u", (unsigned)s_volume);
    return ESP_OK;
}

esp_err_t audio_backend_set_startup_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    s_startup_volume = volume;
    return ESP_OK;
}

uint8_t audio_backend_get_startup_volume(void)
{
    return s_startup_volume;
}

esp_err_t audio_backend_set_tone_control(bool enabled, uint8_t bass, uint8_t treble)
{
    if (bass > 100u) {
        bass = 100u;
    }
    if (treble > 100u) {
        treble = 100u;
    }

    bool changed = (s_tone_enabled != enabled) ||
                   (s_tone_bass != bass) ||
                   (s_tone_treble != treble);

    s_tone_enabled = enabled;
    s_tone_bass = bass;
    s_tone_treble = treble;

    if (changed) {
        uint32_t next = s_tone_revision + 1u;
        s_tone_revision = (next == 0u) ? 1u : next;
        ESP_LOGI(TAG, "Mini DSP %s (bass=%u treble=%u)",
                 enabled ? "on" : "off",
                 (unsigned)s_tone_bass,
                 (unsigned)s_tone_treble);
    }

    return ESP_OK;
}

esp_err_t audio_backend_get_tone_control(bool *enabled, uint8_t *bass, uint8_t *treble)
{
    if (enabled) {
        *enabled = s_tone_enabled;
    }
    if (bass) {
        *bass = s_tone_bass;
    }
    if (treble) {
        *treble = s_tone_treble;
    }
    return ESP_OK;
}

esp_err_t audio_backend_set_loudness(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}

esp_err_t audio_backend_get_loudness(bool *enabled)
{
    if (enabled) {
        *enabled = false;
    }
    return ESP_OK;
}

esp_err_t audio_backend_set_sound_enricher(bool enabled, uint8_t amount)
{
    if (amount > 100u) {
        amount = 100u;
    }

    bool changed = (s_sound_enricher_enabled != enabled) ||
                   (s_sound_enricher_amount != amount);
    s_sound_enricher_enabled = enabled;
    s_sound_enricher_amount = amount;

    if (changed) {
        uint32_t next = s_tone_revision + 1u;
        s_tone_revision = (next == 0u) ? 1u : next;
        ESP_LOGI(TAG, "Sound enricher %s (amount=%u)",
                 enabled ? "on" : "off",
                 (unsigned)s_sound_enricher_amount);
    }

    return ESP_OK;
}

esp_err_t audio_backend_get_sound_enricher(bool *enabled, uint8_t *amount)
{
    if (enabled) {
        *enabled = s_sound_enricher_enabled;
    }
    if (amount) {
        *amount = s_sound_enricher_amount;
    }
    return ESP_OK;
}

esp_err_t audio_backend_set_mono_enabled(bool enabled)
{
    if (s_mono_enabled != enabled) {
        s_mono_enabled = enabled;
        ESP_LOGI(TAG, "Mono downmix %s", enabled ? "on" : "off");
    }
    return ESP_OK;
}

esp_err_t audio_backend_get_mono_enabled(bool *enabled)
{
    if (enabled) {
        *enabled = s_mono_enabled;
    }
    return ESP_OK;
}

esp_err_t audio_backend_set_shutdown_tone_enabled(bool enabled)
{
    s_shutdown_tone_enabled = enabled;
    return ESP_OK;
}

bool audio_backend_get_shutdown_tone_enabled(void)
{
    return s_shutdown_tone_enabled;
}

esp_err_t audio_backend_rescan_catalog(void)
{
    if (!s_sd_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    scan_sd_catalog();
    return ESP_OK;
}

uint8_t audio_backend_story_count(void)
{
    return s_story_count;
}

const audio_backend_story_meta_t *audio_backend_story_get(uint8_t index)
{
    if (index >= s_story_count) {
        return NULL;
    }
    return &s_stories[index];
}





