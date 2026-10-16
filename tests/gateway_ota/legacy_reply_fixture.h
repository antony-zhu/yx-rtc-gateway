/* Production legacy reply controller with only its non-OTA dependencies mocked. */
#define GW_PROTOCOL_ACCEPTED_PUBLISH_TIMEOUT_MS 5000U
typedef enum { GW_PROTOCOL_ROUTE_UNKNOWN, GW_PROTOCOL_ROUTE_RAW_DOWN } gw_protocol_route_t;
typedef enum { GW_REQUEST_CACHE_REGISTERED, GW_REQUEST_CACHE_DUPLICATE,
               GW_REQUEST_CACHE_FULL } gw_request_cache_register_result_t;
static bool s_initialized = true;
static unsigned accepted_wait_count, dispatch_count;
static int32_t last_reply_code;
static esp_err_t reply_build_error;
static char cached_request_id[GW_REQUEST_ID_MAX_LEN + 1U];

static void stats_inc_rejected(void) {}
static void stats_inc_duplicate(void) {}
static void stats_inc_handled(void) {}
static void stats_inc_publish_failed(void) {}
static cJSON *create_error_data(const char *reason)
{ (void)reason; return cJSON_CreateObject(); }
static cJSON *build_busy_data(void) { return cJSON_CreateObject(); }
static bool task_is_busy(void) { return false; }
static bool method_is_query(const char *method) { return strcmp(method, "status.get") == 0; }
static bool method_is_reserved(const char *method) { (void)method; return false; }
static bool method_is_async_accept(const char *method) { return strcmp(method, "ota.upgrade") == 0; }
static bool method_allowed_on_route(gw_protocol_route_t route, const char *method)
{ (void)route; (void)method; return true; }
static void maybe_sync_time_from_mqtt_timestamp(const cJSON *timestamp) { (void)timestamp; }
static void request_cache_mark_done(const char *id, int32_t code, const char *message)
{ (void)id; (void)code; (void)message; }
static gw_request_cache_register_result_t request_cache_register(const char *id, const char *method)
{
    (void)method;
    if (strcmp(id, cached_request_id) == 0) return GW_REQUEST_CACHE_DUPLICATE;
    strlcpy(cached_request_id, id, sizeof(cached_request_id));
    return GW_REQUEST_CACHE_REGISTERED;
}
static esp_err_t parse_topic_route(const char *topic, gw_protocol_route_t *route,
                                    char *reply, size_t capacity)
{
    if (strcmp(topic, "down") != 0) return ESP_ERR_NOT_FOUND;
    *route = GW_PROTOCOL_ROUTE_RAW_DOWN;
    strlcpy(reply, "up", capacity);
    return ESP_OK;
}
static esp_err_t parse_payload(const void *data, size_t size, cJSON **root)
{
    *root = cJSON_ParseWithLength(data, size);
    return *root == NULL ? ESP_ERR_INVALID_ARG : ESP_OK;
}
static esp_err_t build_reply_json(const char *method, const char *id, int32_t code,
                                  const char *message, cJSON *data,
                                  char *out, size_t capacity, size_t *length)
{
    (void)method; (void)id; (void)message;
    cJSON_Delete(data);
    last_reply_code = code;
    if (reply_build_error != ESP_OK) return reply_build_error;
    strlcpy(out, "{}", capacity);
    if (length != NULL) *length = 2U;
    return ESP_OK;
}
static esp_err_t build_small_error_reply(const char *method, const char *id, int32_t code,
                                         const char *message, char *out,
                                         size_t capacity, size_t *length)
{ return build_reply_json(method, id, code, message, NULL, out, capacity, length); }
static cJSON *dispatch_method(const char *method, const char *id, const cJSON *data,
                              gw_result_t *result)
{
    (void)data;
    ++dispatch_count;
    *result = gw_result_ok();
    if (strcmp(method, "ota.upgrade") == 0) {
        gw_ota_upgrade_request_t request = { .size = sizeof(payload) };
        strlcpy(request.request_id, id, sizeof(request.request_id));
        strcpy(request.target_version, "2.0.0");
        strcpy(request.url, "http://host/raw.bin");
        hash_request(&request, payload, sizeof(payload));
        (void)gw_ota_manager_submit_upgrade(&request, result);
    }
    return cJSON_CreateObject();
}
static void *gw_mqtt_payload_alloc(size_t capacity) { return malloc(capacity); }
static void gw_mqtt_payload_free(void *payload_buffer) { free(payload_buffer); }
static bool gw_config_manager_has_pending_update(void) { return false; }
static void gw_config_manager_cancel_pending_update(const char *reason) { (void)reason; }
static esp_err_t gw_config_manager_start_pending_update(void) { return ESP_ERR_NOT_FOUND; }
static esp_err_t gw_mqtt_manager_publish_wait(const char *topic, const char *data,
                                             size_t size, uint32_t timeout)
{
    assert(timeout == GW_PROTOCOL_ACCEPTED_PUBLISH_TIMEOUT_MS);
    ++accepted_wait_count;
    return gw_mqtt_manager_publish(topic, data, size);
}

#include "legacy_reply_under_test.h"

static const char *legacy_query = "{\"version\":\"1\",\"method\":\"status.get\",\"request_id\":\"same-owner\",\"data\":{}}";
static const char *legacy_upgrade = "{\"version\":\"1\",\"method\":\"ota.upgrade\",\"request_id\":\"same-owner\",\"data\":{}}";
static void reset_reply_fixture(void)
{
    reset_fixture();
    accepted_wait_count = dispatch_count = 0U;
    last_reply_code = -1;
    reply_build_error = ESP_OK;
    cached_request_id[0] = '\0';
}
static esp_err_t deliver_legacy(const char *message)
{ return gw_protocol_handle_down("down", message, strlen(message)); }
static void assert_pending_gateway_can_finish(gw_ota_upgrade_request_t *request)
{
    assert(gw_ota_manager_has_pending_upgrade() && gw_ota_manager_is_busy());
    assert(!((struct fake_queue *)s_ota_queue)->used && accepted_wait_count == 0U);
    assert(legacy_begin_count == 0U && legacy_end_count == 0U);
    assert(gw_ota_manager_start_pending_upgrade(true, request->request_id) == ESP_OK);
    run_worker_once();
    assert(boot_count == 1U && reboot_count == 1U);
    assert(cJSON_GetObjectItemCaseSensitive(last_event, "code")->valueint == 0);
}
static void test_legacy_reply_ownership(void)
{
    const char *messages[] = { legacy_query, legacy_query, "not JSON", legacy_query, legacy_upgrade };
    for (size_t i = 0U; i < sizeof(messages) / sizeof(messages[0]); ++i) {
        reset_reply_fixture();
        gw_ota_upgrade_request_t request = gateway_request();
        strcpy(request.request_id, "same-owner");
        gw_result_t result;
        assert(gw_ota_manager_submit_upgrade(&request, &result) == ESP_OK);
        if (i == 1U) legacy_reply_publish_error = ESP_ERR_INVALID_STATE;
        if (i == 3U) reply_build_error = ESP_ERR_NO_MEM;
        esp_err_t expected = i == 1U ? ESP_ERR_INVALID_STATE : i == 3U ? ESP_ERR_NO_MEM : ESP_OK;
        assert(deliver_legacy(messages[i]) == expected);
        if (i == 4U) assert(last_reply_code == GW_RESULT_DEVICE_BUSY);
        assert_pending_gateway_can_finish(&request);
    }

    reset_reply_fixture();
    assert(deliver_legacy(legacy_upgrade) == ESP_OK);
    assert(last_reply_code == GW_RESULT_OK && accepted_wait_count == 1U);
    assert(!gw_ota_manager_has_pending_upgrade() && ((struct fake_queue *)s_ota_queue)->used);
    assert(legacy_begin_count == 1U && legacy_end_count == 0U);
    assert(deliver_legacy(legacy_upgrade) == ESP_OK);
    assert(last_reply_code == GW_RESULT_DUPLICATE_REQUEST_ID);
    assert(dispatch_count == 1U && accepted_wait_count == 1U);
    response = payload; response_size = sizeof(payload); content_length = sizeof(payload);
    run_worker_once();
    assert(boot_count == 1U && legacy_end_count == 1U && reboot_count == 1U);

    reset_reply_fixture();
    legacy_reply_publish_error = ESP_ERR_INVALID_STATE;
    assert(deliver_legacy(legacy_upgrade) == ESP_ERR_INVALID_STATE);
    assert(accepted_wait_count == 1U && legacy_begin_count == 1U && legacy_end_count == 1U);
    assert(!gw_ota_manager_has_pending_upgrade() && !gw_ota_manager_is_busy());
    assert(!((struct fake_queue *)s_ota_queue)->used);
}
static void test_pending_request_ownership(void)
{
    reset_fixture();
    gw_ota_upgrade_request_t request = gateway_request();
    strcpy(request.request_id, "same-owner");
    gw_result_t result;
    assert(gw_ota_manager_submit_upgrade(&request, &result) == ESP_OK);
    assert(gw_ota_manager_start_pending_upgrade(false, request.request_id) == ESP_ERR_NOT_FOUND);
    assert(gw_ota_manager_start_pending_upgrade(true, "other-request") == ESP_ERR_NOT_FOUND);
    gw_ota_manager_cancel_pending_upgrade(false, request.request_id, "unrelated_reply_failed");
    gw_ota_manager_cancel_pending_upgrade(true, "other-request", "stale_reply_failed");
    assert(gw_ota_manager_has_pending_upgrade() && gw_ota_manager_is_busy());
    assert(!((struct fake_queue *)s_ota_queue)->used);
    gw_ota_manager_cancel_pending_upgrade(true, request.request_id, "owner_reply_failed");
    assert(!gw_ota_manager_has_pending_upgrade() && !gw_ota_manager_is_busy());

    gw_ota_upgrade_request_t legacy = { .size = sizeof(payload) };
    strcpy(legacy.request_id, "same-owner");
    strcpy(legacy.target_version, "2.0.0");
    strcpy(legacy.url, "http://host/raw.bin");
    hash_request(&legacy, payload, sizeof(payload));
    assert(gw_ota_manager_submit_upgrade(&legacy, &result) == ESP_OK);
    assert(gw_ota_manager_start_pending_upgrade(true, legacy.request_id) == ESP_ERR_NOT_FOUND);
    assert(gw_ota_manager_start_pending_upgrade(false, "other-request") == ESP_ERR_NOT_FOUND);
    gw_ota_manager_cancel_pending_upgrade(true, legacy.request_id, "gateway_reply_failed");
    gw_ota_manager_cancel_pending_upgrade(false, "other-request", "stale_reply_failed");
    assert(gw_ota_manager_has_pending_upgrade() && legacy_end_count == 0U);
    gw_ota_manager_cancel_pending_upgrade(false, legacy.request_id, "owner_reply_failed");
    assert(!gw_ota_manager_has_pending_upgrade() && !gw_ota_manager_is_busy() && legacy_end_count == 1U);
}
