#include "py_runtime.h"

#include <string.h>
#include <sys/stat.h>

/**
 * @file py_runtime.c
 * @brief 用户 Python 脚本运行时管理。
 *
 * 该模块负责选择当前配置槽的 `main.py`、创建脚本任务、分配 MicroPython PSRAM
 * heap、处理停止请求和上报脚本生命周期事件。脚本任务栈仍使用 FreeRTOS 内部 RAM，
 * MicroPython heap 必须使用 PSRAM。
 */

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "gw_defaults.h"
#include "gw_event.h"
#include "gw_fs.h"
#include "gw_health.h"
#include "gw_log.h"
#include "gw_py_bridge.h"
#include "gw_redact.h"
#include "gw_result.h"
#include "gw_state.h"
#include "gw_time.h"
#include "micropython_embed.h"
#include "script_http_adapter.h"
#include "script_mqtt_adapter.h"

static const char *TAG = "py_runtime";
static const size_t PY_RUNTIME_C_RESERVED_BYTES = 2U * 1024U * 1024U;
static const size_t PY_RUNTIME_HEAP_MIN_BYTES = 512U * 1024U;

#ifdef GW_PY_RUNTIME_ENABLE_TEST_HOOKS
/* 测试构建中仅替换 MicroPython heap 分配，避免全局 wrap ESP-IDF heap 影响启动期。 */
void *py_runtime_test_heap_alloc(size_t size);
void py_runtime_test_heap_free(void *ptr);
#define PY_RUNTIME_HEAP_ALLOC(size) py_runtime_test_heap_alloc(size)
#define PY_RUNTIME_HEAP_FREE(ptr) py_runtime_test_heap_free(ptr)
#else
#define PY_RUNTIME_HEAP_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define PY_RUNTIME_HEAP_FREE(ptr) heap_caps_free(ptr)
#endif

static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_script_task_handle;
static bool s_stop_requested;
static char s_stop_reason[32];
static char s_last_stop_reason[32];
static char s_active_slot[8];
static char s_active_slot_path[GW_FS_MAX_PATH_LEN];
static char s_script_path[GW_FS_MAX_PATH_LEN];
static size_t s_current_heap_size;
static size_t s_last_heap_size;
static uint32_t s_start_count;
static uint32_t s_exit_count;
static uint32_t s_exception_count;
static esp_err_t s_last_error = ESP_OK;
static char s_last_exception[384];

/** @brief 向设备管理主流程投递脚本启动事件。 */
static void post_script_started_event(void)
{
    gw_time_point_t now = gw_time_now();
    gw_event_t event = {
        .event_id = GW_EVT_SCRIPT_STARTED,
        .priority = GW_EVENT_PRIORITY_NORMAL,
        .source = "script_task",
        .target = "dev_mgr_task",
        .request_id = NULL,
        .method = NULL,
        .uptime_ms = now.uptime_ms,
        .timestamp = now.utc_trusted ? now.utc_ms : 0,
        .result = gw_result_ok(),
        .payload = NULL,
        .payload_len = 0,
        .release_cb = NULL,
    };

    (void)gw_event_post(&event);
}

/** @brief 向设备管理主流程投递脚本停止事件。 */
static void post_script_stopped_event(const char *reason)
{
    gw_time_point_t now = gw_time_now();
    gw_event_t event = {
        .event_id = GW_EVT_SCRIPT_STOPPED,
        .priority = GW_EVENT_PRIORITY_NORMAL,
        .source = "script_task",
        .target = "dev_mgr_task",
        .request_id = NULL,
        .method = NULL,
        .uptime_ms = now.uptime_ms,
        .timestamp = now.utc_trusted ? now.utc_ms : 0,
        .result = gw_result_error(GW_RESULT_OK,
                                  reason != NULL && reason[0] != '\0' ? reason : "script_stopped",
                                  "script stopped"),
        .payload = NULL,
        .payload_len = 0,
        .release_cb = NULL,
    };

    (void)gw_event_post(&event);
}

/**
 * @brief 脚本运行任务入口。
 *
 * 任务启动后先按当前 PSRAM 余量计算 MicroPython heap，保留一部分 PSRAM 给 C 层网络、
 * MQTT、HTTP payload 等运行期对象。VM 初始化完成后才激活 C/Python bridge，退出前
 * 先清理脚本 MQTT/HTTP 资源，再让旧 waker generation 失效。
 */
static void script_task(void *arg)
{
    (void)arg;
    void *heap = NULL;
    size_t heap_size = 0U;
    volatile uint32_t stack_anchor = 0U;
    bool had_exception = false;
    bool was_keyboard_interrupt = false;
    char exception_text[384] = {0};
    char redacted_exception[sizeof(exception_text)] = {0};
    char active_slot_path[sizeof(s_active_slot_path)];
    char script_path[sizeof(s_script_path)];

    portENTER_CRITICAL(&s_runtime_lock);
    strlcpy(active_slot_path, s_active_slot_path, sizeof(active_slot_path));
    strlcpy(script_path, s_script_path, sizeof(script_path));
    portEXIT_CRITICAL(&s_runtime_lock);

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (free_psram > PY_RUNTIME_C_RESERVED_BYTES + PY_RUNTIME_HEAP_MIN_BYTES) {
        heap_size = free_psram - PY_RUNTIME_C_RESERVED_BYTES;
        if (heap_size > largest_psram) {
            heap_size = largest_psram;
        }
        heap_size &= ~(size_t)0x0FU;
    }
    if (heap_size < PY_RUNTIME_HEAP_MIN_BYTES) {
        ESP_LOGE(TAG,
                 "not enough psram for MicroPython heap: free=%u largest=%u",
                 (unsigned int)free_psram,
                 (unsigned int)largest_psram);
        portENTER_CRITICAL(&s_runtime_lock);
        s_last_error = ESP_ERR_NO_MEM;
        strlcpy(s_last_exception, "not enough psram for MicroPython heap", sizeof(s_last_exception));
        portEXIT_CRITICAL(&s_runtime_lock);
        gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                             "no_psram",
                                             "MicroPython heap unavailable");
        gw_log_write(GW_LOG_LEVEL_ERROR,
                     "script",
                     "heap_alloc_failed",
                     "not enough psram for MicroPython heap",
                     &result);
        gw_state_set_script_status(GW_SCRIPT_STATUS_ERROR);
        goto stopped;
    }

    heap = PY_RUNTIME_HEAP_ALLOC(heap_size);
    if (heap == NULL) {
        ESP_LOGE(TAG, "alloc MicroPython heap failed: size=%u", (unsigned int)heap_size);
        portENTER_CRITICAL(&s_runtime_lock);
        s_last_error = ESP_ERR_NO_MEM;
        strlcpy(s_last_exception, "MicroPython heap alloc failed", sizeof(s_last_exception));
        portEXIT_CRITICAL(&s_runtime_lock);
        gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                             "no_psram",
                                             "MicroPython heap alloc failed");
        gw_log_write(GW_LOG_LEVEL_ERROR,
                     "script",
                     "heap_alloc_failed",
                     "alloc MicroPython heap failed",
                     &result);
        gw_state_set_script_status(GW_SCRIPT_STATUS_ERROR);
        goto stopped;
    }

    ESP_LOGI(TAG,
             "script_task MicroPython start: script=%s heap=%u",
             script_path,
             (unsigned int)heap_size);
    portENTER_CRITICAL(&s_runtime_lock);
    ++s_start_count;
    s_current_heap_size = heap_size;
    s_last_heap_size = heap_size;
    s_last_error = ESP_OK;
    s_last_exception[0] = '\0';
    portEXIT_CRITICAL(&s_runtime_lock);
    gw_log_write(GW_LOG_LEVEL_INFO,
                 "script",
                 "mp_start",
                 "MicroPython script start",
                 NULL);
    gw_micropython_init(heap, heap_size, (void *)&stack_anchor);
    gw_py_bridge_set_vm_active(true);
    gw_micropython_prepare_sys_path(active_slot_path);
    gw_state_set_script_status(GW_SCRIPT_STATUS_RUNNING);
    post_script_started_event();

    esp_err_t err = gw_micropython_exec_file(script_path,
                                             &had_exception,
                                             &was_keyboard_interrupt,
                                             exception_text,
                                             sizeof(exception_text));
    gw_state_update_stack_high_watermark("script_task",
                                         (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "execute script failed: %s", esp_err_to_name(err));
        portENTER_CRITICAL(&s_runtime_lock);
        s_last_error = err;
        strlcpy(s_last_exception, esp_err_to_name(err), sizeof(s_last_exception));
        portEXIT_CRITICAL(&s_runtime_lock);
        gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                             "script_exec_failed",
                                             "execute script failed");
        result.esp_err = err;
        gw_log_write(GW_LOG_LEVEL_ERROR,
                     "script",
                     "exec_failed",
                     "execute script failed",
                     &result);
        gw_state_set_script_status(GW_SCRIPT_STATUS_ERROR);
    } else if (had_exception && !was_keyboard_interrupt) {
        const char *raw_exception = exception_text[0] != '\0' ? exception_text : "script exception";
        (void)gw_redact_sensitive_text(raw_exception, redacted_exception, sizeof(redacted_exception));
        ESP_LOGE(TAG, "script exception: %s", redacted_exception[0] != '\0' ? redacted_exception : "script exception");
        portENTER_CRITICAL(&s_runtime_lock);
        ++s_exception_count;
        s_last_error = ESP_FAIL;
        strlcpy(s_last_exception,
                redacted_exception[0] != '\0' ? redacted_exception : "script exception",
                sizeof(s_last_exception));
        portEXIT_CRITICAL(&s_runtime_lock);
        gw_result_t result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                             "script_exception",
                                             "script exception");
        gw_log_write(GW_LOG_LEVEL_ERROR,
                     "script",
                     "exception",
                     redacted_exception[0] != '\0' ? redacted_exception : "script exception",
                     &result);
        gw_state_set_script_status(GW_SCRIPT_STATUS_ERROR);
    } else {
        portENTER_CRITICAL(&s_runtime_lock);
        ++s_exit_count;
        s_last_error = ESP_OK;
        if (!was_keyboard_interrupt) {
            s_last_exception[0] = '\0';
        }
        portEXIT_CRITICAL(&s_runtime_lock);
        gw_log_write(GW_LOG_LEVEL_INFO,
                     "script",
                     was_keyboard_interrupt ? "stopped" : "exited",
                     was_keyboard_interrupt ? "script stopped by request" : "script exited",
                     NULL);
        gw_state_set_script_status(GW_SCRIPT_STATUS_STOPPED);
    }

    script_mqtt_adapter_cleanup_on_script_exit();
    script_http_adapter_cleanup_on_script_exit();
    gw_py_bridge_reset_all();
    gw_micropython_deinit();

stopped:
    char reason[sizeof(s_stop_reason)];
    char final_reason[sizeof(s_last_stop_reason)];
    const char *derived_reason = had_exception && !was_keyboard_interrupt ? "script_exception" :
                                 was_keyboard_interrupt ? "stop_requested" :
                                 "script_exited";
    portENTER_CRITICAL(&s_runtime_lock);
    strlcpy(reason, s_stop_reason, sizeof(reason));
    strlcpy(final_reason, reason[0] != '\0' ? reason : derived_reason, sizeof(final_reason));
    strlcpy(s_last_stop_reason, final_reason, sizeof(s_last_stop_reason));
    s_script_task_handle = NULL;
    s_stop_requested = false;
    s_current_heap_size = 0U;
    portEXIT_CRITICAL(&s_runtime_lock);

    ESP_LOGI(TAG, "script_task stopped: reason=%s", final_reason);
    if (heap != NULL) {
        PY_RUNTIME_HEAP_FREE(heap);
    }
    (void)gw_health_report_heartbeat("script_task", "stopped");
    post_script_stopped_event(final_reason);
    vTaskDelete(NULL);
}

/**
 * @brief 根据当前配置槽生成脚本目录和 main.py 路径。
 *
 * @param active_slot 只能是 cfg_a 或 cfg_b。
 * @param out_slot_path 输出配置槽挂载路径。
 * @param out_slot_path_size 输出缓冲区大小。
 * @param out_script_path 输出 main.py 路径。
 * @param out_script_path_size 输出缓冲区大小。
 * @return ESP_OK 表示 main.py 存在且路径生成成功。
 */
static esp_err_t build_script_paths(const char *active_slot,
                                    char *out_slot_path,
                                    size_t out_slot_path_size,
                                    char *out_script_path,
                                    size_t out_script_path_size)
{
    if (active_slot == NULL ||
        (strcmp(active_slot, "cfg_a") != 0 && strcmp(active_slot, "cfg_b") != 0) ||
        out_slot_path == NULL ||
        out_script_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out_slot_path,
                           out_slot_path_size,
                           GW_FS_CFGFS_BASE_PATH "/%s",
                           active_slot);
    if (written <= 0 || written >= (int)out_slot_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(out_script_path,
                       out_script_path_size,
                       "%s/main.py",
                       out_slot_path);
    if (written <= 0 || written >= (int)out_script_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat st = {0};
    if (stat(out_script_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t py_runtime_start(const char *active_slot)
{
    char active_slot_path[sizeof(s_active_slot_path)];
    char script_path[sizeof(s_script_path)];
    esp_err_t path_err = build_script_paths(active_slot,
                                            active_slot_path,
                                            sizeof(active_slot_path),
                                            script_path,
                                            sizeof(script_path));
    if (path_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "script main.py unavailable: slot=%s err=%s",
                 active_slot != NULL ? active_slot : "-",
                 esp_err_to_name(path_err));
        gw_result_t result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                             "script_not_found",
                                             "script main.py unavailable");
        result.esp_err = path_err;
        gw_log_write(GW_LOG_LEVEL_WARN,
                     "script",
                     "main_unavailable",
                     "script main.py unavailable",
                     &result);
        portENTER_CRITICAL(&s_runtime_lock);
        strlcpy(s_active_slot,
                active_slot != NULL ? active_slot : "",
                sizeof(s_active_slot));
        s_active_slot_path[0] = '\0';
        s_script_path[0] = '\0';
        s_last_error = path_err;
        strlcpy(s_last_exception, "script main.py unavailable", sizeof(s_last_exception));
        portEXIT_CRITICAL(&s_runtime_lock);
        gw_state_set_script_status(GW_SCRIPT_STATUS_ERROR);
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    bool already_started = s_script_task_handle != NULL;
    if (!already_started) {
        s_stop_requested = false;
        s_stop_reason[0] = '\0';
        strlcpy(s_active_slot, active_slot, sizeof(s_active_slot));
        strlcpy(s_active_slot_path, active_slot_path, sizeof(s_active_slot_path));
        strlcpy(s_script_path, script_path, sizeof(s_script_path));
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    if (already_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gw_py_bridge_init(), TAG, "init python bridge failed");

    gw_state_set_script_status(GW_SCRIPT_STATUS_STARTING);
    BaseType_t ok = xTaskCreate(script_task,
                                "script_task",
                                GW_TASK_STACK_SCRIPT_BYTES,
                                NULL,
                                6,
                                &s_script_task_handle);
    if (ok != pdPASS) {
        portENTER_CRITICAL(&s_runtime_lock);
        s_script_task_handle = NULL;
        s_last_error = ESP_ERR_NO_MEM;
        strlcpy(s_last_exception, "create script_task failed", sizeof(s_last_exception));
        portEXIT_CRITICAL(&s_runtime_lock);
        gw_state_set_script_status(GW_SCRIPT_STATUS_ERROR);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t py_runtime_request_stop(const char *reason)
{
    portENTER_CRITICAL(&s_runtime_lock);
    s_stop_requested = true;
    bool started = s_script_task_handle != NULL;
    strlcpy(s_stop_reason,
            reason != NULL && reason[0] != '\0' ? reason : "stop_requested",
            sizeof(s_stop_reason));
    portEXIT_CRITICAL(&s_runtime_lock);

    if (started) {
        (void)gw_py_bridge_request_keyboard_interrupt();
    }
    return ESP_OK;
}

esp_err_t py_runtime_wait_stopped(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (py_runtime_is_started()) {
        if (timeout_ms != UINT32_MAX && xTaskGetTickCount() - start >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

bool py_runtime_should_stop(void)
{
    bool value;

    portENTER_CRITICAL(&s_runtime_lock);
    value = s_stop_requested;
    portEXIT_CRITICAL(&s_runtime_lock);
    return value;
}

bool py_runtime_is_started(void)
{
    bool started;

    portENTER_CRITICAL(&s_runtime_lock);
    started = s_script_task_handle != NULL;
    portEXIT_CRITICAL(&s_runtime_lock);
    return started;
}

#ifdef GW_PY_RUNTIME_ENABLE_TEST_HOOKS
void py_runtime_test_reset(void)
{
    TaskHandle_t task = NULL;

    portENTER_CRITICAL(&s_runtime_lock);
    task = s_script_task_handle;
    s_script_task_handle = NULL;
    s_stop_requested = false;
    s_stop_reason[0] = '\0';
    s_last_stop_reason[0] = '\0';
    s_active_slot[0] = '\0';
    s_active_slot_path[0] = '\0';
    s_script_path[0] = '\0';
    s_current_heap_size = 0U;
    s_last_heap_size = 0U;
    s_start_count = 0U;
    s_exit_count = 0U;
    s_exception_count = 0U;
    s_last_error = ESP_OK;
    s_last_exception[0] = '\0';
    portEXIT_CRITICAL(&s_runtime_lock);

    /* 失败用例清理兜底；正常用例应先让脚本任务自行退出。 */
    if (task != NULL && task != xTaskGetCurrentTaskHandle()) {
        vTaskDelete(task);
    }
}
#endif

esp_err_t py_runtime_get_status(py_runtime_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    gw_state_snapshot_t state = {0};
    (void)gw_state_get_snapshot(&state);

    portENTER_CRITICAL(&s_runtime_lock);
    out->started = s_script_task_handle != NULL;
    out->stop_requested = s_stop_requested;
    out->script_status = state.script_status;
    out->start_count = s_start_count;
    out->exit_count = s_exit_count;
    out->exception_count = s_exception_count;
    out->script_stack_free_words = state.script_stack_free_words;
    out->current_heap_size = s_current_heap_size;
    out->last_heap_size = s_last_heap_size;
    out->last_error = s_last_error;
    strlcpy(out->active_slot, s_active_slot, sizeof(out->active_slot));
    strlcpy(out->active_slot_path, s_active_slot_path, sizeof(out->active_slot_path));
    strlcpy(out->script_path, s_script_path, sizeof(out->script_path));
    strlcpy(out->stop_reason,
            s_stop_reason[0] != '\0' ? s_stop_reason : s_last_stop_reason,
            sizeof(out->stop_reason));
    strlcpy(out->last_exception, s_last_exception, sizeof(out->last_exception));
    portEXIT_CRITICAL(&s_runtime_lock);

    return ESP_OK;
}
