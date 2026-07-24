/**
 * @file gw_spi_driver.c
 * @brief ESP-IDF SPI master 的轻量封装实现。
 *
 * 当前实现以固定板级 SPI 引脚为基础，提供单字节、连续块和全双工传输能力，主要
 * 服务 CH37X 外设桥接。
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

#include "drivers/gw_spi_driver.h"

static const char *TAG = "gw_spi";

#define PIN_NUM_MISO 5
#define PIN_NUM_MOSI 6
#define PIN_NUM_CLK  7
#define PIN_NUM_CS   17

#define SPI_MUTEX_TIMEOUT_MS 1000
#define SPI_MAX_TRACKED_HOSTS 4

typedef struct spi {
    spi_device_handle_t handle;
    spi_host_device_t host;
    SemaphoreHandle_t mutex;
} spi_ctx_t;

static SemaphoreHandle_t s_bus_state_lock = NULL;
static uint8_t s_bus_ref_count[SPI_MAX_TRACKED_HOSTS] = {0};

static SemaphoreHandle_t ensure_bus_state_lock(void)
{
    if (s_bus_state_lock == NULL) {
        s_bus_state_lock = xSemaphoreCreateMutex();
    }
    return s_bus_state_lock;
}

static bool map_spi_id_to_host(int spi_id, spi_host_device_t *host)
{
    if (host == NULL) {
        return false;
    }

    if (spi_id == 2) {
        *host = SPI2_HOST;
        return true;
    }

#if (SOC_SPI_PERIPH_NUM >= 3)
    if (spi_id == 3) {
        *host = SPI3_HOST;
        return true;
    }
#endif

    if (spi_id == 1) {
        ESP_LOGW(TAG, "SPI1_HOST should not be used for user devices on ESP32-S3, fallback to SPI2_HOST");
        *host = SPI2_HOST;
        return true;
    }

    return false;
}

static int host_to_index(spi_host_device_t host)
{
    int idx = (int)host;
    if (idx < 0 || idx >= SPI_MAX_TRACKED_HOSTS) {
        return -1;
    }
    return idx;
}

static bool validate_ctx(const spi_ctx_t *ctx)
{
    return (ctx != NULL && ctx->handle != NULL && ctx->mutex != NULL);
}

gw_spi_t gw_spi_create(int spi_id)
{
    spi_host_device_t spi_host = SPI2_HOST;
    if (!map_spi_id_to_host(spi_id, &spi_host)) {
        ESP_LOGE(TAG, "Unsupported SPI id: %d", spi_id);
        return NULL;
    }

    int host_idx = host_to_index(spi_host);
    if (host_idx < 0) {
        ESP_LOGE(TAG, "Invalid SPI host index: %d", (int)spi_host);
        return NULL;
    }

    spi_ctx_t *ctx = (spi_ctx_t *)calloc(1, sizeof(spi_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate SPI context");
        return NULL;
    }

    ctx->host = spi_host;
    ctx->mutex = xSemaphoreCreateMutex();
    if (ctx->mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SPI mutex");
        free(ctx);
        return NULL;
    }

    SemaphoreHandle_t bus_lock = ensure_bus_state_lock();
    if (bus_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create SPI bus lock");
        vSemaphoreDelete(ctx->mutex);
        free(ctx);
        return NULL;
    }

    if (xSemaphoreTake(bus_lock, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire SPI bus lock");
        vSemaphoreDelete(ctx->mutex);
        free(ctx);
        return NULL;
    }

    bool bus_initialized_now = false;
    if (s_bus_ref_count[host_idx] == 0) {
        spi_bus_config_t buscfg = {
            .miso_io_num = PIN_NUM_MISO,
            .mosi_io_num = PIN_NUM_MOSI,
            .sclk_io_num = PIN_NUM_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 2048 + 8,
        };

        esp_err_t ret = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
        if (ret == ESP_OK) {
            bus_initialized_now = true;
        } else if (ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "spi_bus_initialize(%d) failed: %s", (int)spi_host, esp_err_to_name(ret));
            xSemaphoreGive(bus_lock);
            vSemaphoreDelete(ctx->mutex);
            free(ctx);
            return NULL;
        }
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 4,
        .pre_cb = NULL,
    };

    esp_err_t ret = spi_bus_add_device(spi_host, &devcfg, &ctx->handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device(%d) failed: %s", (int)spi_host, esp_err_to_name(ret));
        if (bus_initialized_now) {
            (void)spi_bus_free(spi_host);
        }
        xSemaphoreGive(bus_lock);
        vSemaphoreDelete(ctx->mutex);
        free(ctx);
        return NULL;
    }

    if (s_bus_ref_count[host_idx] < 0xFF) {
        s_bus_ref_count[host_idx]++;
    }

    xSemaphoreGive(bus_lock);

    ESP_LOGI(TAG, "SPI device created: host=%d, ref=%u", (int)spi_host, (unsigned)s_bus_ref_count[host_idx]);
    return ctx;
}

static int spi_transmit(spi_ctx_t *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!validate_ctx(ctx)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    spi_transaction_t t = {0};
    t.length = (uint32_t)(len * 8);
    t.tx_buffer = tx;
    if (rx != NULL) {
        t.rxlength = (uint32_t)(len * 8);
        t.rx_buffer = rx;
    }

    esp_err_t err = spi_device_transmit(ctx->handle, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_transmit failed: %s", esp_err_to_name(err));
        return -1;
    }

    return 0;
}

int gw_spi_write_byte(gw_spi_t ctx, uint8_t data)
{
    spi_ctx_t *self = (spi_ctx_t *)ctx;
    if (!validate_ctx(self)) {
        return -1;
    }

    if (xSemaphoreTake(self->mutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "SPI mutex timeout in gw_spi_write_byte");
        return -1;
    }

    spi_transaction_t t = {
        .length = 8,
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = {data},
    };

    esp_err_t err = spi_device_transmit(self->handle, &t);
    xSemaphoreGive(self->mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gw_spi_write_byte failed: %s", esp_err_to_name(err));
        return -1;
    }

    return 0;
}

int gw_spi_write(gw_spi_t ctx, const uint8_t *data, size_t data_len)
{
    spi_ctx_t *self = (spi_ctx_t *)ctx;
    if (!validate_ctx(self) || data == NULL || data_len == 0) {
        return -1;
    }

    if (xSemaphoreTake(self->mutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "SPI mutex timeout in gw_spi_write");
        return -1;
    }

    int ret = spi_transmit(self, data, NULL, data_len);
    xSemaphoreGive(self->mutex);
    return ret;
}

int gw_spi_read(gw_spi_t ctx, uint8_t *data, size_t data_len)
{
    spi_ctx_t *self = (spi_ctx_t *)ctx;
    if (!validate_ctx(self) || data == NULL || data_len == 0) {
        return -1;
    }

    if (xSemaphoreTake(self->mutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "SPI mutex timeout in gw_spi_read");
        return -1;
    }

    int ret = spi_transmit(self, NULL, data, data_len);
    xSemaphoreGive(self->mutex);

    if (ret < 0) {
        return -1;
    }

    return (int)data_len;
}

int gw_spi_trans_data(gw_spi_t ctx, uint8_t *data, size_t data_len)
{
    spi_ctx_t *self = (spi_ctx_t *)ctx;
    if (!validate_ctx(self) || data == NULL || data_len == 0) {
        return -1;
    }

    if (xSemaphoreTake(self->mutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "SPI mutex timeout in gw_spi_trans_data");
        return -1;
    }

    int ret = spi_transmit(self, data, data, data_len);
    xSemaphoreGive(self->mutex);
    return ret;
}

int gw_spi_trans_data1(gw_spi_t ctx, const uint8_t *data, uint8_t *rx_buffer, size_t data_len)
{
    spi_ctx_t *self = (spi_ctx_t *)ctx;
    if (!validate_ctx(self) || data == NULL || data_len == 0) {
        return -1;
    }

    if (xSemaphoreTake(self->mutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "SPI mutex timeout in gw_spi_trans_data1");
        return -1;
    }

    int ret = spi_transmit(self, data, rx_buffer, data_len);
    xSemaphoreGive(self->mutex);
    return ret;
}

void gw_spi_destroy(gw_spi_t ctx)
{
    spi_ctx_t *self = (spi_ctx_t *)ctx;
    if (!validate_ctx(self)) {
        return;
    }

    SemaphoreHandle_t bus_lock = ensure_bus_state_lock();
    if (bus_lock && xSemaphoreTake(bus_lock, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        int host_idx = host_to_index(self->host);

        esp_err_t ret = spi_bus_remove_device(self->handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "spi_bus_remove_device failed: %s", esp_err_to_name(ret));
        }

        if (host_idx >= 0 && s_bus_ref_count[host_idx] > 0) {
            s_bus_ref_count[host_idx]--;
            if (s_bus_ref_count[host_idx] == 0) {
                ret = spi_bus_free(self->host);
                if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "spi_bus_free(%d) failed: %s", (int)self->host, esp_err_to_name(ret));
                }
            }
        }

        xSemaphoreGive(bus_lock);
    } else {
        (void)spi_bus_remove_device(self->handle);
    }

    vSemaphoreDelete(self->mutex);
    free(self);

    ESP_LOGI(TAG, "SPI device destroyed");
}


