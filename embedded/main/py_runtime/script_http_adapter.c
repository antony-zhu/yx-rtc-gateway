/**
 * @file script_http_adapter.c
 * @brief 脚本 HTTP 异步适配层实现。
 *
 * 本文件把脚本侧 HTTP 调用拆成两个阶段：提交请求和轮询完成事件。这样 Python VM
 * 不会因为网络连接、DNS、服务器响应慢而被长时间阻塞；同时所有脚本 HTTP 相关的
 * URL、请求体和响应体都放在 PSRAM，避免用户脚本业务消耗内部 RAM。
 */

#include "script_http_adapter.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gw_defaults.h"
#include "gw_net.h"
#include "gw_py_bridge.h"
#include "gw_time.h"

static const char *TAG = "script_http";

/**
 * @brief worker 内部请求对象。
 *
 * Python 传入的对象只在 C 函数调用期间可靠，因此提交时必须把所有可变输入复制到
 * 该结构持有的 PSRAM 缓冲区。`generation` 用来区分脚本生命周期；脚本退出后，
 * 旧 generation 的请求即使后来完成，也不能再投递给新脚本。
 */
typedef struct {
    uint32_t request_id;              /*!< 本地请求 ID */
    uint32_t generation;              /*!< 提交时的脚本 HTTP 生命周期代数 */
    script_http_request_t req;        /*!< 指向本结构自持有缓冲区的请求参数 */
    char *url;                        /*!< PSRAM 中的 URL 副本 */
    char *content_type;               /*!< PSRAM 中的 Content-Type 副本 */
    uint8_t *body;                    /*!< PSRAM 中的请求体副本 */
    uint8_t *response_buf;            /*!< PSRAM 中的响应缓冲区 */
    size_t response_buf_size;         /*!< 响应缓冲区大小，含末尾 0 */
} script_http_job_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_request_queue;
static QueueHandle_t s_event_queue;
static TaskHandle_t s_worker_task;
static bool s_initialized;
static bool s_active;
static bool s_closing;
static uint32_t s_generation;
static uint32_t s_next_request_id = 1U;
static uint32_t s_active_request_id;
static uint32_t s_last_request_id;
static uint32_t s_request_count;
static uint32_t s_completed_count;
static uint32_t s_success_count;
static uint32_t s_failure_count;
static uint32_t s_rejected_count;
static uint32_t s_dropped_count;
static uint32_t s_polled_count;
static int s_last_http_status;
static esp_err_t s_last_esp_err = ESP_OK;
static char s_last_reason[GW_SCRIPT_MQTT_REASON_MAX_LEN];

/**
 * @brief 判断当前是否处于脚本退出清理窗口。
 *
 * 该状态只用于拒绝新提交和让旧请求完成后丢弃结果；它不是 HTTP worker 的永久停止
 * 标志，worker 会长期存在并服务后续脚本运行期。
 */
static bool queue_is_closing(void)
{
    bool closing;

    portENTER_CRITICAL(&s_lock);
    closing = s_closing;
    portEXIT_CRITICAL(&s_lock);
    return closing;
}

/**
 * @brief 记录一次被拒绝的请求。
 *
 * 参数错误、网络未连接、队列满、PSRAM 不足等“请求未进入执行队列”的情况都会走
 * 这里，便于 `http.status()` 给脚本和调试工具返回最近失败原因。
 */
static void status_reject(esp_err_t err, const char *reason)
{
    portENTER_CRITICAL(&s_lock);
    ++s_rejected_count;
    s_last_esp_err = err;
    strlcpy(s_last_reason, reason != NULL ? reason : "", sizeof(s_last_reason));
    portEXIT_CRITICAL(&s_lock);
}

/**
 * @brief 分配本地 request_id。
 *
 * request_id 只在当前设备本地用于 `http.poll()` 匹配，不等同于平台 MQTT 的
 * request_id。32 位计数回绕时跳过 0，因为 0 在状态里表示“无活动请求”。
 */
static uint32_t alloc_request_id(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t request_id = s_next_request_id++;
    if (s_next_request_id == 0U) {
        s_next_request_id = 1U;
    }
    portEXIT_CRITICAL(&s_lock);
    return request_id != 0U ? request_id : 1U;
}

/**
 * @brief 标记 worker 当前活动请求。
 */
static void set_active(uint32_t request_id, bool active)
{
    portENTER_CRITICAL(&s_lock);
    s_active = active;
    s_active_request_id = active ? request_id : 0U;
    portEXIT_CRITICAL(&s_lock);
}

/**
 * @brief 更新一次请求完成后的统计信息。
 *
 * 统计口径是“worker 已执行完成”，不要求完成事件已经被 Python poll 走。这样
 * `completed_count`、`success_count` 和 `failure_count` 在同一把锁内更新，避免
 * 脚本刚取到事件时看到 `success_count` 已增加但 `completed_count` 还没增加。
 */
static void update_finished_status(const script_http_event_t *event)
{
    const char *reason = "";
    if (event != NULL && event->response.result.reason_code != NULL) {
        reason = event->response.result.reason_code;
    }

    portENTER_CRITICAL(&s_lock);
    ++s_completed_count;
    if (event != NULL && event->esp_err == ESP_OK &&
        event->response.http_status >= 200 && event->response.http_status < 300) {
        ++s_success_count;
    } else {
        ++s_failure_count;
    }
    s_last_http_status = event != NULL ? event->response.http_status : 0;
    s_last_esp_err = event != NULL ? event->esp_err : ESP_FAIL;
    strlcpy(s_last_reason, reason, sizeof(s_last_reason));
    portEXIT_CRITICAL(&s_lock);
}

/**
 * @brief 判断完成事件是否仍属于当前脚本生命周期。
 *
 * 脚本退出时不会强杀正在进行的 HTTP 请求，因此 worker 在投递完成事件前必须再次
 * 检查 generation，防止旧脚本的事件落到新脚本里。
 */
static bool event_generation_alive(uint32_t generation)
{
    bool alive;

    portENTER_CRITICAL(&s_lock);
    alive = !s_closing && generation == s_generation;
    portEXIT_CRITICAL(&s_lock);
    return alive;
}

/**
 * @brief 递增丢弃计数并保存最近原因。
 */
static void inc_dropped(const char *reason)
{
    portENTER_CRITICAL(&s_lock);
    ++s_dropped_count;
    strlcpy(s_last_reason,
            reason != NULL && reason[0] != '\0' ? reason : "event_dropped",
            sizeof(s_last_reason));
    portEXIT_CRITICAL(&s_lock);
}

/**
 * @brief 复制文本到 PSRAM。
 *
 * @return 成功返回以 0 结尾的副本；输入为空字符串时返回 NULL。
 */
static char *copy_text_psram(const char *text, size_t max_len)
{
    if (text == NULL || text[0] == '\0') {
        return NULL;
    }

    size_t len = strnlen(text, max_len + 1U);
    if (len > max_len) {
        return NULL;
    }

    char *copy = (char *)heap_caps_malloc(len + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, len + 1U);
    return copy;
}

/**
 * @brief 复制请求体到 PSRAM。
 */
static uint8_t *copy_body_psram(const void *body, size_t body_len)
{
    if (body == NULL || body_len == 0U) {
        return NULL;
    }

    uint8_t *copy = (uint8_t *)heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, body, body_len);
    return copy;
}

/**
 * @brief 释放 job 持有的请求输入。
 */
static void free_job_owned_inputs(script_http_job_t *job)
{
    if (job == NULL) {
        return;
    }
    if (job->url != NULL) {
        heap_caps_free(job->url);
        job->url = NULL;
    }
    if (job->content_type != NULL) {
        heap_caps_free(job->content_type);
        job->content_type = NULL;
    }
    if (job->body != NULL) {
        memset(job->body, 0, job->req.body_len);
        heap_caps_free(job->body);
        job->body = NULL;
    }
}

/**
 * @brief 销毁 worker 内部请求对象。
 *
 * 如果响应体已经转移给完成事件，调用前应把 `job->response_buf` 置空，避免重复释放。
 */
static void destroy_job(script_http_job_t *job)
{
    if (job == NULL) {
        return;
    }
    free_job_owned_inputs(job);
    if (job->response_buf != NULL) {
        heap_caps_free(job->response_buf);
        job->response_buf = NULL;
    }
    heap_caps_free(job);
}

/**
 * @brief 校验脚本 HTTP 请求并计算响应体上限。
 */
static esp_err_t validate_request(const script_http_request_t *req, size_t *max_response_bytes)
{
    if (req == NULL || req->url == NULL || req->url[0] == '\0' || max_response_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gw_http_url_scheme_allowed(req->url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->method != GW_HTTP_METHOD_GET && req->method != GW_HTTP_METHOD_POST) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (req->method == GW_HTTP_METHOD_GET && req->body_len > 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->body_len > GW_SCRIPT_HTTP_BODY_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (req->body_len > 0U && req->body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t limit = req->max_response_bytes != 0U ?
                   req->max_response_bytes :
                   GW_HTTP_DEFAULT_RESPONSE_MAX_BYTES;
    if (limit > GW_SCRIPT_HTTP_RESPONSE_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    *max_response_bytes = limit;
    return ESP_OK;
}

/**
 * @brief 创建并填充 worker 请求对象。
 */
static esp_err_t create_job(const script_http_request_t *req,
                            size_t max_response_bytes,
                            uint32_t request_id,
                            uint32_t generation,
                            script_http_job_t **out_job)
{
    script_http_job_t *job = (script_http_job_t *)heap_caps_calloc(1,
                                                                   sizeof(script_http_job_t),
                                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }

    job->request_id = request_id;
    job->generation = generation;
    job->req = *req;

    job->url = copy_text_psram(req->url, GW_HTTP_MAX_URL_LEN);
    if (job->url == NULL) {
        destroy_job(job);
        return ESP_ERR_NO_MEM;
    }
    job->req.url = job->url;

    job->content_type = copy_text_psram(req->content_type, 128U);
    job->req.content_type = job->content_type;
    if (req->content_type != NULL && req->content_type[0] != '\0' && job->content_type == NULL) {
        destroy_job(job);
        return ESP_ERR_NO_MEM;
    }

    job->body = copy_body_psram(req->body, req->body_len);
    job->req.body = job->body;
    if (req->body_len > 0U && job->body == NULL) {
        destroy_job(job);
        return ESP_ERR_NO_MEM;
    }

    job->response_buf_size = max_response_bytes + 1U;
    job->response_buf = (uint8_t *)heap_caps_calloc(1,
                                                    job->response_buf_size,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job->response_buf == NULL) {
        destroy_job(job);
        return ESP_ERR_NO_MEM;
    }

    *out_job = job;
    return ESP_OK;
}

/**
 * @brief HTTP worker 主循环。
 *
 * worker 只处理队列中的脚本 HTTP 请求，不直接碰 Python 对象。完成后把结果复制到
 * 完成事件队列，并通过 C/Python bridge 唤醒脚本侧 waker。
 */
static void script_http_worker(void *arg)
{
    (void)arg;

    for (;;) {
        script_http_job_t *job = NULL;
        if (xQueueReceive(s_request_queue, &job, portMAX_DELAY) != pdTRUE || job == NULL) {
            continue;
        }

        if (!event_generation_alive(job->generation)) {
            inc_dropped("stale_request");
            destroy_job(job);
            continue;
        }

        set_active(job->request_id, true);

        gw_http_request_t http_req = {
            .method = job->req.method,
            .url = job->url,
            .content_type = job->content_type,
            .body = job->body,
            .body_len = job->req.body_len,
            .response_buf = (char *)job->response_buf,
            .response_buf_size = job->response_buf_size,
            .connect_timeout_sec = job->req.connect_timeout_sec,
            .response_interval_timeout_sec = job->req.response_interval_timeout_sec,
        };
        gw_http_response_t http_resp = {0};
        esp_err_t http_err = gw_http_perform(&http_req, &http_resp);

        gw_time_point_t now = gw_time_now();
        script_http_event_t event = {
            .request_id = job->request_id,
            .esp_err = http_err,
            .response = {
                .result = http_resp.result,
                .http_status = http_resp.http_status,
                .body_len = http_resp.response_len,
                .body_truncated = http_resp.response_truncated,
                .body = job->response_buf,
                .timestamp_ms = now.utc_trusted ? now.utc_ms : 0,
            },
        };

        update_finished_status(&event);
        set_active(job->request_id, false);

        bool transferred = false;
        if (event_generation_alive(job->generation)) {
            if (xQueueSend(s_event_queue, &event, 0) == pdTRUE) {
                transferred = true;
                (void)gw_py_bridge_wake(GW_PY_BRIDGE_SOURCE_HTTP);
            } else {
                inc_dropped("event_queue_full");
            }
        } else {
            inc_dropped("stale_response");
        }

        if (transferred) {
            job->response_buf = NULL;
        } else {
            script_http_adapter_release_response(&event.response);
            job->response_buf = NULL;
        }
        destroy_job(job);
    }
}

/**
 * @brief 确保队列和 worker 已创建。
 */
static esp_err_t ensure_started(void)
{
    if (s_initialized && s_request_queue != NULL && s_event_queue != NULL && s_worker_task != NULL) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_lock);
    bool need_request_queue = s_request_queue == NULL;
    bool need_event_queue = s_event_queue == NULL;
    portEXIT_CRITICAL(&s_lock);

    if (need_request_queue) {
        QueueHandle_t queue = xQueueCreate(GW_SCRIPT_HTTP_QUEUE_LEN, sizeof(script_http_job_t *));
        if (queue == NULL) {
            status_reject(ESP_ERR_NO_MEM, "request_queue_alloc_failed");
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_lock);
        if (s_request_queue == NULL) {
            s_request_queue = queue;
            queue = NULL;
        }
        portEXIT_CRITICAL(&s_lock);
        if (queue != NULL) {
            vQueueDelete(queue);
        }
    }

    if (need_event_queue) {
        QueueHandle_t queue = xQueueCreate(GW_SCRIPT_HTTP_EVENT_QUEUE_LEN, sizeof(script_http_event_t));
        if (queue == NULL) {
            status_reject(ESP_ERR_NO_MEM, "event_queue_alloc_failed");
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_lock);
        if (s_event_queue == NULL) {
            s_event_queue = queue;
            queue = NULL;
        }
        portEXIT_CRITICAL(&s_lock);
        if (queue != NULL) {
            vQueueDelete(queue);
        }
    }

    portENTER_CRITICAL(&s_lock);
    bool need_task = s_worker_task == NULL;
    portEXIT_CRITICAL(&s_lock);

    if (need_task) {
        TaskHandle_t task = NULL;
        BaseType_t ok = xTaskCreate(script_http_worker,
                                    "script_http",
                                    GW_HTTP_WORKER_STACK_BYTES,
                                    NULL,
                                    5,
                                    &task);
        if (ok != pdPASS) {
            status_reject(ESP_ERR_NO_MEM, "worker_alloc_failed");
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_lock);
        if (s_worker_task == NULL) {
            s_worker_task = task;
            task = NULL;
        }
        portEXIT_CRITICAL(&s_lock);
        if (task != NULL) {
            vTaskDelete(task);
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t script_http_adapter_init(void)
{
    return ensure_started();
}

esp_err_t script_http_adapter_start(const script_http_request_t *req, uint32_t *out_request_id)
{
    if (out_request_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_request_id = 0U;

    size_t max_response_bytes = 0U;
    esp_err_t err = validate_request(req, &max_response_bytes);
    if (err != ESP_OK) {
        status_reject(err, "invalid_request");
        return err;
    }

    if (queue_is_closing()) {
        status_reject(ESP_ERR_INVALID_STATE, "script_http_closing");
        return ESP_ERR_INVALID_STATE;
    }

    gw_net_status_snapshot_t net = {0};
    if (gw_net_get_status(&net) != ESP_OK || net.public_status != GW_NETWORK_STATUS_CONNECTED) {
        status_reject(ESP_ERR_INVALID_STATE, GW_REASON_NETWORK_UNAVAILABLE);
        return ESP_ERR_INVALID_STATE;
    }

    err = ensure_started();
    if (err != ESP_OK) {
        return err;
    }

    uint32_t request_id = alloc_request_id();
    uint32_t generation;
    portENTER_CRITICAL(&s_lock);
    generation = s_generation;
    portEXIT_CRITICAL(&s_lock);

    script_http_job_t *job = NULL;
    err = create_job(req, max_response_bytes, request_id, generation, &job);
    if (err != ESP_OK) {
        status_reject(err, "job_alloc_failed");
        return err;
    }

    if (!event_generation_alive(generation)) {
        status_reject(ESP_ERR_INVALID_STATE, "script_http_closing");
        destroy_job(job);
        return ESP_ERR_INVALID_STATE;
    }

    script_http_job_t *queued_job = job;
    if (xQueueSend(s_request_queue, &queued_job, 0) != pdTRUE) {
        status_reject(ESP_ERR_TIMEOUT, GW_REASON_QUEUE_FULL);
        destroy_job(job);
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_lock);
    ++s_request_count;
    s_last_request_id = request_id;
    strlcpy(s_last_reason, "queued", sizeof(s_last_reason));
    portEXIT_CRITICAL(&s_lock);

    *out_request_id = request_id;
    return ESP_OK;
}

esp_err_t script_http_adapter_poll(uint32_t timeout_ms, script_http_event_t *out_event)
{
    if (out_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_started();
    if (err != ESP_OK) {
        return err;
    }

    memset(out_event, 0, sizeof(*out_event));
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_event_queue, out_event, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_lock);
    ++s_polled_count;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void script_http_adapter_release_response(script_http_response_t *response)
{
    if (response == NULL) {
        return;
    }
    if (response->body != NULL) {
        heap_caps_free(response->body);
        response->body = NULL;
    }
    response->body_len = 0;
}

void script_http_adapter_release_event(script_http_event_t *event)
{
    if (event == NULL) {
        return;
    }
    script_http_adapter_release_response(&event->response);
    event->request_id = 0U;
    event->esp_err = ESP_OK;
}

esp_err_t script_http_adapter_get_status(script_http_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    out->initialized = s_initialized;
    out->worker_running = s_worker_task != NULL;
    out->active = s_active;
    out->closing = s_closing;
    out->generation = s_generation;
    out->active_request_id = s_active_request_id;
    out->last_request_id = s_last_request_id;
    out->request_count = s_request_count;
    out->completed_count = s_completed_count;
    out->success_count = s_success_count;
    out->failure_count = s_failure_count;
    out->rejected_count = s_rejected_count;
    out->dropped_count = s_dropped_count;
    out->polled_count = s_polled_count;
    out->last_http_status = s_last_http_status;
    out->last_esp_err = s_last_esp_err;
    strlcpy(out->last_reason, s_last_reason, sizeof(out->last_reason));
    portEXIT_CRITICAL(&s_lock);

    if (s_request_queue != NULL) {
        out->queue_pending = (uint32_t)uxQueueMessagesWaiting(s_request_queue);
    }
    if (s_event_queue != NULL) {
        out->event_pending = (uint32_t)uxQueueMessagesWaiting(s_event_queue);
    }
    return ESP_OK;
}

/**
 * @brief 清空等待执行的请求队列。
 */
static void drain_request_queue(void)
{
    if (s_request_queue == NULL) {
        return;
    }

    script_http_job_t *job = NULL;
    while (xQueueReceive(s_request_queue, &job, 0) == pdTRUE) {
        destroy_job(job);
        inc_dropped("cleanup_pending_request");
    }
}

/**
 * @brief 清空等待脚本 poll 的完成事件队列。
 */
static void drain_event_queue(void)
{
    if (s_event_queue == NULL) {
        return;
    }

    script_http_event_t event = {0};
    while (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
        script_http_adapter_release_event(&event);
        inc_dropped("cleanup_pending_event");
    }
}

void script_http_adapter_cleanup_on_script_exit(void)
{
    if (s_request_queue == NULL && s_event_queue == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_closing = true;
    ++s_generation;
    s_active_request_id = 0U;
    strlcpy(s_last_reason, "script_exit_cleanup", sizeof(s_last_reason));
    portEXIT_CRITICAL(&s_lock);

    drain_request_queue();
    drain_event_queue();
    gw_py_bridge_invalidate(GW_PY_BRIDGE_SOURCE_HTTP);

    portENTER_CRITICAL(&s_lock);
    s_closing = false;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGD(TAG, "script http cleanup done");
}
