idf_component_register(
    SRCS "esp_lcd_axs15231b.c" "esp_lcd_touch.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_spi
)
