idf_component_register(SRCS "main.c" "hermes.c" INCLUDE_DIRS "."
                       REQUIRES driver esp_lcd fatfs sdmmc spi_flash es8311
                                esp_wifi esp_event esp_netif nvs_flash mqtt)
