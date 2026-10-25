idf_component_register(
    SRCS "bsp_display.c" "main.c" "perf_stats.c"
    INCLUDE_DIRS "."
    REQUIRES ui lvgl
    PRIV_REQUIRES esp_lcd esp_timer esp_app_format esp_lcd_axs15231b esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_ledc
    EMBED_FILES "assets/boot_ro_basmio.png" "assets/boot_en_basmio.png"
)
