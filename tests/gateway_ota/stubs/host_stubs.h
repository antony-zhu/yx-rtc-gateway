#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_err.h"
#include "gw_ota.h"
#include "gw_result.h"
#include "cJSON.h"

#define GW_CRYPTO_SHA256_SIZE 32U
#define GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC 10U
#define GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC 10U
#define GW_HTTP_DEFAULT_RETRY_COUNT 4U
#define GW_HTTP_MAX_URL_LEN 512U
#define GW_OTA_HTTP_BUF_BYTES 4096U
#define GW_OTA_PROGRESS_INTERVAL_SEC 1U
#define GW_TASK_STACK_OTA_BYTES 8192U
#define GW_REBOOT_DEFAULT_DELAY_MS 1000U
#define GW_REBOOT_REASON_OTA_UPDATED "ota_updated"
#define GW_REQUEST_ID_MAX_LEN 64U
#define GW_PROTOCOL_VERSION "1"
#define GW_PROTOCOL_TOPIC_MAX_LEN 256U
#define GW_MQTT_MAX_PAYLOAD_SIZE 4096U
#define GW_FIRMWARE_VERSION_MAJOR 1U
#define GW_FIRMWARE_VERSION_MINOR 0U
#define GW_FIRMWARE_VERSION_PATCH 0U
#define ESP_RETURN_ON_ERROR(expr, tag, message) do { esp_err_t e_ = (expr); if (e_ != ESP_OK) return e_; } while (0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0

typedef unsigned int TickType_t;
typedef int BaseType_t;
typedef int portMUX_TYPE;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
QueueHandle_t xQueueCreate(unsigned count, size_t item_size);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
int64_t esp_timer_get_time(void);
size_t host_strlcpy(char *destination, const char *source, size_t capacity);
#define strlcpy host_strlcpy

typedef enum { HTTP_ADDR_TYPE_INET, HTTP_ADDR_TYPE_INET6 } esp_http_client_addr_type_t;
#define HTTP_METHOD_GET 0
typedef void *esp_http_client_handle_t;
typedef struct {
    const char *url;
    int method;
    int timeout_ms;
    esp_http_client_addr_type_t addr_type;
    int buffer_size;
    int buffer_size_tx;
    esp_err_t (*crt_bundle_attach)(void *);
    bool keep_alive_enable;
} esp_http_client_config_t;
esp_err_t esp_crt_bundle_attach(void *config);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int capacity);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

typedef struct { uint32_t address; uint32_t size; int type; int subtype; } esp_partition_t;
typedef struct host_partition_iterator *esp_partition_iterator_t;
typedef uint32_t esp_ota_handle_t;
typedef enum { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
               ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED } esp_ota_img_states_t;
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_FACTORY 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_MIN 16
#define ESP_PARTITION_SUBTYPE_APP_OTA_MAX 31
#define ESP_PARTITION_SUBTYPE_ANY 255
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *from);
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t size, esp_ota_handle_t *handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_partition_iterator_t esp_partition_find(int type, int subtype, const char *label);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t it);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it);
void esp_partition_iterator_release(esp_partition_iterator_t it);
typedef struct { uint32_t offset; uint32_t size; } esp_partition_pos_t;
typedef struct { unsigned unused; } esp_image_metadata_t;
#define ESP_IMAGE_VERIFY_SILENT 0
esp_err_t esp_image_verify(int mode, const esp_partition_pos_t *partition, esp_image_metadata_t *metadata);

typedef enum { GW_DEVICE_STATUS_ONLINE, GW_DEVICE_STATUS_OTA_UPGRADING } gw_device_status_t;
typedef enum { GW_TASK_STATUS_IDLE, GW_TASK_STATUS_OTA_UPGRADING } gw_task_status_t;
typedef struct { gw_device_status_t device_status; } gw_state_snapshot_t;
esp_err_t gw_state_get_snapshot(gw_state_snapshot_t *out);
void gw_state_set_device_status(gw_device_status_t status);
void gw_state_set_task_status(gw_task_status_t status);
typedef struct { char group_id[65]; char device_id[65]; } gw_storage_identity_t;
esp_err_t gw_storage_get_identity(gw_storage_identity_t *out);
typedef struct { bool utc_trusted; uint64_t utc_ms; uint64_t uptime_ms; } gw_time_point_t;
gw_time_point_t gw_time_now(void);
esp_err_t gw_time_maybe_save_last_trusted(bool force);
typedef struct { int public_status; } gw_net_status_snapshot_t;
#define GW_NETWORK_STATUS_CONNECTED 1
esp_err_t gw_net_get_status(gw_net_status_snapshot_t *out);
#define GW_LOG_LEVEL_INFO 1
#define GW_LOG_LEVEL_WARN 2
void gw_log_write(int level, const char *module, const char *event, const char *message, const gw_result_t *result);
bool gw_http_url_scheme_allowed(const char *url);
esp_err_t gw_http_redact_url(const char *url, char *out, size_t capacity);
bool gw_crypto_sha256_hex_to_bytes(const char *hex, uint8_t *out);
esp_err_t gw_mqtt_manager_publish(const char *topic, const char *payload, size_t size);
esp_err_t gw_gateway_publish_event(const char *mt, const char *dt, const cJSON *pd, bool wait_for_attempt);
esp_err_t gw_protocol_task_begin(const char *method, const char *id, gw_task_status_t status);
void gw_protocol_task_end(const char *id, int32_t code, const char *message);
typedef struct {
    char request_id[65];
    char reason[32];
    uint32_t delay_ms;
    bool execute;
    bool gateway_protocol;
} gw_reboot_request_t;
esp_err_t gw_reboot_manager_request(const gw_reboot_request_t *request, gw_result_t *result);
