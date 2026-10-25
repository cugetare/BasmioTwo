#include "audio_backend.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_private/wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_netif.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#ifndef CONFIG_ESP_WIFI_REMOTE_ENABLED
#define CONFIG_ESP_WIFI_REMOTE_ENABLED 0
#endif

#define AUDIO_BACKEND_WIFI_SUPPORTED (SOC_WIFI_SUPPORTED || CONFIG_ESP_WIFI_REMOTE_ENABLED)

#if AUDIO_BACKEND_WIFI_SUPPORTED
#include "esp_wifi.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define AUDIO_MOUNT_POINT "/sdcard"
#define WIFI_UPLOAD_PATH_MAX 160
#define WIFI_UPLOAD_QUERY_MAX 256
#define WIFI_UPLOAD_DELETE_DEPTH_MAX 4

#ifndef CONFIG_AUDIO_BACKEND_WIFI_STA_SSID
#define CONFIG_AUDIO_BACKEND_WIFI_STA_SSID ""
#endif

#ifndef CONFIG_AUDIO_BACKEND_WIFI_STA_PASSWORD
#define CONFIG_AUDIO_BACKEND_WIFI_STA_PASSWORD ""
#endif

#ifndef CONFIG_AUDIO_BACKEND_WIFI_CONNECT_TIMEOUT_MS
#define CONFIG_AUDIO_BACKEND_WIFI_CONNECT_TIMEOUT_MS 12000
#endif

#ifndef CONFIG_AUDIO_BACKEND_WIFI_HTTP_PORT
#define CONFIG_AUDIO_BACKEND_WIFI_HTTP_PORT 80
#endif

static const char *TAG = "audio_wifi";

#if AUDIO_BACKEND_WIFI_SUPPORTED
static bool s_wifi_stack_initialized = false;
static bool s_wifi_started = false;
static bool s_wifi_connected = false;
static esp_netif_t *s_wifi_netif = NULL;
static httpd_handle_t s_wifi_httpd = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_event_handler_instance_t s_wifi_evt_any_id;
static esp_event_handler_instance_t s_ip_evt_got_ip;
static bool s_wifi_netif_started = false;
static bool s_wifi_rxcb_registered = false;
static bool s_wifi_netstack_cb_registered = false;
static char s_wifi_ip[16] = {0};
static char s_wifi_status[96] = "Wi-Fi: off";
static int s_wifi_retry_count = 0;
static bool s_wifi_credentials_loaded = false;
static bool s_wifi_has_saved_credentials = false;
static char s_wifi_ssid[AUDIO_BACKEND_WIFI_SSID_MAX] = "";
static char s_wifi_password[AUDIO_BACKEND_WIFI_PASSWORD_MAX] = "";

#define WIFI_CONNECTED_BIT          BIT0
#define WIFI_FAIL_BIT               BIT1
#define WIFI_STA_STARTED_BIT        BIT2
#define WIFI_MAX_RETRY              8
#define WIFI_UPLOAD_RECV_BUF_SIZE   (16 * 1024)
#define WIFI_HTTP_OUT_BUF_SIZE      4096
#define WIFI_UPLOAD_RECV_TIMEOUT_MAX 8
#define WIFI_UPLOAD_PROGRESS_STEP_BYTES (256 * 1024)
#define WIFI_UPLOAD_BROWSER_CHUNK_BYTES (64 * 1024)
#define WIFI_SCAN_START_SETTLE_MS   900
#define WIFI_SCAN_RETRY_DELAY_MS    350
#define WIFI_SCAN_RETRY_COUNT       6
#endif
/* ------------------------------------------------------------------ */
/*  Wi-Fi upload (STA only)                                            */
/* ------------------------------------------------------------------ */

#if AUDIO_BACKEND_WIFI_SUPPORTED

static void wifi_status_set(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_wifi_status, sizeof(s_wifi_status), fmt, args);
    va_end(args);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (c - '0');
    if (c >= 'a' && c <= 'f') return (c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (c - 'A') + 10;
    return -1;
}

static void url_decode_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
            continue;
        }
        if (*r == '%' && r[1] && r[2]) {
            int hi = hex_nibble(r[1]);
            int lo = hex_nibble(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static bool parse_u64_dec(const char *text, uint64_t *out_value)
{
    if (!text || !out_value || text[0] == '\0') {
        return false;
    }

    uint64_t value = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!isdigit(c)) {
            return false;
        }
        value = (value * 10u) + (uint64_t)(c - '0');
    }

    *out_value = value;
    return true;
}

/*
 * Buffered chunked response: small JSON fragments are collected and sent in
 * WIFI_HTTP_OUT_BUF_SIZE pieces instead of one TCP write per fragment.
 */
typedef struct {
    httpd_req_t *req;
    char *buf;
    size_t len;
} http_out_t;

static void http_out_init(http_out_t *out, httpd_req_t *req)
{
    out->req = req;
    out->len = 0;
    out->buf = (char *)malloc(WIFI_HTTP_OUT_BUF_SIZE);
}

static esp_err_t http_out_flush(http_out_t *out)
{
    if (out->len == 0) {
        return ESP_OK;
    }
    esp_err_t err = httpd_resp_send_chunk(out->req, out->buf, out->len);
    out->len = 0;
    return err;
}

static esp_err_t http_out_write(http_out_t *out, const char *data, size_t len)
{
    if (!out->buf) {
        return httpd_resp_send_chunk(out->req, data, len);
    }
    while (len > 0) {
        if (out->len == WIFI_HTTP_OUT_BUF_SIZE) {
            esp_err_t err = http_out_flush(out);
            if (err != ESP_OK) return err;
        }
        size_t n = WIFI_HTTP_OUT_BUF_SIZE - out->len;
        if (n > len) n = len;
        memcpy(out->buf + out->len, data, n);
        out->len += n;
        data += n;
        len -= n;
    }
    return ESP_OK;
}

/* Flushes pending data, terminates the chunked response and frees the buffer. */
static esp_err_t http_out_finish(http_out_t *out, esp_err_t err)
{
    if (err == ESP_OK && out->buf) err = http_out_flush(out);
    if (err == ESP_OK) err = httpd_resp_send_chunk(out->req, NULL, 0);
    free(out->buf);
    out->buf = NULL;
    return err;
}

static esp_err_t http_send_chunk(http_out_t *out, const char *text)
{
    if (!text) {
        return ESP_OK;
    }
    return http_out_write(out, text, strlen(text));
}

static esp_err_t http_send_json_string(http_out_t *out, const char *text)
{
    esp_err_t err = http_out_write(out, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); err == ESP_OK && *p; ++p) {
        const unsigned char c = *p;
        char esc[7];

        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            err = http_out_write(out, esc, 2);
        } else if (c == '\n') {
            err = http_out_write(out, "\\n", 2);
        } else if (c == '\r') {
            err = http_out_write(out, "\\r", 2);
        } else if (c == '\t') {
            err = http_out_write(out, "\\t", 2);
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
            err = http_out_write(out, esc, 6);
        } else {
            err = http_out_write(out, (const char *)p, 1);
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    return http_out_write(out, "\"", 1);
}

static void sanitize_path_component(const char *in, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }

    size_t w = 0;
    if (in) {
        for (size_t i = 0; in[i] != '\0' && w + 1 < out_size; i++) {
            unsigned char c = (unsigned char)in[i];
            bool ok = isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')';
            out[w++] = ok ? (char)c : '_';
        }
    }
    out[w] = '\0';

    while (w > 0 && out[w - 1] == ' ') {
        out[--w] = '\0';
    }
    size_t lead = 0;
    while (out[lead] == ' ') {
        lead++;
    }
    if (lead > 0) {
        memmove(out, out + lead, strlen(out + lead) + 1);
        w = strlen(out);
    }

    if (strstr(out, "..")) {
        for (size_t i = 0; out[i] != '\0'; i++) {
            if (out[i] == '.') {
                out[i] = '_';
            }
        }
    }

    if (out[0] == '\0') {
        strlcpy(out, fallback ? fallback : "item", out_size);
    }
}

static bool path_join2(char *out, size_t out_size, const char *base, const char *name)
{
    if (!out || out_size == 0 || !base || !name) {
        return false;
    }

    out[0] = '\0';
    if (strlcpy(out, base, out_size) >= out_size) {
        return false;
    }
    if (out[0] != '\0' && out[strlen(out) - 1] != '/') {
        if (strlcat(out, "/", out_size) >= out_size) {
            return false;
        }
    }
    return strlcat(out, name, out_size) < out_size;
}

static bool build_sd_path_from_relative(const char *relative, char *out_path, size_t out_path_size)
{
    if (!relative || !out_path || out_path_size == 0 || relative[0] == '\0') {
        return false;
    }
    if (relative[0] == '/' || relative[0] == '\\' || strstr(relative, "..")) {
        return false;
    }

    char clean[WIFI_UPLOAD_PATH_MAX];
    size_t w = 0;
    for (size_t i = 0; relative[i] != '\0' && w + 1 < sizeof(clean); ++i) {
        unsigned char c = (unsigned char)relative[i];
        if (c < 0x20) {
            return false;
        }
        if (c == '\\') {
            c = '/';
        }
        clean[w++] = (char)c;
    }
    clean[w] = '\0';

    if (relative[w] != '\0' || clean[0] == '\0') {
        return false;
    }

    return path_join2(out_path, out_path_size, AUDIO_MOUNT_POINT, clean);
}

static esp_err_t ensure_story_directory(const char *story_dir_name, char *out_dir, size_t out_dir_size)
{
    if (!story_dir_name || !out_dir || out_dir_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!path_join2(out_dir, out_dir_size, AUDIO_MOUNT_POINT, story_dir_name)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (mkdir(out_dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir failed for %s (errno=%d)", out_dir, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t delete_path_recursive(const char *path, uint8_t depth)
{
    struct stat st;
    if (!path || stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (S_ISDIR(st.st_mode)) {
        if (depth >= WIFI_UPLOAD_DELETE_DEPTH_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }

        DIR *dir = opendir(path);
        if (!dir) {
            return ESP_FAIL;
        }

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }

            char child[WIFI_UPLOAD_PATH_MAX];
            if (!path_join2(child, sizeof(child), path, ent->d_name)) {
                closedir(dir);
                return ESP_ERR_INVALID_SIZE;
            }

            esp_err_t err = delete_path_recursive(child, (uint8_t)(depth + 1));
            if (err != ESP_OK) {
                closedir(dir);
                return err;
            }
        }
        closedir(dir);

        if (rmdir(path) != 0) {
            ESP_LOGW(TAG, "rmdir failed for %s (errno=%d)", path, errno);
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (remove(path) != 0) {
        ESP_LOGW(TAG, "remove failed for %s (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool wifi_sd_is_ready(void)
{
    struct stat st;
    return (stat(AUDIO_MOUNT_POINT, &st) == 0 && S_ISDIR(st.st_mode));
}

static void wifi_credentials_load_once(void)
{
    if (s_wifi_credentials_loaded) {
        return;
    }

    s_wifi_credentials_loaded = true;
    s_wifi_has_saved_credentials = false;
    s_wifi_ssid[0] = '\0';
    s_wifi_password[0] = '\0';

    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        nvs_handle_t nvs;
        err = nvs_open("spod_wifi", NVS_READONLY, &nvs);
        if (err == ESP_OK) {
            size_t ssid_len = sizeof(s_wifi_ssid);
            size_t pass_len = sizeof(s_wifi_password);
            esp_err_t ssid_err = nvs_get_str(nvs, "ssid", s_wifi_ssid, &ssid_len);
            esp_err_t pass_err = nvs_get_str(nvs, "pass", s_wifi_password, &pass_len);
            nvs_close(nvs);
            if (ssid_err == ESP_OK && s_wifi_ssid[0] != '\0') {
                if (pass_err != ESP_OK) {
                    s_wifi_password[0] = '\0';
                }
                s_wifi_has_saved_credentials = true;
                ESP_LOGI(TAG, "Wi-Fi credentials loaded from NVS for SSID \"%s\"", s_wifi_ssid);
                return;
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Wi-Fi credential load skipped (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "NVS init for Wi-Fi credentials failed (%s)", esp_err_to_name(err));
    }

    strlcpy(s_wifi_ssid, CONFIG_AUDIO_BACKEND_WIFI_STA_SSID, sizeof(s_wifi_ssid));
    strlcpy(s_wifi_password, CONFIG_AUDIO_BACKEND_WIFI_STA_PASSWORD, sizeof(s_wifi_password));
}

static esp_err_t wifi_credentials_save(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open("spod_wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pass", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t wifi_credentials_erase(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open("spod_wifi", NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    (void)nvs_erase_key(nvs, "ssid");
    (void)nvs_erase_key(nvs, "pass");
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void wifi_netif_register_rxcb_if_needed(wifi_netif_driver_t driver)
{
    if (!driver || s_wifi_rxcb_registered) {
        return;
    }

    esp_err_t ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, s_wifi_netif);
    if (ret == ESP_OK) {
        s_wifi_rxcb_registered = true;
    } else {
        ESP_LOGW(TAG, "esp_wifi_register_if_rxcb failed (%s)", esp_err_to_name(ret));
    }
}

static void wifi_netif_start_once(esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (!s_wifi_netif) {
        return;
    }

    if (s_wifi_netif_started) {
        ESP_LOGW(TAG, "Ignoring duplicate WIFI_EVENT_STA_START");
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_STA_STARTED_BIT);
        }
        return;
    }

    wifi_netif_driver_t driver = (wifi_netif_driver_t)esp_netif_get_io_driver(s_wifi_netif);
    uint8_t mac[6] = {0};
    esp_err_t ret = esp_wifi_get_if_mac(driver, mac);
    if (ret == ESP_OK) {
        (void)esp_netif_set_mac(s_wifi_netif, mac);
    } else {
        ESP_LOGW(TAG, "esp_wifi_get_if_mac failed (%s)", esp_err_to_name(ret));
    }

    if (driver && esp_wifi_is_if_ready_when_started(driver)) {
        wifi_netif_register_rxcb_if_needed(driver);
    }

    if (!s_wifi_netstack_cb_registered) {
        ret = esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref, esp_netif_netstack_buf_free);
        if (ret == ESP_OK) {
            s_wifi_netstack_cb_registered = true;
        } else {
            ESP_LOGW(TAG, "netstack cb register failed (%s)", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "Starting guarded Wi-Fi netif");
    esp_netif_action_start(s_wifi_netif, base, event_id, event_data);
    s_wifi_netif_started = true;
    if (s_wifi_event_group) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_STA_STARTED_BIT);
    }
}

static void wifi_netif_connected(esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (!s_wifi_netif) {
        return;
    }

    wifi_netif_driver_t driver = (wifi_netif_driver_t)esp_netif_get_io_driver(s_wifi_netif);
    if (driver && !esp_wifi_is_if_ready_when_started(driver)) {
        wifi_netif_register_rxcb_if_needed(driver);
    }

    esp_netif_action_connected(s_wifi_netif, base, event_id, event_data);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            s_wifi_retry_count = 0;
            wifi_status_set("Wi-Fi: connecting...");
            wifi_netif_start_once(event_base, event_id, event_data);
        } else if (event_id == WIFI_EVENT_STA_STOP) {
            if (s_wifi_netif_started && s_wifi_netif) {
                esp_netif_action_stop(s_wifi_netif, event_base, event_id, event_data);
            }
            s_wifi_netif_started = false;
            s_wifi_rxcb_registered = false;
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            wifi_netif_connected(event_base, event_id, event_data);
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_wifi_netif_started && s_wifi_netif) {
                esp_netif_action_disconnected(s_wifi_netif, event_base, event_id, event_data);
            }
            s_wifi_connected = false;
            s_wifi_ip[0] = '\0';
            if (s_wifi_started && s_wifi_retry_count < WIFI_MAX_RETRY) {
                s_wifi_retry_count++;
                (void)esp_wifi_connect();
                wifi_status_set("Wi-Fi: reconnect %d/%d", s_wifi_retry_count, WIFI_MAX_RETRY);
            } else {
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                wifi_status_set("Wi-Fi: disconnected");
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        int sta_ip_ret = esp_wifi_internal_set_sta_ip();
        if (sta_ip_ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_internal_set_sta_ip failed (%s)", esp_err_to_name(sta_ip_ret));
        }
        if (s_wifi_netif) {
            esp_netif_action_got_ip(s_wifi_netif, event_base, event_id, event_data);
        }
        snprintf(s_wifi_ip, sizeof(s_wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        s_wifi_retry_count = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        wifi_status_set("Wi-Fi: %s", s_wifi_ip);
    }
}

static esp_err_t wifi_upload_index_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Basmio Library</title><style>"
        ":root{color-scheme:light;--ink:#221a14;--muted:#7c6b5d;--paper:#fff9ef;--card:#fff;--line:#ead9c5;--accent:#d97732;--danger:#b42318}"
        "*{box-sizing:border-box}body{margin:0;background:linear-gradient(135deg,#fff6e8,#f4fbff);font-family:Georgia,'Times New Roman',serif;color:var(--ink)}"
        "main{max-width:900px;margin:0 auto;padding:22px 14px 40px}.hero{display:flex;gap:12px;align-items:flex-end;justify-content:space-between;margin-bottom:18px}"
        "h1{font-size:34px;margin:0}.sub{color:var(--muted);margin:6px 0 0}.grid{display:grid;grid-template-columns:1fr;gap:14px}@media(min-width:760px){.grid{grid-template-columns:360px 1fr}}"
        ".card{background:rgba(255,255,255,.86);border:1px solid var(--line);border-radius:22px;padding:16px;box-shadow:0 14px 40px rgba(92,60,24,.08)}"
        "label{font-weight:700;font-size:14px}input{width:100%;padding:12px;border:1px solid var(--line);border-radius:14px;background:#fff;font:inherit}"
        "button{border:0;border-radius:999px;background:var(--accent);color:#fff;font-weight:700;padding:11px 16px;margin:5px 4px 0 0;cursor:pointer}"
        "button.ghost{background:#f1e4d6;color:var(--ink)}button.danger{background:var(--danger)}button:disabled{opacity:.55}"
        ".story{border:1px solid var(--line);border-radius:16px;padding:12px;margin:10px 0;background:#fffdf9}.storyHead{display:flex;justify-content:space-between;gap:8px;align-items:center}"
        ".storyTitle{font-weight:800;font-size:18px}.file{display:flex;justify-content:space-between;gap:8px;padding:7px 0;border-top:1px dashed #ecdcc8;color:var(--muted);font-size:14px}"
        ".path{font-family:ui-monospace,Consolas,monospace;font-size:12px;color:#9b7b5d}.log{white-space:pre-wrap;font:13px ui-monospace,Consolas,monospace;background:#1f1711;color:#ffe8c7;border-radius:16px;padding:12px;min-height:120px;max-height:240px;overflow:auto}"
        "</style></head><body><main>"
        "<div class='hero'><div><h1>Basmio Library</h1><p class='sub'>Manage stories on the SD card</p></div><button class='ghost' onclick='loadList()'>Refresh</button></div>"
        "<div class='grid'><section class='card'><h2>Add content</h2>"
        "<p class='sub'>Use one folder per story. Select MP3 chapters and cover files together.</p>"
        "<label>Story folder</label><input id='story' placeholder='Alba ca Zapada'><br><br>"
        "<label>Files</label><input id='files' type='file' multiple><br>"
        "<button onclick='uploadAll()'>Upload to SD</button><button class='ghost' onclick='document.getElementById(\"files\").value=\"\"'>Clear</button>"
        "<h3>Activity</h3><div id='log' class='log'></div></section>"
        "<section class='card'><h2>SD contents</h2><div id='status' class='sub'>Loading...</div><div id='list'></div></section></div>"
        "<script>"
        "const $=id=>document.getElementById(id);"
        "const esc=s=>String(s==null?'':s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));"
        "const log=m=>{const e=$('log');e.textContent+=m+'\\n';e.scrollTop=e.scrollHeight;};"
        "function size(n){if(n<1024)return n+' B';if(n<1048576)return Math.round(n/1024)+' KB';return (n/1048576).toFixed(1)+' MB';}"
        "async function loadList(){"
        "$('status').textContent='Reading SD...';"
        "try{const r=await fetch('/api/list');const d=await r.json();"
        "if(!d.ok){$('status').textContent=d.error||'SD read failed';return;}"
        "$('status').textContent=d.stories.length+' story folder(s), '+size(d.bytes||0);"
        "$('list').innerHTML=d.stories.map(st=>`<div class='story'><div class='storyHead'><div><div class='storyTitle'>${esc(st.name)}</div><div class='path'>${esc(st.path)}</div></div><button class='danger' onclick='delPath(${JSON.stringify(st.path)})'>Delete</button></div>${(st.files||[]).map(f=>`<div class='file'><span>${esc(f.name)} <span class='path'>${size(f.size||0)}</span></span><button class='danger' onclick='delPath(${JSON.stringify(f.path)})'>Delete</button></div>`).join('')}</div>`).join('')||'<p class=sub>No stories found.</p>';}"
        "catch(e){$('status').textContent='Failed: '+e;}}"
        "async function delPath(p){if(!confirm('Delete '+p+'?'))return;"
        "const r=await fetch('/api/delete?path='+encodeURIComponent(p),{method:'POST'});"
        "log(await r.text());await loadList();}"
        "async function uploadChunk(story,file,offset,total){"
        "const end=Math.min(offset+65536,total);"
        "const qs='story='+encodeURIComponent(story)+'&filename='+encodeURIComponent(file.name)+'&offset='+offset+'&total='+total;"
        "let last='';"
        "for(let a=1;a<=3;a++){"
        "const r=await fetch('/api/upload?'+qs,{method:'POST',body:file.slice(offset,end)});"
        "last=await r.text();"
        "if(r.ok)return JSON.parse(last);"
        "log('Chunk retry '+a+'/3 for '+file.name+': '+last);"
        "await new Promise(res=>setTimeout(res,500*a));"
        "}"
        "throw new Error(last||'chunk upload failed');"
        "}"
        "async function uploadFile(story,f){"
        "let offset=0;const total=f.size;log('Uploading '+f.name+' ('+size(total)+') ...');"
        "while(offset<total){"
        "const d=await uploadChunk(story,f,offset,total);"
        "offset=d.next||0;"
        "if(offset%262144===0||offset>=total)log(f.name+': '+size(offset)+' / '+size(total));"
        "}"
        "}"
        "const isMp3=f=>/\\.mp3$/i.test(f.name||'');"
        "const isCover=f=>/^cover\\.(png|jpe?g|bmp)$/i.test(f.name||'');"
        "const ss=(b,i)=>((b[i]&127)<<21)|((b[i+1]&127)<<14)|((b[i+2]&127)<<7)|(b[i+3]&127);"
        "const be=(b,i)=>((b[i]<<24)>>>0)|(b[i+1]<<16)|(b[i+2]<<8)|b[i+3];"
        "const asc=(b,s,e)=>{let r='';for(let i=s;i<e;i++)r+=String.fromCharCode(b[i]);return r;};"
        "async function canvasJpeg(mime,data){"
        "const url=URL.createObjectURL(new Blob([data],{type:mime||'image/jpeg'}));"
        "try{const img=new Image();img.src=url;await img.decode();"
        "const side=360,c=document.createElement('canvas');c.width=side;c.height=side;"
        "const x=c.getContext('2d');x.fillStyle='#fff';x.fillRect(0,0,side,side);"
        "const sc=Math.max(side/img.width,side/img.height),w=img.width*sc,h=img.height*sc;"
        "x.drawImage(img,(side-w)/2,(side-h)/2,w,h);"
        "const blob=await new Promise(res=>c.toBlob(res,'image/jpeg',0.86));"
        "if(!blob)return null;"
        "if(typeof File==='function')return new File([blob],'cover.jpg',{type:'image/jpeg'});"
        "blob.name='cover.jpg';return blob;"
        "}finally{URL.revokeObjectURL(url);}"
        "}"
        "async function coverFromMp3(f){"
        "try{if(!f)return null;const h=new Uint8Array(await f.slice(0,10).arrayBuffer());"
        "if(asc(h,0,3)!=='ID3')return null;const ver=h[3],total=10+ss(h,6)+((h[5]&16)?10:0);"
        "if(total<=10||total>2097152)return null;"
        "const b=new Uint8Array(await f.slice(10,total).arrayBuffer());"
        "let o=0;while(o+10<=b.length){const id=asc(b,o,o+4);"
        "if(!/^[A-Z0-9]{4}$/.test(id))break;const n=(ver===4)?ss(b,o+4):be(b,o+4);"
        "if(!n||o+10+n>b.length)break;"
        "if(id==='APIC'){let p=o+10,end=o+10+n,enc=b[p++],ms=p;"
        "while(p<end&&b[p]!==0)p++;let mime=asc(b,ms,p).toLowerCase();p++;p++;"
        "if(enc===0||enc===3){while(p<end&&b[p]!==0)p++;p++;}"
        "else{while(p+1<end&&!(b[p]===0&&b[p+1]===0))p+=2;p+=2;}"
        "if(p<end){if(!mime)mime='image/jpeg';return await canvasJpeg(mime,b.slice(p,end));}"
        "}"
        "o+=10+n;"
        "}}catch(e){log('Embedded cover skipped: '+e);}"
        "return null;"
        "}"
        "async function uploadAll(){"
        "const story=$('story').value.trim();"
        "const files=[...$('files').files];"
        "if(!story){log('Missing story name');return;}"
        "if(files.length===0){log('No files selected');return;}"
        "try{"
        "if(!files.some(isCover)){const cover=await coverFromMp3(files.find(isMp3));"
        "if(cover){log('Generated compatible cover.jpg from MP3 art');await uploadFile(story,cover);}"
        "else log('No embedded cover found; upload cover.jpg/png for artwork');}"
        "for(const f of files){await uploadFile(story,f);}"
        "}catch(e){log('Upload failed: '+e);return;}"
        "const done=await fetch('/api/finalize?story='+encodeURIComponent(story),{method:'POST'});"
        "log('Finalize: '+await done.text());"
        "log('Done.');await loadList();"
        "}"
        "loadList();"
        "</script></main></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_upload_favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t wifi_upload_api_list_handler(httpd_req_t *req)
{
    if (!wifi_sd_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"SD not ready\"}", HTTPD_RESP_USE_STRLEN);
    }

    DIR *root = opendir(AUDIO_MOUNT_POINT);
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"Cannot open SD\"}", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    http_out_t out;
    http_out_init(&out, req);
    esp_err_t err = http_send_chunk(&out, "{\"ok\":true,\"stories\":[");
    bool first_story = true;
    uint64_t total_bytes = 0;
    struct dirent *story_ent;

    while (err == ESP_OK && (story_ent = readdir(root)) != NULL) {
        if (story_ent->d_name[0] == '.') {
            continue;
        }

        char story_path[WIFI_UPLOAD_PATH_MAX];
        if (!path_join2(story_path, sizeof(story_path), AUDIO_MOUNT_POINT, story_ent->d_name)) {
            continue;
        }

        struct stat story_st;
        if (stat(story_path, &story_st) != 0 || !S_ISDIR(story_st.st_mode)) {
            continue;
        }

        if (!first_story) {
            err = http_send_chunk(&out, ",");
            if (err != ESP_OK) break;
        }
        first_story = false;

        err = http_send_chunk(&out, "{\"name\":");
        if (err == ESP_OK) err = http_send_json_string(&out, story_ent->d_name);
        if (err == ESP_OK) err = http_send_chunk(&out, ",\"path\":");
        if (err == ESP_OK) err = http_send_json_string(&out, story_ent->d_name);
        if (err == ESP_OK) err = http_send_chunk(&out, ",\"files\":[");
        if (err != ESP_OK) break;

        DIR *story_dir = opendir(story_path);
        bool first_file = true;
        if (story_dir) {
            struct dirent *file_ent;
            while (err == ESP_OK && (file_ent = readdir(story_dir)) != NULL) {
                if (file_ent->d_name[0] == '.') {
                    continue;
                }

                char file_path[WIFI_UPLOAD_PATH_MAX];
                if (!path_join2(file_path, sizeof(file_path), story_path, file_ent->d_name)) {
                    continue;
                }

                struct stat file_st;
                if (stat(file_path, &file_st) != 0 || S_ISDIR(file_st.st_mode)) {
                    continue;
                }
                total_bytes += (uint64_t)file_st.st_size;

                char rel_path[WIFI_UPLOAD_PATH_MAX];
                if (!path_join2(rel_path, sizeof(rel_path), story_ent->d_name, file_ent->d_name)) {
                    continue;
                }

                if (!first_file) {
                    err = http_send_chunk(&out, ",");
                    if (err != ESP_OK) break;
                }
                first_file = false;

                char meta[80];
                snprintf(meta, sizeof(meta), ",\"size\":%llu}", (unsigned long long)file_st.st_size);
                err = http_send_chunk(&out, "{\"name\":");
                if (err == ESP_OK) err = http_send_json_string(&out, file_ent->d_name);
                if (err == ESP_OK) err = http_send_chunk(&out, ",\"path\":");
                if (err == ESP_OK) err = http_send_json_string(&out, rel_path);
                if (err == ESP_OK) err = http_send_chunk(&out, meta);
            }
            closedir(story_dir);
        }

        if (err == ESP_OK) {
            err = http_send_chunk(&out, "]}");
        }
    }
    closedir(root);

    if (err == ESP_OK) {
        char tail[64];
        snprintf(tail, sizeof(tail), "],\"bytes\":%llu}", (unsigned long long)total_bytes);
        err = http_send_chunk(&out, tail);
    }
    return http_out_finish(&out, err);
}

static esp_err_t wifi_upload_api_delete_handler(httpd_req_t *req)
{
    if (!wifi_sd_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "SD not ready", HTTPD_RESP_USE_STRLEN);
    }

    int qlen = httpd_req_get_url_query_len(req);
    if (qlen <= 0 || qlen >= WIFI_UPLOAD_QUERY_MAX) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Missing path", HTTPD_RESP_USE_STRLEN);
    }

    char query[WIFI_UPLOAD_QUERY_MAX];
    char rel_raw[WIFI_UPLOAD_PATH_MAX] = {0};
    char target[WIFI_UPLOAD_PATH_MAX];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", rel_raw, sizeof(rel_raw)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Bad path", HTTPD_RESP_USE_STRLEN);
    }
    url_decode_inplace(rel_raw);

    if (!build_sd_path_from_relative(rel_raw, target, sizeof(target))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Unsafe path", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = delete_path_recursive(target, 0);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Delete failed", HTTPD_RESP_USE_STRLEN);
    }

    (void)audio_backend_rescan_catalog();

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Deleted", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_upload_api_upload_handler(httpd_req_t *req)
{
    if (!wifi_sd_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "SD not ready", HTTPD_RESP_USE_STRLEN);
    }

    int qlen = httpd_req_get_url_query_len(req);
    if (qlen <= 0 || qlen >= WIFI_UPLOAD_QUERY_MAX) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Missing query", HTTPD_RESP_USE_STRLEN);
    }

    char query[WIFI_UPLOAD_QUERY_MAX];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Bad query", HTTPD_RESP_USE_STRLEN);
    }

    char story_raw[96] = {0};
    char file_raw[96] = {0};
    if (httpd_query_key_value(query, "story", story_raw, sizeof(story_raw)) != ESP_OK ||
        httpd_query_key_value(query, "filename", file_raw, sizeof(file_raw)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Need story and filename", HTTPD_RESP_USE_STRLEN);
    }

    uint64_t upload_offset = 0;
    uint64_t upload_total = (uint64_t)req->content_len;
    char offset_raw[24] = {0};
    char total_raw[24] = {0};
    if (httpd_query_key_value(query, "offset", offset_raw, sizeof(offset_raw)) == ESP_OK &&
        !parse_u64_dec(offset_raw, &upload_offset)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Bad offset", HTTPD_RESP_USE_STRLEN);
    }
    if (httpd_query_key_value(query, "total", total_raw, sizeof(total_raw)) == ESP_OK &&
        !parse_u64_dec(total_raw, &upload_total)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Bad total", HTTPD_RESP_USE_STRLEN);
    }
    if (upload_offset + (uint64_t)req->content_len > upload_total) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Chunk exceeds total", HTTPD_RESP_USE_STRLEN);
    }
    url_decode_inplace(story_raw);
    url_decode_inplace(file_raw);

    char story_name[96];
    char file_name[96];
    sanitize_path_component(story_raw, story_name, sizeof(story_name), "story");
    sanitize_path_component(file_raw, file_name, sizeof(file_name), "track.mp3");

    char story_dir[WIFI_UPLOAD_PATH_MAX];
    esp_err_t err = ensure_story_directory(story_name, story_dir, sizeof(story_dir));
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Cannot create story directory", HTTPD_RESP_USE_STRLEN);
    }

    char target[WIFI_UPLOAD_PATH_MAX];
    if (!path_join2(target, sizeof(target), story_dir, file_name)) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_send(req, "File name too long", HTTPD_RESP_USE_STRLEN);
    }

    if (upload_offset > 0) {
        struct stat st;
        int stat_ret = stat(target, &st);
        int actual_size = (stat_ret == 0) ? (int)st.st_size : -1;
        if (stat_ret != 0 || (uint64_t)st.st_size != upload_offset) {
            ESP_LOGW(TAG,
                     "Upload offset mismatch: %s/%s requested=%u actual=%d",
                     story_name,
                     file_name,
                     (unsigned)upload_offset,
                     actual_size);
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_send(req, "Offset mismatch", HTTPD_RESP_USE_STRLEN);
        }
    }

    FILE *fp = fopen(target, upload_offset == 0 ? "wb" : "ab");
    if (!fp) {
        ESP_LOGW(TAG, "Cannot open upload target %s (errno=%d)", target, errno);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Cannot open target file", HTTPD_RESP_USE_STRLEN);
    }

    char *buf = (char *)heap_caps_malloc(WIFI_UPLOAD_RECV_BUF_SIZE, MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(fp);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "No upload buffer", HTTPD_RESP_USE_STRLEN);
    }

    int remaining = req->content_len;
    int timeout_count = 0;
    size_t written = 0;
    size_t last_progress_log = 0;
    bool failed = false;
    bool timed_out = false;

    ESP_LOGI(TAG,
             "Upload chunk start: %s/%s offset=%u chunk=%d total=%u",
             story_name,
             file_name,
             (unsigned)upload_offset,
             remaining,
             (unsigned)upload_total);

    while (remaining > 0) {
        int chunk = remaining;
        if (chunk > WIFI_UPLOAD_RECV_BUF_SIZE) {
            chunk = WIFI_UPLOAD_RECV_BUF_SIZE;
        }
        int received = httpd_req_recv(req, buf, chunk);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            timeout_count++;
            ESP_LOGW(TAG,
                     "Upload waiting for data: %s/%s written=%u remaining=%d timeout=%d/%d",
                     story_name,
                     file_name,
                     (unsigned)(upload_offset + written),
                     remaining,
                     timeout_count,
                     WIFI_UPLOAD_RECV_TIMEOUT_MAX);
            if (timeout_count >= WIFI_UPLOAD_RECV_TIMEOUT_MAX) {
                timed_out = true;
                failed = true;
                break;
            }
            continue;
        }
        if (received <= 0) {
            failed = true;
            break;
        }
        if (fwrite(buf, 1, (size_t)received, fp) != (size_t)received) {
            failed = true;
            break;
        }
        remaining -= received;
        written += (size_t)received;
        timeout_count = 0;
        if (written - last_progress_log >= WIFI_UPLOAD_PROGRESS_STEP_BYTES || remaining == 0) {
            ESP_LOGI(TAG,
                     "Upload progress: %s/%s %u/%u bytes",
                     story_name,
                     file_name,
                     (unsigned)(upload_offset + written),
                     (unsigned)upload_total);
            last_progress_log = written;
        }
    }
    free(buf);
    fclose(fp);

    if (failed) {
        if (upload_offset == 0) {
            (void)remove(target);
        }
        ESP_LOGW(TAG,
                 "Upload failed: %s/%s written=%u expected=%u%s",
                 story_name,
                 file_name,
                 (unsigned)(upload_offset + written),
                 (unsigned)upload_total,
                 timed_out ? " timeout" : "");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, timed_out ? "Upload timeout" : "Upload failed", HTTPD_RESP_USE_STRLEN);
    }

    const uint64_t next_offset = upload_offset + written;
    const bool done = (next_offset >= upload_total);
    ESP_LOGI(TAG,
             "Upload chunk complete: %s/%s next=%u/%u%s",
             story_name,
             file_name,
             (unsigned)next_offset,
             (unsigned)upload_total,
             done ? " done" : "");

    char rel_file[WIFI_UPLOAD_PATH_MAX];
    if (!path_join2(rel_file, sizeof(rel_file), story_name, file_name)) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_send(req, "File name too long", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    http_out_t out;
    http_out_init(&out, req);
    err = http_send_chunk(&out, "{\"ok\":true,\"file\":");
    if (err == ESP_OK) err = http_send_json_string(&out, rel_file);
    if (err == ESP_OK) {
        char meta[96];
        snprintf(meta,
                 sizeof(meta),
                 ",\"offset\":%u,\"received\":%u,\"next\":%u,\"total\":%u,\"done\":%s}",
                 (unsigned)upload_offset,
                 (unsigned)written,
                 (unsigned)next_offset,
                 (unsigned)upload_total,
                 done ? "true" : "false");
        err = http_send_chunk(&out, meta);
    }
    return http_out_finish(&out, err);
}

static esp_err_t wifi_upload_api_finalize_handler(httpd_req_t *req)
{
    if (!wifi_sd_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "SD not ready", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = audio_backend_rescan_catalog();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Catalog rescan failed", HTTPD_RESP_USE_STRLEN);
    }

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"stories\":%u}", (unsigned)audio_backend_story_count());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_upload_start_http_server(void)
{
    if (s_wifi_httpd) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_AUDIO_BACKEND_WIFI_HTTP_PORT;
    cfg.ctrl_port = (cfg.server_port == 65535) ? 32768 : (cfg.server_port + 1);
    cfg.stack_size = 12 * 1024;
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    /*
     * Handlers only touch the SD card (no NVS/flash writes), so the stack can
     * live in PSRAM; that frees 12 KB of the internal RAM Wi-Fi needs.
     */
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    cfg.max_uri_handlers = 6;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;

    esp_err_t err = httpd_start(&s_wifi_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_start failed (%s)", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = wifi_upload_index_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t uri_favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = wifi_upload_favicon_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t uri_upload = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = wifi_upload_api_upload_handler,
        .user_ctx = NULL
    };
    httpd_uri_t uri_list = {
        .uri = "/api/list",
        .method = HTTP_GET,
        .handler = wifi_upload_api_list_handler,
        .user_ctx = NULL
    };
    httpd_uri_t uri_delete = {
        .uri = "/api/delete",
        .method = HTTP_POST,
        .handler = wifi_upload_api_delete_handler,
        .user_ctx = NULL
    };
    httpd_uri_t uri_finalize = {
        .uri = "/api/finalize",
        .method = HTTP_POST,
        .handler = wifi_upload_api_finalize_handler,
        .user_ctx = NULL
    };

    err = httpd_register_uri_handler(s_wifi_httpd, &uri_index);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_wifi_httpd, &uri_favicon);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_wifi_httpd, &uri_upload);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_wifi_httpd, &uri_list);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_wifi_httpd, &uri_delete);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_wifi_httpd, &uri_finalize);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd uri register failed (%s)", esp_err_to_name(err));
        httpd_stop(s_wifi_httpd);
        s_wifi_httpd = NULL;
    }
    return err;
}

static esp_err_t wifi_upload_init_stack(void)
{
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (!s_wifi_netif) {
        ESP_LOGI(TAG,
                 "Wi-Fi netif create: ifs=%u free8=%u free_internal=%u free_psram=%u",
                 (unsigned)esp_netif_get_nr_of_ifs(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        s_wifi_netif = esp_netif_create_wifi(WIFI_IF_STA, ESP_NETIF_BASE_DEFAULT_WIFI_STA);
        if (!s_wifi_netif) {
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Wi-Fi netif created: ifs=%u", (unsigned)esp_netif_get_nr_of_ifs());
    }

    if (!s_wifi_stack_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_wifi_evt_any_id);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &s_ip_evt_got_ip);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK) {
            return err;
        }
        s_wifi_stack_initialized = true;
    }

    return ESP_OK;
}

static esp_err_t wifi_start_scan_only(void)
{
    if (!s_wifi_event_group) {
        return ESP_FAIL;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_STA_STARTED_BIT);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi scan: mode failed");
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi scan: start failed");
        return err;
    }

    s_wifi_started = true;
    /* Modem sleep delays inbound packets by up to a DTIM interval; keep the radio awake for the portal. */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    s_wifi_connected = false;
    s_wifi_ip[0] = '\0';

    EventBits_t start_bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_STA_STARTED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(3000));
    if (!(start_bits & WIFI_STA_STARTED_BIT)) {
        (void)audio_backend_wifi_upload_stop();
        wifi_status_set("Wi-Fi scan: start timeout");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static esp_err_t wifi_connect_sta_for_scan(void)
{
    if (!s_wifi_event_group) {
        return ESP_FAIL;
    }
    if (s_wifi_ssid[0] == '\0') {
        return wifi_start_scan_only();
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_STA_STARTED_BIT);

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, s_wifi_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, s_wifi_password, sizeof(wifi_cfg.sta.password));
    /* Mesh/multi-AP networks share one SSID: scan all channels and join the strongest node. */
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg.sta.failure_retry_cnt = WIFI_MAX_RETRY;
    wifi_cfg.sta.threshold.authmode = (s_wifi_password[0] == '\0')
                                    ? WIFI_AUTH_OPEN
                                    : WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi scan: mode failed");
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi scan: config failed");
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi scan: start failed");
        return err;
    }

    s_wifi_started = true;
    /* Modem sleep delays inbound packets by up to a DTIM interval; keep the radio awake for the portal. */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    s_wifi_connected = false;
    s_wifi_ip[0] = '\0';

    EventBits_t start_bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_STA_STARTED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(3000));
    if (!(start_bits & WIFI_STA_STARTED_BIT)) {
        (void)audio_backend_wifi_upload_stop();
        wifi_status_set("Wi-Fi scan: start timeout");
        return ESP_ERR_TIMEOUT;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        (void)audio_backend_wifi_upload_stop();
        wifi_status_set("Wi-Fi scan: connect failed");
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(CONFIG_AUDIO_BACKEND_WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    (void)audio_backend_wifi_upload_stop();
    wifi_status_set("Wi-Fi scan: connect timeout");
    return ESP_ERR_TIMEOUT;
}

static int wifi_ap_rssi_desc_cmp(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *rb = (const wifi_ap_record_t *)b;
    return (int)rb->rssi - (int)ra->rssi;
}

static const char *wifi_authmode_name(uint8_t authmode)
{
    switch ((wifi_auth_mode_t)authmode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa/wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2-ent";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2/wpa3";
    default:
        return "secure";
    }
}

static esp_err_t wifi_scan_start_with_retry(const wifi_scan_config_t *scan_cfg)
{
    esp_err_t err = ESP_FAIL;
    for (uint8_t attempt = 0; attempt < WIFI_SCAN_RETRY_COUNT; ++attempt) {
        err = esp_wifi_scan_start(scan_cfg, true);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "Wi-Fi scan start failed attempt %u/%u (%s)",
                 (unsigned)(attempt + 1),
                 (unsigned)WIFI_SCAN_RETRY_COUNT,
                 esp_err_to_name(err));

        if (err != ESP_ERR_WIFI_STATE) {
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_RETRY_DELAY_MS));
    }
    return err;
}

esp_err_t audio_backend_wifi_scan(audio_backend_wifi_ap_t *aps, size_t max_aps, size_t *out_count)
{
    if (!out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!aps || max_aps == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = wifi_upload_init_stack();
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi scan: init failed");
        return err;
    }
    if (!s_wifi_event_group) {
        wifi_status_set("Wi-Fi scan: event group failed");
        return ESP_FAIL;
    }

    const bool was_running = audio_backend_wifi_upload_is_running();
    bool started_for_scan = false;
    wifi_credentials_load_once();

    /*
     * ESP-Hosted's injected esp_wifi API cannot scan while the remote STA is
     * still in its pre-connect state. Connect first, then scan over the active
     * hosted link. This is heavier than native Wi-Fi, but it avoids the endless
     * ESP_ERR_WIFI_STATE loop from the C6 firmware.
     */
#if CONFIG_SOC_WIFI_SUPPORTED
    /* Native Wi-Fi (ESP32-S3) scans from any STA state; leave a live link and portal alone. */
    (void)was_running;
    if (!s_wifi_started) {
        wifi_status_set("Wi-Fi scan: starting STA...");
        err = wifi_start_scan_only();
        if (err != ESP_OK) {
            return err;
        }
        started_for_scan = true;
    }
#else
    if (!s_wifi_connected) {
        if (s_wifi_started) {
            (void)audio_backend_wifi_upload_stop();
        }

        if (s_wifi_ssid[0] != '\0') {
            wifi_status_set("Wi-Fi scan: connecting...");
            ESP_LOGI(TAG, "Wi-Fi scan: connecting STA before scan_start");
            err = wifi_connect_sta_for_scan();
        } else {
            wifi_status_set("Wi-Fi scan: starting STA...");
            ESP_LOGI(TAG, "Wi-Fi scan: starting STA without saved credentials");
            err = wifi_start_scan_only();
        }
        if (err != ESP_OK) {
            return err;
        }
        started_for_scan = !was_running;
    }
#endif

    wifi_status_set("Wi-Fi: scanning...");

#if !CONFIG_SOC_WIFI_SUPPORTED
    /*
     * ESP-Hosted reports WIFI_EVENT_STA_START before the C6 is always
     * ready to accept a scan RPC. Settle here even if Wi-Fi was already
     * started by a previous attempt, because failed scans can leave the
     * remote side briefly busy.
     */
    ESP_LOGI(TAG, "Wi-Fi scan: settle %ums before scan_start", (unsigned)WIFI_SCAN_START_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_START_SETTLE_MS));
#endif

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = wifi_scan_start_with_retry(&scan_cfg);
    if (err != ESP_OK) {
        if (started_for_scan) {
            (void)audio_backend_wifi_upload_stop();
        }
        wifi_status_set("Wi-Fi scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t found_total = 0;
    (void)esp_wifi_scan_get_ap_num(&found_total);

    uint16_t record_count = (uint16_t)max_aps;
    wifi_ap_record_t *records = calloc(max_aps, sizeof(wifi_ap_record_t));
    if (!records) {
        if (started_for_scan) {
            (void)audio_backend_wifi_upload_stop();
        }
        wifi_status_set("Wi-Fi scan: no memory");
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&record_count, records);
    if (err != ESP_OK) {
        free(records);
        if (started_for_scan) {
            (void)audio_backend_wifi_upload_stop();
        }
        wifi_status_set("Wi-Fi scan read failed");
        return err;
    }

    qsort(records, record_count, sizeof(records[0]), wifi_ap_rssi_desc_cmp);

    for (uint16_t i = 0; i < record_count; i++) {
        strlcpy(aps[i].ssid, (const char *)records[i].ssid, sizeof(aps[i].ssid));
        if (aps[i].ssid[0] == '\0') {
            strlcpy(aps[i].ssid, "<hidden>", sizeof(aps[i].ssid));
        }
        aps[i].rssi = records[i].rssi;
        aps[i].channel = records[i].primary;
        aps[i].authmode = (uint8_t)records[i].authmode;
        ESP_LOGI(TAG,
                 "Wi-Fi AP[%u/%u]: ssid=\"%s\" rssi=%d ch=%u auth=%s",
                 (unsigned)(i + 1),
                 (unsigned)found_total,
                 aps[i].ssid,
                 (int)aps[i].rssi,
                 (unsigned)aps[i].channel,
                 wifi_authmode_name(aps[i].authmode));
    }

    *out_count = record_count;
    free(records);

    if (started_for_scan) {
        (void)audio_backend_wifi_upload_stop();
    }

    if (record_count == 0) {
        wifi_status_set("Scan: no networks");
    } else {
        wifi_status_set("Scan: %u found, best %s %ddBm",
                        (unsigned)found_total,
                        aps[0].ssid,
                        (int)aps[0].rssi);
    }

    return ESP_OK;
}

esp_err_t audio_backend_wifi_scan_summary(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_backend_wifi_ap_t aps[AUDIO_BACKEND_WIFI_SCAN_MAX];
    size_t count = 0;
    esp_err_t err = audio_backend_wifi_scan(aps, AUDIO_BACKEND_WIFI_SCAN_MAX, &count);
    if (err != ESP_OK) {
        strlcpy(out, audio_backend_wifi_upload_get_status(), out_size);
        return err;
    }

    if (count == 0) {
        strlcpy(out, audio_backend_wifi_upload_get_status(), out_size);
        return ESP_OK;
    }

    strlcpy(out, audio_backend_wifi_upload_get_status(), out_size);
    return ESP_OK;
}

esp_err_t audio_backend_wifi_upload_start(void)
{
    if (audio_backend_wifi_upload_is_running()) {
        return ESP_OK;
    }

    wifi_credentials_load_once();
    if (s_wifi_ssid[0] == '\0') {
        wifi_status_set("Wi-Fi: select a network");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wifi_upload_init_stack();
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi: init failed");
        return err;
    }

    if (!s_wifi_event_group) {
        wifi_status_set("Wi-Fi: event group failed");
        return ESP_FAIL;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_STA_STARTED_BIT);

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,
            s_wifi_ssid,
            sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password,
            s_wifi_password,
            sizeof(wifi_cfg.sta.password));
    /* Mesh/multi-AP networks share one SSID: scan all channels and join the strongest node. */
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg.sta.failure_retry_cnt = WIFI_MAX_RETRY;
    wifi_cfg.sta.threshold.authmode = (s_wifi_password[0] == '\0')
                                    ? WIFI_AUTH_OPEN
                                    : WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi: mode failed");
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi: config failed");
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        wifi_status_set("Wi-Fi: start failed");
        return err;
    }

    s_wifi_started = true;
    /* Modem sleep delays inbound packets by up to a DTIM interval; keep the radio awake for the portal. */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    s_wifi_connected = false;
    s_wifi_ip[0] = '\0';
    wifi_status_set("Wi-Fi: connecting...");

    EventBits_t start_bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_STA_STARTED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(3000));
    if (!(start_bits & WIFI_STA_STARTED_BIT)) {
        (void)audio_backend_wifi_upload_stop();
        wifi_status_set("Wi-Fi: start timeout");
        return ESP_ERR_TIMEOUT;
    }

    /*
     * ESP-Hosted can emit STA_START while LWIP is still adding the netif on
     * tcpip_thread. Connecting from inside the event callback can race that
     * start path and make lwIP try to add the same netif twice.
     */
    vTaskDelay(pdMS_TO_TICKS(100));
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        (void)audio_backend_wifi_upload_stop();
        wifi_status_set("Wi-Fi: connect failed");
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(CONFIG_AUDIO_BACKEND_WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        err = wifi_upload_start_http_server();
        if (err == ESP_OK) {
            if (CONFIG_AUDIO_BACKEND_WIFI_HTTP_PORT == 80) {
                wifi_status_set("Upload: http://%s", s_wifi_ip);
            } else {
                wifi_status_set("Upload: http://%s:%d", s_wifi_ip, CONFIG_AUDIO_BACKEND_WIFI_HTTP_PORT);
            }
            ESP_LOGI(TAG, "%s", s_wifi_status);
            return ESP_OK;
        }

        (void)audio_backend_wifi_upload_stop();
        wifi_status_set("Wi-Fi: HTTP server failed");
        return err;
    }

    (void)audio_backend_wifi_upload_stop();
    wifi_status_set("Wi-Fi: connect timeout");
    return ESP_ERR_TIMEOUT;
}

esp_err_t audio_backend_wifi_upload_stop(void)
{
    if (s_wifi_httpd) {
        httpd_stop(s_wifi_httpd);
        s_wifi_httpd = NULL;
    }

    bool was_started = s_wifi_started;
    s_wifi_started = false;
    if (was_started && s_wifi_stack_initialized) {
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
    }

    s_wifi_connected = false;
    s_wifi_retry_count = 0;
    s_wifi_ip[0] = '\0';
    wifi_status_set("Wi-Fi: off");
    return ESP_OK;
}

bool audio_backend_wifi_upload_is_running(void)
{
    return (s_wifi_started && s_wifi_httpd != NULL);
}

bool audio_backend_wifi_upload_is_connected(void)
{
    return s_wifi_connected;
}

esp_err_t audio_backend_wifi_upload_get_url(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_wifi_connected || s_wifi_ip[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    int n = 0;
    if (CONFIG_AUDIO_BACKEND_WIFI_HTTP_PORT == 80) {
        n = snprintf(out, out_size, "http://%s", s_wifi_ip);
    } else {
        n = snprintf(out, out_size, "http://%s:%d", s_wifi_ip, CONFIG_AUDIO_BACKEND_WIFI_HTTP_PORT);
    }
    if (n < 0 || (size_t)n >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

const char *audio_backend_wifi_upload_get_status(void)
{
    return s_wifi_status;
}

esp_err_t audio_backend_wifi_get_saved_network(char *ssid, size_t ssid_size, bool *out_has_saved)
{
    wifi_credentials_load_once();
    if (out_has_saved) {
        *out_has_saved = s_wifi_has_saved_credentials || s_wifi_ssid[0] != '\0';
    }
    if (ssid && ssid_size > 0) {
        strlcpy(ssid, s_wifi_ssid, ssid_size);
    }
    return ESP_OK;
}

esp_err_t audio_backend_wifi_forget_saved_network(void)
{
    esp_err_t err = wifi_credentials_erase();
    s_wifi_credentials_loaded = true;
    s_wifi_has_saved_credentials = false;
    s_wifi_ssid[0] = '\0';
    s_wifi_password[0] = '\0';
    if (audio_backend_wifi_upload_is_running() || s_wifi_connected) {
        (void)audio_backend_wifi_upload_stop();
    }
    wifi_status_set("Wi-Fi: network forgotten");
    return err;
}

esp_err_t audio_backend_wifi_connect_network(const char *ssid, const char *password, bool save)
{
    if (!ssid || ssid[0] == '\0') {
        wifi_status_set("Wi-Fi: SSID missing");
        return ESP_ERR_INVALID_ARG;
    }

    char old_ssid[AUDIO_BACKEND_WIFI_SSID_MAX];
    char old_password[AUDIO_BACKEND_WIFI_PASSWORD_MAX];
    wifi_credentials_load_once();
    strlcpy(old_ssid, s_wifi_ssid, sizeof(old_ssid));
    strlcpy(old_password, s_wifi_password, sizeof(old_password));

    if (s_wifi_started || s_wifi_connected) {
        (void)audio_backend_wifi_upload_stop();
    }

    strlcpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid));
    strlcpy(s_wifi_password, password ? password : "", sizeof(s_wifi_password));
    s_wifi_credentials_loaded = true;
    wifi_status_set("Wi-Fi: connecting to %s", s_wifi_ssid);

    esp_err_t err = wifi_connect_sta_for_scan();
    if (err != ESP_OK) {
        strlcpy(s_wifi_ssid, old_ssid, sizeof(s_wifi_ssid));
        strlcpy(s_wifi_password, old_password, sizeof(s_wifi_password));
        wifi_status_set("Wi-Fi: connect failed");
        return err;
    }

    if (save) {
        err = wifi_credentials_save(s_wifi_ssid, s_wifi_password);
        if (err != ESP_OK) {
            wifi_status_set("Wi-Fi connected, save failed");
            return err;
        }
        s_wifi_has_saved_credentials = true;
    }

    /* Being on the network without the portal looked like a dead device; serve it right away. */
    if (s_wifi_connected && wifi_upload_start_http_server() == ESP_OK) {
        wifi_status_set("Upload: http://%s", s_wifi_ip);
        ESP_LOGI(TAG, "%s (connected to %s)", s_wifi_status, s_wifi_ssid);
        return ESP_OK;
    }

    wifi_status_set("Wi-Fi: connected to %s", s_wifi_ssid);
    return ESP_OK;
}

#else

esp_err_t audio_backend_wifi_upload_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_backend_wifi_upload_stop(void)
{
    return ESP_OK;
}

bool audio_backend_wifi_upload_is_running(void)
{
    return false;
}

bool audio_backend_wifi_upload_is_connected(void)
{
    return false;
}

esp_err_t audio_backend_wifi_upload_get_url(char *out, size_t out_size)
{
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

const char *audio_backend_wifi_upload_get_status(void)
{
    return "Wi-Fi unsupported on this target";
}

esp_err_t audio_backend_wifi_get_saved_network(char *ssid, size_t ssid_size, bool *out_has_saved)
{
    if (ssid && ssid_size > 0) {
        ssid[0] = '\0';
    }
    if (out_has_saved) {
        *out_has_saved = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_backend_wifi_forget_saved_network(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_backend_wifi_connect_network(const char *ssid, const char *password, bool save)
{
    (void)ssid;
    (void)password;
    (void)save;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_backend_wifi_scan(audio_backend_wifi_ap_t *aps, size_t max_aps, size_t *out_count)
{
    (void)aps;
    (void)max_aps;
    if (out_count) {
        *out_count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_backend_wifi_scan_summary(char *out, size_t out_size)
{
    if (out && out_size > 0) {
        strlcpy(out, "Wi-Fi unsupported on this target", out_size);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
