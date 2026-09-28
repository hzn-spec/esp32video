idf_component_register(SRCS "video_main.c" "oled.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver nvs_flash esp_wifi esp_netif esp_event lwip
                    PRIV_REQUIRES esp_timer)
