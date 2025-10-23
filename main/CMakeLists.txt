idf_component_register(SRCS "app_streaming.c" "app_main.c" "app_uvc.c"
                    PRIV_REQUIRES
                        esp_psram    # Required for CONFIG_SPIRAM
                    )