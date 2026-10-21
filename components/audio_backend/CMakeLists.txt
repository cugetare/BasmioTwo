idf_component_register(
    SRCS "audio_backend.c" "audio_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_i2s esp_driver_gpio esp_driver_i2c esp_codec_dev esp_audio_codec json esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_timer
)
