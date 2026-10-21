idf_component_register(
    SRCS
        "ui_app.c"
        "ui_screens.c"
    INCLUDE_DIRS
        "."
    REQUIRES lvgl audio_backend nvs_flash esp_app_format
)
