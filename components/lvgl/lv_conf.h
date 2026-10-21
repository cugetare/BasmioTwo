#ifndef LV_CONF_H
#define LV_CONF_H

/* Ensure LVGL can include this file from component include paths */
#define LV_CONF_INCLUDE_SIMPLE 1

/* Core rendering profile (ESP32-S3 + AXS15231B RGB565) */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1
#define LV_COLOR_SCREEN_TRANSP 0

/* Keep defaults close to current stable behavior */
#define LV_MEM_CUSTOM 1
#define LV_MEM_SIZE (64U * 1024U)
#define LV_MEM_BUF_MAX_NUM 16
#define LV_MEMCPY_MEMSET_STD 0

#define LV_DISP_DEF_REFR_PERIOD 16
#define LV_INDEV_DEF_READ_PERIOD 30
#define LV_DPI_DEF 200

#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4
#define LV_LAYER_SIMPLE_BUF_SIZE (24 * 1024)
#define LV_IMG_CACHE_DEF_SIZE 8
#define LV_GRADIENT_MAX_STOPS 2
#define LV_GRAD_CACHE_DEF_SIZE 0
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

/* File-system/image pipeline for SD cover art */
#define LV_FS_DEFAULT_DRIVER_LETTER 'S'
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'S'
#define LV_FS_STDIO_PATH ""
#define LV_FS_STDIO_CACHE_SIZE 4096

#define LV_USE_BMP 1
#define LV_USE_SJPG 1
#define LV_USE_PNG 1

/* Runtime overhead reducers */
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

/* Keep default theme available, but no extra transitions */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 0
#define LV_THEME_DEFAULT_GROW 0

#endif /* LV_CONF_H */
