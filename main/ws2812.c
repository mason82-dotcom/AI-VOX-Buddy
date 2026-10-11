#include "ws2812.h"

#include "board.h"
#include "driver/spi_master.h"

static spi_device_handle_t dev;

esp_err_t ws2812_init(void)
{
    if (dev) return ESP_OK;
    spi_bus_config_t bus = {.mosi_io_num = AIVOX3_RGB, .miso_io_num = -1, .sclk_io_num = -1,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 9};
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) return err;
    spi_device_interface_config_t dc = {.clock_speed_hz = 2500000, .mode = 0,
                                        .spics_io_num = -1, .queue_size = 1};
    if ((err = spi_bus_add_device(SPI2_HOST, &dc, &dev)) != ESP_OK) {
        spi_bus_free(SPI2_HOST);
        dev = NULL;
    }
    return err;
}

/* Encode WS2812 bits as 100 (zero) and 110 (one), clocked at 2.5 MHz. */
esp_err_t ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!dev) return ESP_ERR_INVALID_STATE;
    const uint8_t grb[3] = {g, r, b};
    uint8_t encoded[9] = {0};
    unsigned p = 0;
    for (unsigned i = 0; i < 3; ++i) for (int bit = 7; bit >= 0; --bit) {
        unsigned symbol = grb[i] & (1U << bit) ? 6 : 4;
        for (int s = 2; s >= 0; --s, ++p)
            encoded[p / 8] |= ((symbol >> s) & 1U) << (7 - p % 8);
    }
    spi_transaction_t t = {.length = 72, .tx_buffer = encoded};
    return spi_device_transmit(dev, &t);
}
