#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the implementation to advance its worker deterministically. */
#include "../../embedded/components/gw_app/src/time/gw_gateway_timer.c"
#include "gw_crypto.h"
#include "gw_ymzl_protocol.h"

static gw_time_point_t test_clock;
static int16_t test_timezone;
static char *persisted;
static bool fail_save;
static unsigned save_count;
static unsigned send_count;
static unsigned fail_send_at;
static uint8_t frames[64][GW_GS200_ACTION_FRAME_LEN];
static unsigned event_count;
static cJSON *last_event;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{ (void)semaphore; (void)ticks; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{ (void)semaphore; return pdTRUE; }
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)entry; (void)name; (void)stack; (void)arg; (void)priority;
    *handle = (void *)1;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; }
gw_time_point_t gw_time_now(void) { return test_clock; }
esp_err_t gw_scheduler_get_status(gw_scheduler_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->timezone_offset_min = test_timezone;
    return ESP_OK;
}
esp_err_t gw_storage_secure_set_blob(const char *ns, const char *key,
                                     gw_storage_value_type_t type,
                                     const void *data, size_t length)
{
    assert(strcmp(ns, "gateway_timer") == 0);
    assert(strcmp(key, "table") == 0);
    assert(type == GW_STORAGE_VALUE_JSON);
    ++save_count;
    if (fail_save) {
        return ESP_FAIL;
    }
    char *replacement = malloc(length);
    assert(replacement != NULL);
    memcpy(replacement, data, length);
    free(persisted);
    persisted = replacement;
    return ESP_OK;
}
esp_err_t gw_storage_secure_get_blob(const char *ns, const char *key,
                                     gw_storage_value_type_t type, void *out,
                                     size_t capacity, size_t *length)
{
    (void)ns; (void)key; (void)type;
    if (persisted == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t needed = strlen(persisted) + 1;
    if (needed > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, persisted, needed);
    *length = needed;
    return ESP_OK;
}
esp_err_t gw_dongle_mqtt_bridge_send_frame(const uint8_t *frame,
                                           size_t length, uint32_t timeout)
{
    (void)timeout;
    assert(length == GW_GS200_ACTION_FRAME_LEN);
    assert(gw_ymzl_frame_validate(frame, length) == ESP_OK);
    assert(send_count < 64);
    memcpy(frames[send_count++], frame, length);
    return fail_send_at == send_count ? ESP_FAIL : ESP_OK;
}
esp_err_t gw_gateway_publish_event(const char *mt, const char *dt,
                                   const cJSON *pd, bool wait)
{
    assert(strcmp(mt, "gateway.timer.fired") == 0);
    assert(strcmp(dt, "gateway") == 0);
    assert(!wait);
    ++event_count;
    cJSON_Delete(last_event);
    last_event = cJSON_Duplicate(pd, true);
    assert(last_event != NULL);
    return ESP_OK;
}
uint16_t gw_crypto_crc16_modbus(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1) ? 0xA001 : 0));
        }
    }
    return crc;
}

static void reboot(void)
{
    free_table(&s_table);
    s_initialized = false;
    s_clock_seen = false;
    s_time_paused = false;
    s_worker = NULL;
    memset(&s_previous_clock, 0, sizeof(s_previous_clock));
    test_clock.uptime_ms = 0;
    assert(gw_gateway_timer_init() == ESP_OK);
}

static void fresh(void)
{
    free(persisted);
    persisted = NULL;
    cJSON_Delete(last_event);
    last_event = NULL;
    fail_save = false;
    send_count = 0;
    fail_send_at = 0;
    save_count = 0;
    event_count = 0;
    test_timezone = 0;
    test_clock = (gw_time_point_t){.utc_ms = 1784592000000ULL,
                                  .utc_trusted = true};
    reboot();
}

static void elapse(uint64_t milliseconds)
{
    test_clock.uptime_ms += milliseconds;
    if (test_clock.utc_trusted) {
        test_clock.utc_ms += milliseconds;
    }
    timer_poll();
}

static cJSON *request(const char *operation, const char *json, int expected)
{
    char mt[64];
    snprintf(mt, sizeof(mt), "gateway.timer.%s", operation);
    cJSON *pd = cJSON_Parse(json);
    assert(pd != NULL);
    cJSON *response = gw_gateway_timer_handle(mt, pd);
    cJSON_Delete(pd);
    assert(response != NULL);
    if (field(response, "code")->valueint != expected) {
        char *printed = cJSON_PrintUnformatted(response);
        fprintf(stderr, "%s: expected %d, got %s\n", operation, expected, printed);
        cJSON_free(printed);
        abort();
    }
    return response;
}

#define CONTROL_ON "{\"type\":\"light_on\",\"target\":{\"area_addr\":124,\"group_addr\":1,\"unicast_addr\":1}}"
#define CONTROL_OFF "{\"type\":\"light_off\",\"target\":{\"area_addr\":124,\"group_addr\":1,\"unicast_addr\":1}}"
#define CONFIG(enabled, name, type, actions) \
    "{\"enabled\":" enabled ",\"name\":\"" name "\",\"type\":\"" type "\",\"schedule\":{\"value\":1,\"unit\":\"minute\"},\"action\":[" actions "]}"

static void batch_atomicity(void)
{
    fresh();
    cJSON *response = request("upsert", "{\"task\":["
        CONFIG("false", "one", "duration", CONTROL_ON) ","
        CONFIG("false", "two", "duration", CONTROL_OFF) "]}", 0);
    const cJSON *tasks = field(response, "task");
    assert(cJSON_GetArraySize(tasks) == 2);
    assert(string_is(field(cJSON_GetArrayItem(tasks, 0), "task_id"), "1"));
    assert(string_is(field(cJSON_GetArrayItem(tasks, 1), "task_id"), "2"));
    cJSON_Delete(response);
    char *before = strdup(persisted);
    unsigned saves = save_count;
    response = request("upsert", "{\"task\":["
        "{\"task_id\":\"1\",\"revision\":1,\"enabled\":false,\"name\":\"changed\",\"type\":\"duration\",\"schedule\":{\"value\":2,\"unit\":\"minute\"},\"action\":[" CONTROL_ON "]},"
        "{\"task_id\":\"2\",\"revision\":2,\"enabled\":false,\"name\":\"two\",\"type\":\"duration\",\"schedule\":{\"value\":1,\"unit\":\"minute\"},\"action\":[" CONTROL_OFF "]}]}", 5);
    cJSON_Delete(response);
    assert(save_count == saves && strcmp(before, persisted) == 0);
    assert(s_table.revision == 1 && task_revision(&s_table.tasks[0]) == 1);
    fail_save = true;
    response = request("enable", "{\"task_id\":\"1\",\"revision\":1,\"enabled\":true}", 14);
    cJSON_Delete(response);
    assert(!task_enabled(&s_table.tasks[0]) && !s_table.tasks[0].due_uptime_valid);
    assert(strcmp(before, persisted) == 0);
    fail_save = false;
    free(before);
    response = request("list", "{\"cursor\":0,\"limit\":1}", 0);
    const cJSON *summary = cJSON_GetArrayItem(field(response, "items"), 0);
    assert(cJSON_GetArraySize(summary) == 5);
    assert(field(summary, "action") == NULL && field(summary, "schedule") == NULL);
    assert(field(response, "next_cursor")->valueint == 1);
    assert(cJSON_IsFalse(field(response, "done")));
    cJSON_Delete(response);
    response = request("enable", "{\"task_id\":\"1\",\"revision\":2,\"enabled\":true}", 5);
    cJSON_Delete(response);
}

static void actions_delays_and_busy(void)
{
    fresh();
    cJSON *response = request("upsert", "{\"task\":["
        CONFIG("true", "one", "duration", CONTROL_ON ","
            "{\"type\":\"delay\",\"duration_ms\":500}," CONTROL_OFF) ","
        CONFIG("true", "two", "duration", CONTROL_OFF) "]}", 0);
    cJSON_Delete(response);
    elapse(60000);
    assert(send_count == 2 && event_count == 1);
    assert(s_table.tasks[0].active && s_table.tasks[0].delaying);
    assert(s_table.tasks[0].delay_until_ms == 60500);
    response = request("enable", "{\"task_id\":\"1\",\"revision\":1,\"enabled\":false}", 8);
    cJSON_Delete(response);
    elapse(499);
    assert(send_count == 2);
    elapse(1);
    assert(send_count == 3 && event_count == 2);
    assert(!task_enabled(&s_table.tasks[0]) && task_revision(&s_table.tasks[0]) == 2);
    assert(cJSON_IsTrue(field(last_event, "success")));
    assert((uint64_t)field(last_event, "scheduled_utc_ms")->valuedouble == 1784592060000ULL);
}

static void address_conversion_and_failure(void)
{
    fresh();
    cJSON *response = request("upsert", "{\"task\":["
        CONFIG("true", "targets", "duration", CONTROL_ON ","
            "{\"type\":\"delay\",\"duration_ms\":0},"
            "{\"type\":\"scene_switch\",\"scene_id\":16,\"target\":{\"area_addr\":65535,\"group_addr\":16382,\"unicast_addr\":0}},"
            "{\"type\":\"light_off\",\"target\":{\"area_addr\":0,\"group_addr\":0,\"unicast_addr\":0}}") "]}", 0);
    cJSON_Delete(response);
    elapse(60000);
    assert(send_count == 3 && event_count == 1);
    gw_gs200_action_t parsed;
    assert(gw_gs200_parse_action_frame(frames[0], 24, &parsed) == ESP_OK);
    assert(parsed.device_addr == 1 && parsed.network_id == 124);
    assert(gw_gs200_parse_action_frame(frames[1], 24, &parsed) == ESP_OK);
    assert(parsed.device_addr == 0xFFFE && parsed.network_id == 65535 && parsed.scene_no == 16);
    assert(gw_gs200_parse_action_frame(frames[2], 24, &parsed) == ESP_OK);
    assert(parsed.device_addr == 0xFFFF && parsed.network_id == 0);
    fresh();
    response = request("upsert", "{\"task\":["
        CONFIG("true", "failure", "duration", CONTROL_ON "," CONTROL_OFF "," CONTROL_ON) "]}", 0);
    cJSON_Delete(response);
    fail_send_at = 2;
    elapse(60000);
    assert(send_count == 2 && event_count == 1);
    assert(cJSON_IsFalse(field(last_event, "success")));
    assert(field(last_event, "esp_err")->valueint == ESP_FAIL);
    assert(!task_enabled(&s_table.tasks[0]));
}

static void claim_persistence_and_restart(void)
{
    fresh();
    cJSON *response = request("upsert", "{\"task\":["
        CONFIG("true", "claim", "duration", CONTROL_ON ","
            "{\"type\":\"delay\",\"duration_ms\":500}," CONTROL_OFF) "]}", 0);
    cJSON_Delete(response);
    char *before = strdup(persisted);
    fail_save = true;
    elapse(60000);
    assert(send_count == 0 && s_table.tasks[0].occurrence_id == 0);
    assert(!s_table.tasks[0].active && strcmp(before, persisted) == 0);
    fail_save = false;
    elapse(1);
    assert(send_count == 1 && s_table.tasks[0].occurrence_id == 1);
    assert(s_table.tasks[0].active);
    reboot();
    elapse(1000);
    assert(send_count == 1 && event_count == 0);
    assert(!task_enabled(&s_table.tasks[0]));
    free(before);
    fresh();
    response = request("upsert", "{\"task\":["
        CONFIG("true", "missed", "duration", CONTROL_ON) ","
        CONFIG("true", "interval", "interval", CONTROL_OFF) "]}", 0);
    cJSON_Delete(response);
    test_clock.utc_ms += 180001;
    reboot();
    assert(!task_enabled(&s_table.tasks[0]));
    assert(task_enabled(&s_table.tasks[1]));
    elapse(0);
    assert(send_count == 0);
    elapse(59999);
    assert(send_count == 1 && event_count == 1);
}

static void interval_no_overlap(void)
{
    fresh();
    cJSON *response = request("upsert", "{\"task\":["
        CONFIG("true", "interval", "interval", CONTROL_ON ","
            "{\"type\":\"delay\",\"duration_ms\":120000}," CONTROL_OFF) "]}", 0);
    cJSON_Delete(response);
    elapse(60000);
    assert(send_count == 1);
    elapse(60000);
    assert(send_count == 1 && event_count == 0);
    elapse(60000);
    assert(send_count == 2 && event_count == 1);
    elapse(1);
    assert(send_count == 2);
    elapse(59999);
    assert(send_count == 3 && s_table.tasks[0].occurrence_id == 2);
}

static void time_rules_and_daily_rollback(void)
{
    fresh();
    test_clock.utc_trusted = false;
    test_clock.utc_ms = 0;
    cJSON *response = request("upsert", "{\"task\":["
        CONFIG("true", "invalid", "duration", CONTROL_ON) "]}", 7);
    cJSON_Delete(response);
    response = request("upsert", "{\"task\":["
        CONFIG("false", "disabled", "duration", CONTROL_ON) "]}", 0);
    cJSON_Delete(response);
    response = request("enable", "{\"task_id\":\"1\",\"revision\":1,\"enabled\":true}", 7);
    cJSON_Delete(response);
    fresh();
    response = request("upsert", "{\"task\":[{\"enabled\":true,\"name\":\"daily\",\"type\":\"daily\",\"schedule\":{\"hour\":0,\"minute\":1},\"action\":[" CONTROL_ON "]}]}", 0);
    cJSON_Delete(response);
    elapse(60000);
    assert(send_count == 1);
    gw_gateway_timer_time_change_begin();
    test_clock.utc_ms -= 60000;
    gw_gateway_timer_time_change_finish(true);
    elapse(60000);
    assert(send_count == 1);
    gw_gateway_timer_time_change_begin();
    test_clock.utc_trusted = false;
    test_clock.utc_ms = 0;
    gw_gateway_timer_time_change_finish(true);
    response = request("upsert", "{\"task\":[{\"task_id\":\"1\",\"revision\":1,\"enabled\":true,\"name\":\"edited\",\"type\":\"daily\",\"schedule\":{\"hour\":0,\"minute\":1},\"action\":[" CONTROL_OFF "]}]}", 0);
    cJSON_Delete(response);
    gw_gateway_timer_time_change_begin();
    test_clock.utc_trusted = true;
    test_clock.utc_ms = 1784592000000ULL + 2 * TIMER_DAY_MS + 120000;
    gw_gateway_timer_time_change_finish(true);
    elapse(0);
    assert(send_count == 1);
    elapse(TIMER_DAY_MS - 60000);
    assert(send_count == 2);
}

static void small_correction_restart_projection(void)
{
    for (int direction = -1; direction <= 1; direction += 2) {
        fresh();
        uint64_t base_utc_ms = test_clock.utc_ms;
        cJSON *response = request("upsert", "{\"task\":["
            CONFIG("true", "duration", "duration", CONTROL_ON) ","
            CONFIG("true", "interval", "interval", CONTROL_OFF) "]}", 0);
        cJSON_Delete(response);
        elapse(20000);
        gw_gateway_timer_time_change_begin();
        test_clock.utc_ms = (uint64_t)((int64_t)test_clock.utc_ms + direction * 1000);
        gw_gateway_timer_time_change_finish(false);
        elapse(0);
        assert(send_count == 0);
        assert(s_table.tasks[0].due_uptime_ms == 60000);
        assert(s_table.tasks[1].due_uptime_ms == 60000);
        reboot();
        elapse(39999);
        assert(send_count == 0);
        elapse(1);
        assert(send_count == 2 && event_count == 2);
        assert(!task_enabled(&s_table.tasks[0]));
        uint64_t scheduled = (uint64_t)((int64_t)base_utc_ms + 60000 + direction * 1000);
        assert((uint64_t)field(last_event, "scheduled_utc_ms")->valuedouble == scheduled);
    }
}

static void small_correction_crosses_deadline(void)
{
    for (int direction = -1; direction <= 1; direction += 2) {
        fresh();
        uint64_t base_utc_ms = test_clock.utc_ms;
        cJSON *response = request("upsert", "{\"task\":["
            CONFIG("true", "duration", "duration", CONTROL_ON) ","
            CONFIG("true", "interval", "interval", CONTROL_OFF) ","
            "{\"enabled\":true,\"name\":\"daily\",\"type\":\"daily\",\"schedule\":{\"hour\":0,\"minute\":1},\"action\":[" CONTROL_ON "]}]}", 0);
        cJSON_Delete(response);
        elapse(59990);
        gw_gateway_timer_time_change_begin();
        elapse(20);
        test_clock.utc_ms = (uint64_t)((int64_t)test_clock.utc_ms + direction * 1000);
        elapse(0);
        assert(send_count == 0 && event_count == 0);
        gw_gateway_timer_time_change_finish(false);
        fail_save = true;
        elapse(0);
        assert(send_count == 0 && event_count == 0);
        fail_save = false;
        elapse(0);
        assert(send_count == (direction > 0 ? 3U : 2U));
        assert(!task_enabled(&s_table.tasks[0]));
        assert(s_table.tasks[1].occurrence_id == 1);
        assert(s_table.tasks[1].due_uptime_ms == 120000);
        uint64_t scheduled = (uint64_t)((int64_t)base_utc_ms + 60000 + direction * 1000);
        assert(s_table.tasks[0].scheduled_utc_ms == scheduled);
        assert(s_table.tasks[1].scheduled_utc_ms == scheduled);
        if (direction < 0) {
            elapse(989);
            assert(send_count == 2);
            elapse(1);
        }
        assert(send_count == 3 && event_count == 3);
        assert((uint64_t)field(last_event, "scheduled_utc_ms")->valuedouble == base_utc_ms + 60000);
        gw_gateway_timer_time_change_begin();
        test_clock.utc_ms -= 2000;
        gw_gateway_timer_time_change_finish(false);
        elapse(2000);
        assert(send_count == 3);
        elapse(119999 - test_clock.uptime_ms);
        assert(send_count == 3);
        elapse(1);
        assert(send_count == 4 && event_count == 4);
        assert(s_table.tasks[1].occurrence_id == 2);
    }
}

static void rebase_skips_missed_occurrences(void)
{
    for (int restore_trust = 0; restore_trust <= 1; ++restore_trust) {
        fresh();
        uint64_t base_utc_ms = test_clock.utc_ms;
        cJSON *response = request("upsert", "{\"task\":["
            CONFIG("true", "duration", "duration", CONTROL_ON) ","
            CONFIG("true", "interval", "interval", CONTROL_OFF) ","
            "{\"enabled\":true,\"name\":\"daily\",\"type\":\"daily\",\"schedule\":{\"hour\":0,\"minute\":1},\"action\":[" CONTROL_ON "]}]}", 0);
        cJSON_Delete(response);
        elapse(59990);
        gw_gateway_timer_time_change_begin();
        if (restore_trust) {
            test_clock.utc_trusted = false;
            test_clock.utc_ms = 0;
            gw_gateway_timer_time_change_finish(true);
            elapse(20);
            assert(send_count == 0);
            gw_gateway_timer_time_change_begin();
            test_clock.utc_trusted = true;
            test_clock.utc_ms = base_utc_ms + 660010;
        } else {
            elapse(20);
            test_clock.utc_ms += 600000;
        }
        gw_gateway_timer_time_change_finish(true);
        elapse(0);
        assert(send_count == 0 && event_count == 0);
        assert(!task_enabled(&s_table.tasks[0]));
        assert(s_table.tasks[1].due_uptime_ms == 120000);
        assert(s_table.tasks[2].next_utc_ms == base_utc_ms + TIMER_DAY_MS + 60000);
        elapse(59989);
        assert(send_count == 0);
        elapse(1);
        assert(send_count == 1 && event_count == 1);
        assert(s_table.tasks[1].occurrence_id == 1);
    }
}

static void payload_and_validation(void)
{
    fresh();
    cJSON *pd = cJSON_CreateObject();
    cJSON *tasks = cJSON_CreateArray();
    cJSON_AddItemToObject(pd, "task", tasks);
    cJSON *config = cJSON_Parse(CONFIG("false", "large", "duration", CONTROL_ON));
    cJSON *actions = cJSON_GetObjectItemCaseSensitive(config, "action");
    for (unsigned i = 0; i < 300; ++i) {
        cJSON_AddItemToArray(actions, cJSON_Parse("{\"type\":\"delay\",\"duration_ms\":0}"));
    }
    cJSON_AddItemToArray(tasks, config);
    cJSON *response = gw_gateway_timer_handle("gateway.timer.upsert", pd);
    assert(response != NULL && field(response, "code")->valueint == 0);
    assert(strlen(persisted) > 8192);
    cJSON_Delete(response);
    cJSON_Delete(pd);
    reboot();
    assert(cJSON_GetArraySize(field(s_table.tasks[0].config, "action")) == 301);
    response = request("upsert", "{\"task\":["
        CONFIG("false", "bad", "duration", "{\"type\":\"delay\",\"duration_ms\":1.5}") "]}", 1);
    cJSON_Delete(response);
    response = request("get", "{\"task_id\":1}", 1);
    cJSON_Delete(response);
    response = request("list", "{\"cursor\":0,\"limit\":4}", 1);
    cJSON_Delete(response);
}

int main(void)
{
    batch_atomicity();
    actions_delays_and_busy();
    address_conversion_and_failure();
    claim_persistence_and_restart();
    interval_no_overlap();
    time_rules_and_daily_rollback();
    small_correction_restart_projection();
    small_correction_crosses_deadline();
    rebase_skips_missed_occurrences();
    payload_and_validation();
    free_table(&s_table);
    free(persisted);
    cJSON_Delete(last_event);
    puts("gateway timer: 10 focused scenarios passed");
    return 0;
}
