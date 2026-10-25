/**
 * @file bsp_display.c
 * @brief BasmioTwo display/touch BSP for JC3248W535EN
 *
 * Display: AXS15231B, QSPI, 480x320 landscape
 * Touch:   AXS15231B-compatible controller, I2C
 * MCU:     ESP32-S3
 */

#include "bsp_display.h"
#include "ui_app.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <assert.h>
#include <inttypes.h>
#include <string.h>

static const char *TAG = "bsp";

#define BSP_LCD_PHYS_H_RES      320
#define BSP_LCD_PHYS_V_RES      480
#define BSP_LCD_H_RES           480
#define BSP_LCD_V_RES           320

#define BSP_LCD_BACKLIGHT_GPIO  GPIO_NUM_1
#define BSP_LCD_RST_GPIO        GPIO_NUM_NC
#define BSP_LCD_CS_GPIO         GPIO_NUM_45
#define BSP_LCD_PCLK_GPIO       GPIO_NUM_47
#define BSP_LCD_DATA0_GPIO      GPIO_NUM_21
#define BSP_LCD_DATA1_GPIO      GPIO_NUM_48
#define BSP_LCD_DATA2_GPIO      GPIO_NUM_40
#define BSP_LCD_DATA3_GPIO      GPIO_NUM_39
#define BSP_LCD_TE_GPIO         GPIO_NUM_38

#define BSP_TOUCH_I2C_PORT      I2C_NUM_0
#define BSP_TOUCH_SDA_GPIO      GPIO_NUM_4
#define BSP_TOUCH_SCL_GPIO      GPIO_NUM_8
#define BSP_TOUCH_INT_GPIO      GPIO_NUM_3
#define BSP_TOUCH_RST_GPIO      GPIO_NUM_NC
#define BSP_TOUCH_I2C_CLK_HZ    (400 * 1000)

#define BSP_LCD_HOST            SPI2_HOST
#define BSP_LCD_PIXEL_CLOCK_HZ  (50 * 1000 * 1000)
#define BSP_LVGL_BUF_LINES      32
#define BSP_PUSH_LINES          16
#define BSP_PUSH_BUF_COUNT      2

#define BSP_BL_LEDC_TIMER       LEDC_TIMER_0
#define BSP_BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BSP_BL_LEDC_FREQ_HZ     5000
#define BSP_BL_LEDC_RES         LEDC_TIMER_10_BIT

#define BSP_DISPLAY_PATTERN_ONLY 0

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static void *s_push_bufs[BSP_PUSH_BUF_COUNT];
static uint16_t *s_lcd_framebuffer = NULL;
static SemaphoreHandle_t s_flush_done_sem = NULL;
/* Highest physical row touched since the last push, -1 when nothing is dirty. */
static int s_dirty_row_max = -1;

static const axs15231b_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t []){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t []){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xf9, 0x10, 0x02, 0xff, 0xff, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t []){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t []){0xA0, 0x06, 0xAa, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t []){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0d, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t []){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t []){0x00, 0x24, 0x33, 0x80, 0x00, 0xea, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t []){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t []){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t []){0x50, 0x32, 0x28, 0x00, 0xa2, 0x80, 0x8f, 0x00, 0x80, 0xff, 0x07, 0x11, 0x9c, 0x67, 0xff, 0x24, 0x0c, 0x0d, 0x0e, 0x0f}, 20, 0},
    {0xC9, (uint8_t []){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t []){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t []){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t []){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t []){0x03, 0x01, 0x0b, 0x09, 0x0f, 0x0d, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t []){0x02, 0x00, 0x0a, 0x08, 0x0e, 0x0c, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19}, 12, 0},
    {0xD9, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t []){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8,  0},
    {0xE0, (uint8_t []){0x3B, 0x28, 0x10, 0x16, 0x0c, 0x06, 0x11, 0x28, 0x5c, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t []){0x37, 0x28, 0x10, 0x16, 0x0b, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 20},
};

static void flush_sem_ensure(void)
{
    if (!s_flush_done_sem) {
        /* Counting: up to BSP_PUSH_BUF_COUNT colour transfers can complete before we take. */
        s_flush_done_sem = xSemaphoreCreateCounting(BSP_PUSH_BUF_COUNT, 0);
    }
    assert(s_flush_done_sem);
}

static void draw_native_fill(uint16_t color)
{
    flush_sem_ensure();

    const int lines = 16;
    const size_t pixels = (size_t)BSP_LCD_PHYS_H_RES * lines;
    uint16_t *buf = heap_caps_malloc(pixels * sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    assert(buf);

    for (size_t i = 0; i < pixels; i++) {
        buf[i] = color;
    }

    for (int y = 0; y < BSP_LCD_PHYS_V_RES; y += lines) {
        const int h = (BSP_LCD_PHYS_V_RES - y) > lines ? lines : (BSP_LCD_PHYS_V_RES - y);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel,
                                                  0,
                                                  y,
                                                  BSP_LCD_PHYS_H_RES,
                                                  y + h,
                                                  buf));
        xSemaphoreTake(s_flush_done_sem, portMAX_DELAY);
    }

    free(buf);
}

/*
 * Streams physical rows [0, row_end) to the panel. In QSPI mode the AXS15231B
 * driver sends no RASET: a write at row 0 uses RAMWR, every other write uses
 * RAMWRC and continues where the previous one stopped. So a push must always
 * start at row 0, but it can stop right after the last dirty row.
 * Two bounce buffers let the PSRAM->DMA copy overlap the previous transfer.
 */
static void push_native_rows(int row_end)
{
    assert(s_push_bufs[0] && s_push_bufs[1] && s_flush_done_sem && s_lcd_framebuffer);

    const size_t line_bytes = (size_t)BSP_LCD_PHYS_H_RES * sizeof(uint16_t);
    int inflight = 0;
    int k = 0;
    for (int y = 0; y < row_end; y += BSP_PUSH_LINES) {
        const int h = (row_end - y) > BSP_PUSH_LINES ? BSP_PUSH_LINES : (row_end - y);
        if (inflight == BSP_PUSH_BUF_COUNT) {
            /* Transfers finish in order, so this frees the buffer we are about to reuse. */
            xSemaphoreTake(s_flush_done_sem, portMAX_DELAY);
            inflight--;
        }
        memcpy(s_push_bufs[k], &s_lcd_framebuffer[(size_t)y * BSP_LCD_PHYS_H_RES], (size_t)h * line_bytes);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_PHYS_H_RES, y + h, s_push_bufs[k]));
        inflight++;
        k = (k + 1) % BSP_PUSH_BUF_COUNT;
    }
    while (inflight-- > 0) {
        xSemaphoreTake(s_flush_done_sem, portMAX_DELAY);
    }
}

#if BSP_DISPLAY_PATTERN_ONLY
static void draw_native_test_pattern(void)
{
    flush_sem_ensure();

    const int lines = 16;
    const size_t pixels = (size_t)BSP_LCD_PHYS_H_RES * lines;
    uint8_t *buf = heap_caps_malloc(pixels * sizeof(uint16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    assert(buf);

    const uint8_t colors[][2] = {
        {0xF8, 0x00}, // red, RGB565 big-endian
        {0x07, 0xE0}, // green
        {0x00, 0x1F}, // blue
        {0xFF, 0xFF}, // white
        {0x00, 0x00}, // black
        {0xFF, 0xE0}, // yellow
        {0x07, 0xFF}, // cyan
        {0xF8, 0x1F}, // magenta
    };
    const int color_count = (int)(sizeof(colors) / sizeof(colors[0]));

    ESP_LOGI(TAG, "Drawing native LCD test pattern");
    for (int y = 0; y < BSP_LCD_PHYS_V_RES; y += lines) {
        const int h = (BSP_LCD_PHYS_V_RES - y) > lines ? lines : (BSP_LCD_PHYS_V_RES - y);
        for (int row = 0; row < h; row++) {
            for (int x = 0; x < BSP_LCD_PHYS_H_RES; x++) {
                const int band = (x * color_count) / BSP_LCD_PHYS_H_RES;
                const size_t off = ((size_t)row * BSP_LCD_PHYS_H_RES + x) * sizeof(uint16_t);
                buf[off + 0] = colors[band][0];
                buf[off + 1] = colors[band][1];
            }
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel,
                                                  0,
                                                  y,
                                                  BSP_LCD_PHYS_H_RES,
                                                  y + h,
                                                  buf));
        xSemaphoreTake(s_flush_done_sem, portMAX_DELAY);
    }

    free(buf);
    ESP_LOGI(TAG, "Native LCD test pattern drawn");
}
#endif

static bool lvgl_flush_done_cb(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t high_task_woken = pdFALSE;
    if (s_flush_done_sem) {
        xSemaphoreGiveFromISR(s_flush_done_sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    int x_start = area->x1;
    int x_end = area->x2;
    int y_start = area->y1;
    int y_end = area->y2;

    if (!s_lcd_framebuffer || !s_push_bufs[0] || !s_flush_done_sem) {
        ESP_LOGW(TAG, "flush framebuffer unavailable");
        lv_disp_flush_ready(drv);
        return;
    }

    if (x_start < 0) x_start = 0;
    if (y_start < 0) y_start = 0;
    if (x_end >= BSP_LCD_H_RES) x_end = BSP_LCD_H_RES - 1;
    if (y_end >= BSP_LCD_V_RES) y_end = BSP_LCD_V_RES - 1;
    if (x_start > x_end || y_start > y_end) {
        /* Nothing visible here, but earlier areas of this refresh may still need pushing. */
        x_end = -1;
    }

    /*
     * Logical (lx, ly) maps to physical (px = PHYS_H - 1 - ly, py = lx). Walk one
     * logical column at a time so the PSRAM writes run along a physical row
     * (contiguous, descending px) instead of striding 640 bytes per pixel.
     * Panel expects big-endian RGB565; lv_color_t holds native RGB565.
     */
    const int width = area->x2 - area->x1 + 1;
    for (int lx = x_start; lx <= x_end; lx++) {
        uint16_t *dst = &s_lcd_framebuffer[(size_t)lx * BSP_LCD_PHYS_H_RES + (BSP_LCD_PHYS_H_RES - 1 - y_start)];
        const lv_color_t *src = &color_map[(size_t)(y_start - area->y1) * width + (size_t)(lx - area->x1)];
        for (int ly = y_start; ly <= y_end; ly++) {
            *dst-- = __builtin_bswap16(src->full);
            src += width;
        }
    }
    if (x_end > s_dirty_row_max) {
        s_dirty_row_max = x_end;
    }

    if (lv_disp_flush_is_last(drv) && s_dirty_row_max >= 0) {
        push_native_rows(s_dirty_row_max + 1);
        s_dirty_row_max = -1;
    }

    lv_disp_flush_ready(drv);
}

static void touch_process_landscape(esp_lcd_touch_handle_t tp,
                                    uint16_t *x,
                                    uint16_t *y,
                                    uint16_t *strength,
                                    uint8_t *point_num,
                                    uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;

    for (uint8_t i = 0; i < *point_num; i++) {
        uint16_t tmp = x[i];
        x[i] = y[i];
        y[i] = BSP_LCD_PHYS_H_RES - tmp;
        if (x[i] >= BSP_LCD_H_RES) x[i] = BSP_LCD_H_RES - 1;
        if (y[i] >= BSP_LCD_V_RES) y[i] = BSP_LCD_V_RES - 1;
    }
}
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint8_t cnt = 0;

    if (!tp || esp_lcd_touch_read_data(tp) != ESP_OK) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    if (esp_lcd_touch_get_coordinates(tp, x, y, NULL, &cnt, 1) && cnt > 0) {
        data->point.x = (lv_coord_t)x[0];
        data->point.y = (lv_coord_t)y[0];
        data->state = LV_INDEV_STATE_PR;
        ui_wakeup();
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BSP_BL_LEDC_RES,
        .timer_num = BSP_BL_LEDC_TIMER,
        .freq_hz = BSP_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BSP_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BSP_BL_LEDC_CHANNEL,
        .timer_sel = BSP_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
}

void bsp_backlight_set(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    uint32_t duty = ((uint32_t)pct * 1023u) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_BL_LEDC_CHANNEL);
}

void bsp_display_init(void)
{
    backlight_init();
    bsp_backlight_set(0);

    spi_bus_config_t buscfg = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_PCLK_GPIO,
        BSP_LCD_DATA0_GPIO,
        BSP_LCD_DATA1_GPIO,
        BSP_LCD_DATA2_GPIO,
        BSP_LCD_DATA3_GPIO,
        (size_t)BSP_LCD_PHYS_H_RES * BSP_LCD_PHYS_V_RES * sizeof(lv_color_t));
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t lcd_io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = AXS15231B_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS_GPIO,
                                                                           lvgl_flush_done_cb,
                                                                           &s_disp_drv);
    io_cfg.pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_HOST, &io_cfg, &lcd_io));

    axs15231b_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(lcd_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, false));
    ESP_LOGI(TAG, "AXS15231B panel ready (%dx%d physical, %dx%d logical)",
             BSP_LCD_PHYS_H_RES, BSP_LCD_PHYS_V_RES, BSP_LCD_H_RES, BSP_LCD_V_RES);
    flush_sem_ensure();

#if BSP_DISPLAY_PATTERN_ONLY
    bsp_backlight_set(100);
    draw_native_test_pattern();
    ESP_LOGI(TAG, "Display pattern-only test active; UI/audio startup is intentionally stopped");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
    draw_native_fill(0x0000);

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = BSP_TOUCH_I2C_PORT,
        .sda_io_num = BSP_TOUCH_SDA_GPIO,
        .scl_io_num = BSP_TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    tp_io_cfg.scl_speed_hz = BSP_TOUCH_I2C_CLK_HZ;
    esp_err_t touch_ret = esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io);
    if (touch_ret == ESP_OK) {
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = BSP_LCD_PHYS_H_RES,
            .y_max = BSP_LCD_PHYS_V_RES,
            .rst_gpio_num = BSP_TOUCH_RST_GPIO,
            .int_gpio_num = BSP_TOUCH_INT_GPIO,
            .process_coordinates = touch_process_landscape,
            .levels = {.reset = 0, .interrupt = 0},
            .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        };
        touch_ret = esp_lcd_touch_new_i2c_axs15231b(tp_io, &tp_cfg, &s_touch);
    }
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "AXS15231B touch init failed (%s); continuing without touch", esp_err_to_name(touch_ret));
        s_touch = NULL;
    }

    lv_init();
    size_t buf_px = (size_t)BSP_LCD_H_RES * BSP_LVGL_BUF_LINES;
    size_t buf_bytes = buf_px * sizeof(lv_color_t);
    const size_t push_bytes = (size_t)BSP_LCD_PHYS_H_RES * BSP_PUSH_LINES * sizeof(uint16_t);
    for (int i = 0; i < BSP_PUSH_BUF_COUNT; i++) {
        s_push_bufs[i] = heap_caps_malloc(push_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    s_lcd_framebuffer = heap_caps_calloc((size_t)BSP_LCD_PHYS_H_RES * BSP_LCD_PHYS_V_RES,
                                         sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "LCD push buffers: %p %p bytes=%u fb=%p",
             s_push_bufs[0], s_push_bufs[1], (unsigned)push_bytes, s_lcd_framebuffer);
    assert(s_push_bufs[0] && s_push_bufs[1] && s_lcd_framebuffer && s_flush_done_sem);

    /* Flush rotates into the framebuffer synchronously, so a second LVGL buffer would sit idle. */
    lv_color_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf1) {
        ESP_LOGW(TAG, "Internal LVGL buffer allocation failed; trying PSRAM fallback");
        buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    ESP_LOGI(TAG, "LVGL buffer: %p bytes=%u", buf1, (unsigned)buf_bytes);
    assert(buf1);
    lv_disp_draw_buf_init(&s_draw_buf, buf1, NULL, (lv_coord_t)buf_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = BSP_LCD_H_RES;
    s_disp_drv.ver_res = BSP_LCD_V_RES;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.antialiasing = 1;
    s_disp_drv.user_data = s_panel;
    lv_disp_t *disp = lv_disp_drv_register(&s_disp_drv);
    assert(disp);

    if (s_touch) {
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type = LV_INDEV_TYPE_POINTER;
        s_indev_drv.read_cb = lvgl_touch_cb;
        s_indev_drv.user_data = s_touch;
        lv_indev_drv_register(&s_indev_drv);
        ESP_LOGI(TAG, "Touch ready on SDA=%d SCL=%d INT=%d", BSP_TOUCH_SDA_GPIO, BSP_TOUCH_SCL_GPIO, BSP_TOUCH_INT_GPIO);
    }

    bsp_backlight_set(100);
    ESP_LOGI(TAG, "LVGL ready");
}

