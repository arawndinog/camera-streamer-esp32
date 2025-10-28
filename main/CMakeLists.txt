idf_component_register(
    SRCS 
        "app_wifi.c"
        "app_uvc.c"
       
        "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi_remote
        esp_wifi
        nvs_flash
    PRIV_REQUIRES
        esp_psram
)