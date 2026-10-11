idf_component_register(SRCS "main.c" "hermes.c" "wav.c" "ws2812.c" "lcd.c" "status.c" "text.c"
                            "font/font8x16.c" INCLUDE_DIRS "."
                       REQUIRES driver esp_lcd fatfs sdmmc spi_flash es8311
                                esp_wifi esp_event esp_netif nvs_flash mqtt json)
