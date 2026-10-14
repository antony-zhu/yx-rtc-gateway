#include <assert.h>
#include <ctype.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define GW_OTA_MANAGER_ENABLE_TEST_HOOKS
#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#include "host_stubs.h"
#include "mbedtls/sha256.h"
#include "../../embedded/components/gw_app/src/ota/gw_ota.c"

static esp_partition_t partitions[] = {
    { .address = 0x30000U, .size = 1024U, .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_FACTORY },
    { .address = 0x330000U, .size = 1024U, .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_MIN },
    { .address = 0x630000U, .size = 1024U, .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 1 },
};
struct host_partition_iterator { size_t index; };
static struct host_partition_iterator partition_iterator;
static uint8_t payload[257];
static uint8_t container[297];
static const uint8_t *response;
static size_t response_size, response_offset, read_chunk;
static int64_t content_length, monotonic_us;
static bool http_complete, http_chunked;
static uint8_t written[2048];
static size_t written_size, begin_size;
static unsigned begin_count, end_count, abort_count, boot_count, verify_count;
static unsigned event_count, legacy_begin_count, legacy_end_count, reboot_count, confirm_count;
static esp_err_t begin_error, write_error, end_error, boot_error, verify_error, event_error;
static esp_ota_img_states_t old_state, running_state;
static bool old_state_exists;
static const esp_partition_t *selected_boot;
static cJSON *last_event;
static char last_mt[64];
static bool last_wait, reboot_gateway, reboot_after_event;
static void (*worker_entry)(void *);
static jmp_buf worker_return;
static bool stepping_worker;
struct fake_queue { size_t item_size; bool used; uint8_t item[sizeof(gw_ota_upgrade_request_t)]; };

size_t host_strlcpy(char *destination, const char *source, size_t capacity)
{
    size_t length = strlen(source);
    if (capacity > 0U) {
        size_t take = length < capacity - 1U ? length : capacity - 1U;
        memcpy(destination, source, take);
        destination[take] = '\0';
    }
    return length;
}
const char *esp_err_to_name(esp_err_t err) { (void)err; return "host_error"; }
QueueHandle_t xQueueCreate(unsigned count, size_t item_size)
{
    assert(count == 1U);
    struct fake_queue *queue = calloc(1U, sizeof(*queue));
    queue->item_size = item_size;
    return queue;
}
void vQueueDelete(QueueHandle_t queue) { free(queue); }
BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t wait)
{
    (void)wait;
    struct fake_queue *queue = handle;
    if (queue->used) return 0;
    memcpy(queue->item, item, queue->item_size);
    queue->used = true;
    return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t wait)
{
    (void)wait;
    struct fake_queue *queue = handle;
    if (!queue->used) {
        if (stepping_worker) longjmp(worker_return, 1);
        return 0;
    }
    memcpy(item, queue->item, queue->item_size);
    queue->used = false;
    return pdTRUE;
}
BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)arg; (void)priority;
    worker_entry = task;
    *out = (void *)1;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(TickType_t ticks) { monotonic_us += (int64_t)ticks * 1000; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(monotonic_us / 1000); }
int64_t esp_timer_get_time(void) { return monotonic_us; }
esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{ assert(config->url != NULL); response_offset = 0U; return (void *)1; }
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{ (void)client; (void)write_len; return ESP_OK; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client) { (void)client; return content_length; }
int esp_http_client_get_status_code(esp_http_client_handle_t client) { (void)client; return 200; }
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client) { (void)client; return http_chunked; }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout)
{ (void)client; (void)timeout; return ESP_OK; }
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int capacity)
{
    (void)client;
    size_t take = response_size - response_offset;
    if (take > read_chunk) take = read_chunk;
    if (take > (size_t)capacity) take = (size_t)capacity;
    memcpy(buffer, response + response_offset, take);
    response_offset += take;
    monotonic_us += 1000000;
    return (int)take;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) { (void)client; return http_complete; }
esp_err_t esp_http_client_close(esp_http_client_handle_t client) { (void)client; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) { (void)client; return ESP_OK; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *from) { (void)from; return &partitions[2]; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &partitions[1]; }
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t size, esp_ota_handle_t *handle)
{ assert(partition == &partitions[2]); ++begin_count; begin_size = size; *handle = 1U; return begin_error; }
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    assert(handle == 1U);
    if (write_error != ESP_OK) return write_error;
    assert(written_size + size <= sizeof(written));
    memcpy(written + written_size, data, size); written_size += size;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) { assert(handle == 1U); ++end_count; return end_error; }
esp_err_t esp_ota_abort(esp_ota_handle_t handle) { assert(handle == 1U); ++abort_count; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{ if (boot_error != ESP_OK) return boot_error; ++boot_count; selected_boot = partition; return ESP_OK; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state)
{
    if (partition == &partitions[1]) { *state = running_state; return ESP_OK; }
    assert(partition == &partitions[2]);
    if (!old_state_exists) return ESP_ERR_NOT_FOUND;
    *state = old_state; return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { ++confirm_count; running_state = ESP_OTA_IMG_VALID; return ESP_OK; }
esp_partition_iterator_t esp_partition_find(int type, int subtype, const char *label)
{ assert(type == ESP_PARTITION_TYPE_APP); (void)subtype; (void)label; partition_iterator.index = 0U; return &partition_iterator; }
const esp_partition_t *esp_partition_get(esp_partition_iterator_t it) { return &partitions[it->index]; }
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it)
{ return ++it->index < sizeof(partitions) / sizeof(partitions[0]) ? it : NULL; }
void esp_partition_iterator_release(esp_partition_iterator_t it) { (void)it; }
esp_err_t esp_image_verify(int mode, const esp_partition_pos_t *partition, esp_image_metadata_t *metadata)
{ (void)mode; (void)metadata; assert(partition->offset == partitions[2].address); ++verify_count; return verify_error; }
esp_err_t gw_state_get_snapshot(gw_state_snapshot_t *out) { out->device_status = GW_DEVICE_STATUS_ONLINE; return ESP_OK; }
void gw_state_set_device_status(gw_device_status_t status) { (void)status; }
void gw_state_set_task_status(gw_task_status_t status) { (void)status; }
esp_err_t gw_storage_get_identity(gw_storage_identity_t *out) { memset(out, 0, sizeof(*out)); return ESP_OK; }
gw_time_point_t gw_time_now(void) { return (gw_time_point_t){ .utc_trusted = true, .utc_ms = 1U }; }
esp_err_t gw_time_maybe_save_last_trusted(bool force) { (void)force; return ESP_OK; }
esp_err_t gw_net_get_status(gw_net_status_snapshot_t *out) { out->public_status = GW_NETWORK_STATUS_CONNECTED; return ESP_OK; }
void gw_log_write(int level, const char *module, const char *event, const char *message, const gw_result_t *result)
{ (void)level; (void)module; (void)event; (void)message; (void)result; }
bool gw_http_url_scheme_allowed(const char *url)
{ return strncmp(url, "http://", 7U) == 0 || strncmp(url, "https://", 8U) == 0; }
esp_err_t gw_http_redact_url(const char *url, char *out, size_t capacity)
{ return host_strlcpy(out, url, capacity) < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE; }
bool gw_crypto_sha256_hex_to_bytes(const char *hex, uint8_t *out)
{
    if (strlen(hex) != 64U) return false;
    for (size_t i = 0U; i < 32U; ++i) {
        unsigned value = 0U;
        for (size_t j = 0U; j < 2U; ++j) {
            char c = (char)tolower((unsigned char)hex[i * 2U + j]);
            if (!isxdigit((unsigned char)c)) return false;
            value = value * 16U + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = (uint8_t)value;
    }
    return true;
}
esp_err_t gw_mqtt_manager_publish(const char *topic, const char *data, size_t size)
{ (void)topic; (void)data; (void)size; return ESP_OK; }
esp_err_t gw_gateway_publish_event(const char *mt, const char *dt, const cJSON *pd, bool wait)
{
    assert(strcmp(dt, "plc") == 0);
    ++event_count; last_wait = wait;
    host_strlcpy(last_mt, mt, sizeof(last_mt));
    cJSON_Delete(last_event); last_event = cJSON_Duplicate(pd, true);
    assert(last_event != NULL);
    return event_error;
}
esp_err_t gw_protocol_task_begin(const char *method, const char *id, gw_task_status_t status)
{ (void)method; (void)status; assert(id[0] != '\0'); ++legacy_begin_count; return ESP_OK; }
void gw_protocol_task_end(const char *id, int32_t code, const char *message)
{ (void)id; (void)code; (void)message; ++legacy_end_count; }
esp_err_t gw_reboot_manager_request(const gw_reboot_request_t *request, gw_result_t *result)
{ ++reboot_count; reboot_gateway = request->gateway_protocol; reboot_after_event = event_count > 0U; *result = gw_result_ok(); return ESP_OK; }

static void reset_fixture(void)
{
    gw_ota_manager_test_reset();
    cJSON_Delete(last_event); last_event = NULL;
    response = container; response_size = sizeof(container); response_offset = 0U;
    content_length = sizeof(container); read_chunk = 17U; monotonic_us = 13000000;
    http_complete = true; http_chunked = false;
    written_size = 0U; begin_size = 0U;
    begin_count = end_count = abort_count = boot_count = verify_count = 0U;
    event_count = legacy_begin_count = legacy_end_count = reboot_count = confirm_count = 0U;
    begin_error = write_error = end_error = boot_error = verify_error = event_error = ESP_OK;
    old_state = running_state = ESP_OTA_IMG_VALID; old_state_exists = true;
    selected_boot = NULL; last_wait = false; reboot_gateway = false; reboot_after_event = false;
    stepping_worker = false;
    partitions[2].size = 1024U;
    payload[0] = 0xe9U;
    for (size_t i = 1U; i < sizeof(payload); ++i) payload[i] = (uint8_t)i;
    memcpy(container, "YXOT\x01\x00\x00\x00", 8U);
    assert(mbedtls_sha256(payload, sizeof(payload), container + 8U, 0) == 0);
    memcpy(container + 40U, payload, sizeof(payload));
    assert(gw_ota_manager_init() == ESP_OK);
}

static void hash_request(gw_ota_upgrade_request_t *request, const uint8_t *data, size_t size)
{
    uint8_t hash[32];
    assert(mbedtls_sha256(data, size, hash, 0) == 0);
    for (size_t i = 0U; i < sizeof(hash); ++i) snprintf(request->sha256 + i * 2U, 3U, "%02x", hash[i]);
}
static gw_ota_upgrade_request_t gateway_request(void)
{
    gw_ota_upgrade_request_t request = {
        .gateway_protocol = true, .gateway_version = "release-same-version-2026",
        .gateway_url = "https://host/fw.yxot", .size = sizeof(container),
        .gateway_progress_interval_sec = 0ULL,
    };
    hash_request(&request, container, sizeof(container));
    return request;
}
static void run_worker_once(void)
{
    stepping_worker = true;
    if (setjmp(worker_return) == 0) worker_entry(NULL);
    stepping_worker = false;
}

static void test_streaming_and_header_splits(void)
{
    const size_t chunks[] = { 1U, 7U, 39U, 40U, 41U, 297U, 4096U };
    for (size_t i = 0U; i < sizeof(chunks) / sizeof(chunks[0]); ++i) {
        reset_fixture(); read_chunk = chunks[i];
        gw_ota_upgrade_request_t request = gateway_request();
        uint32_t downloaded = 0U;
        gw_result_t result = gw_ota_manager_test_download_once(&request, &downloaded);
        assert(result.code == GW_RESULT_OK);
        assert(downloaded == sizeof(container));
        assert(begin_size == sizeof(payload));
        assert(written_size == sizeof(payload));
        assert(memcmp(written, payload, sizeof(payload)) == 0);
        assert(boot_count == 1U && selected_boot == &partitions[2]);
        assert(abort_count == 0U && event_count == 0U);
    }
}
static void assert_failed_download(gw_ota_upgrade_request_t *request)
{
    gw_result_t result = gw_ota_manager_test_download_once(request, NULL);
    assert(result.code != GW_RESULT_OK);
    assert(boot_count == 0U && selected_boot == NULL);
}
static void test_rejections_before_boot_switch(void)
{
    for (unsigned case_no = 0U; case_no < 15U; ++case_no) {
        reset_fixture();
        gw_ota_upgrade_request_t request = gateway_request();
        uint8_t extra[sizeof(container) + 1U];
        switch (case_no) {
        case 0: container[0] = 0xe9U; hash_request(&request, container, sizeof(container)); break;
        case 1: container[4] = 2U; hash_request(&request, container, sizeof(container)); break;
        case 2: container[7] = 1U; hash_request(&request, container, sizeof(container)); break;
        case 3: --response_size; break;
        case 4: memcpy(extra, container, sizeof(container)); extra[sizeof(container)] = 0U;
                response = extra; response_size = sizeof(extra); break;
        case 5: --content_length; break;
        case 6: content_length = 0; break;
        case 7: http_chunked = true; break;
        case 8: container[8] ^= 1U; hash_request(&request, container, sizeof(container)); break;
        case 9: request.sha256[0] = request.sha256[0] == '0' ? '1' : '0'; break;
        case 10: http_complete = false; break;
        case 11: write_error = ESP_FAIL; break;
        case 12: end_error = ESP_ERR_OTA_VALIDATE_FAILED; break;
        case 13: begin_error = ESP_ERR_NO_MEM; break;
        case 14: boot_error = ESP_FAIL; break;
        }
        assert_failed_download(&request);
    }
}
static void test_validation_busy_and_borrowed_strings(void)
{
    reset_fixture();
    gw_ota_upgrade_request_t request = gateway_request();
    gw_result_t result;
    request.gateway_version = "1.0.0";
    assert(validate_upgrade_request(&request, &result) == ESP_OK);
    memset(request.request_id, 'a', 31U); request.request_id[31] = '\0';
    assert(validate_upgrade_request(&request, &result) == ESP_OK);
    request.request_id[31] = 'a'; request.request_id[32] = '\0';
    assert(validate_upgrade_request(&request, &result) != ESP_OK);
    request.request_id[0] = '\0';
    request.gateway_progress_interval_sec = 9007199254740992ULL;
    assert(validate_upgrade_request(&request, &result) != ESP_OK);
    request.gateway_progress_interval_sec = 9007199254740991ULL;
    char version[] = "non-semver-version-that-is-longer-than-sixteen-bytes";
    char url[700]; memset(url, 'x', sizeof(url)); memcpy(url, "https://host/", 13U); url[sizeof(url)-1U] = '\0';
    request.gateway_version = version; request.gateway_url = url;
    assert(gw_ota_manager_submit_upgrade(&request, &result) == ESP_OK);
    assert(gw_ota_manager_is_busy() && gw_ota_manager_has_pending_upgrade());
    assert(legacy_begin_count == 0U);
    assert(strcmp(s_pending_upgrade.gateway_version, version) == 0);
    assert(strcmp(s_pending_upgrade.gateway_url, url) == 0);
    version[0] = 'X'; url[15] = 'Y';
    assert(s_pending_upgrade.gateway_version[0] == 'n' && s_pending_upgrade.gateway_url[15] == 'x');
    assert(gw_ota_manager_prepare_rollback(&result) != ESP_OK && result.code == GW_RESULT_DEVICE_BUSY);
    gw_ota_upgrade_request_t legacy = { .size = sizeof(payload), .progress_interval_sec = 1U };
    strcpy(legacy.request_id, "old"); strcpy(legacy.target_version, "2.0.0"); strcpy(legacy.url, "http://host/raw.bin");
    hash_request(&legacy, payload, sizeof(payload));
    assert(gw_ota_manager_submit_upgrade(&legacy, &result) != ESP_OK && result.code == GW_RESULT_DEVICE_BUSY);
    gw_ota_manager_cancel_pending_upgrade("test_cancel");
    assert(!gw_ota_manager_is_busy() && legacy_end_count == 0U);
    assert(gw_ota_manager_submit_upgrade(&legacy, &result) == ESP_OK);
    assert(legacy_begin_count == 1U);
    assert(gw_ota_manager_submit_upgrade(&request, &result) != ESP_OK && result.code == GW_RESULT_DEVICE_BUSY);
    gw_ota_manager_cancel_pending_upgrade("test_cancel");
    assert(!gw_ota_manager_is_busy() && legacy_end_count == 1U);
    request = gateway_request();
    partitions[2].size = sizeof(payload);
    assert(validate_upgrade_request(&request, &result) == ESP_OK);
    --partitions[2].size;
    assert(validate_upgrade_request(&request, &result) == ESP_ERR_INVALID_SIZE);
    partitions[2].size = 1024U;
    request.size = 40U;
    assert(validate_upgrade_request(&request, &result) != ESP_OK);
}
static void test_result_attempt_before_reboot_and_progress_disable(void)
{
    reset_fixture();
    gw_ota_upgrade_request_t request = gateway_request();
    gw_result_t result;
    event_error = ESP_ERR_INVALID_STATE;
    assert(gw_ota_manager_submit_upgrade(&request, &result) == ESP_OK);
    assert(gw_ota_manager_start_pending_upgrade() == ESP_OK);
    run_worker_once();
    assert(event_count == 1U && strcmp(last_mt, "gateway.ota.result") == 0 && last_wait);
    assert(cJSON_GetObjectItemCaseSensitive(last_event, "code")->valueint == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(last_event, "message")->valuestring, "OTA completed") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(last_event, "request_id")->valuestring, "") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(last_event, "version")->valuestring, request.gateway_version) == 0);
    assert(legacy_begin_count == 0U && legacy_end_count == 0U);
    assert(reboot_count == 1U && reboot_gateway && reboot_after_event);
    reset_fixture(); request = gateway_request(); request.sha256[0] = request.sha256[0] == '0' ? '1' : '0';
    assert(gw_ota_manager_submit_upgrade(&request, &result) == ESP_OK);
    assert(gw_ota_manager_start_pending_upgrade() == ESP_OK);
    run_worker_once();
    assert(event_count == 1U && last_wait && strcmp(last_mt, "gateway.ota.result") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(last_event, "code")->valueint == 11);
    assert(boot_count == 0U && reboot_count == 0U && !gw_ota_manager_is_busy());
    reset_fixture(); request = gateway_request(); request.gateway_progress_interval_sec = 2ULL;
    assert(gw_ota_manager_test_download_once(&request, NULL).code == GW_RESULT_OK);
    assert(event_count > 0U && !last_wait && strcmp(last_mt, "gateway.ota.progress") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(last_event, "total")->valueint == (int)sizeof(container));
    assert(cJSON_GetObjectItemCaseSensitive(last_event, "received")->valueint <= (int)sizeof(container));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(last_event, "request_id")->valuestring, "") == 0);
}
static void test_legacy_raw_image(void)
{
    reset_fixture(); response = payload; response_size = sizeof(payload); content_length = sizeof(payload);
    gw_ota_upgrade_request_t request = { .size = sizeof(payload), .progress_interval_sec = 1U };
    strcpy(request.request_id, "legacy"); strcpy(request.target_version, "2.0.0"); strcpy(request.url, "http://host/raw.bin");
    hash_request(&request, payload, sizeof(payload));
    assert(gw_ota_manager_test_download_once(&request, NULL).code == GW_RESULT_OK);
    assert(begin_size == sizeof(payload) && written_size == sizeof(payload));
    assert(memcmp(written, payload, sizeof(payload)) == 0 && boot_count == 1U);
}
static void test_rollback_only_verified_old_ota(void)
{
    const esp_ota_img_states_t rejected[] = { ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED,
                                            ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY };
    gw_result_t result;
    for (size_t i = 0U; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        reset_fixture(); old_state = rejected[i];
        assert(gw_ota_manager_prepare_rollback(&result) != ESP_OK);
        assert(verify_count == 0U && boot_count == 0U && reboot_count == 0U && !gw_ota_manager_is_busy());
    }
    reset_fixture(); old_state_exists = false;
    assert(gw_ota_manager_prepare_rollback(&result) != ESP_OK && boot_count == 0U);
    reset_fixture(); verify_error = ESP_ERR_OTA_VALIDATE_FAILED;
    assert(gw_ota_manager_prepare_rollback(&result) != ESP_OK && boot_count == 0U && !gw_ota_manager_is_busy());
    reset_fixture(); boot_error = ESP_FAIL;
    assert(gw_ota_manager_prepare_rollback(&result) != ESP_OK && boot_count == 0U && reboot_count == 0U);
    reset_fixture(); old_state = ESP_OTA_IMG_UNDEFINED;
    assert(gw_ota_manager_prepare_rollback(&result) == ESP_OK);
    assert(verify_count == 1U && selected_boot == &partitions[2] && gw_ota_manager_is_busy());
    assert(event_count == 0U && reboot_count == 0U);
    assert(gw_ota_manager_prepare_rollback(&result) != ESP_OK && result.code == GW_RESULT_DEVICE_BUSY);
    assert(gw_ota_manager_reboot_prepared_rollback() == ESP_OK && reboot_gateway && reboot_count == 1U);
    reset_fixture(); gw_ota_manager_test_reset();
    assert(gw_ota_manager_prepare_rollback(&result) != ESP_OK && strcmp(result.reason_code, GW_REASON_NOT_INITIALIZED) == 0);
    assert(gw_ota_manager_reboot_prepared_rollback() == ESP_ERR_INVALID_STATE);
}
static void test_conditional_confirmation_at_boot_uptime(void)
{
    reset_fixture(); running_state = ESP_OTA_IMG_PENDING_VERIFY; monotonic_us = 3000000;
    run_worker_once();
    assert(confirm_count == 1U && monotonic_us == 12000000);
    reset_fixture(); running_state = ESP_OTA_IMG_VALID; monotonic_us = 3000000;
    run_worker_once();
    assert(confirm_count == 0U && monotonic_us == 3000000);
}
int main(void)
{
    test_streaming_and_header_splits();
    test_rejections_before_boot_switch();
    test_validation_busy_and_borrowed_strings();
    test_result_attempt_before_reboot_and_progress_disable();
    test_legacy_raw_image();
    test_rollback_only_verified_old_ota();
    test_conditional_confirmation_at_boot_uptime();
    gw_ota_manager_test_reset(); cJSON_Delete(last_event);
    puts("Gateway OTA host tests passed (streaming, validation, events, legacy raw, rollback, confirmation)");
    return 0;
}
