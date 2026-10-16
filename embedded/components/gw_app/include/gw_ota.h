#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_ota.h
 * @brief 设备管理侧 OTA 升级任务接口。
 *
 * 原有 `ota.upgrade` 保留版本递增、计划时间和裸镜像下载行为。
 * 网关 `gateway.ota` 使用 YXOT 容器，并通过网关发送队列上报进度和结果。
 * 两条升级路径与手动回滚共用 OTA 占用状态。
 */

/**
 * @brief OTA 执行阶段。
 */
typedef enum {
    GW_OTA_STAGE_IDLE = 0,          /*!< 当前没有 OTA 任务 */
    GW_OTA_STAGE_ACCEPTED,          /*!< 任务已接收，等待 accepted 应答发布成功 */
    GW_OTA_STAGE_WAITING_TIME_SYNC, /*!< 定时 OTA 等待可信 UTC */
    GW_OTA_STAGE_WAITING_SCHEDULE,  /*!< 定时 OTA 等待计划时间 */
    GW_OTA_STAGE_DOWNLOADING,       /*!< 正在下载并写入 OTA 分区 */
    GW_OTA_STAGE_VERIFYING,         /*!< 正在校验大小、SHA-256 和镜像 */
    GW_OTA_STAGE_SETTING_BOOT,      /*!< 正在设置下一次启动分区 */
    GW_OTA_STAGE_SUCCESS,           /*!< OTA 成功，即将重启 */
    GW_OTA_STAGE_FAILED,            /*!< OTA 失败，保持当前固件 */
} gw_ota_stage_t;

/**
 * @brief OTA 升级请求。
 */
typedef struct {
    char request_id[65];          /*!< MQTT request_id */
    char target_version[16];      /*!< 目标固件版本，格式 major.minor.patch */
    char url[512];                /*!< 固件下载 URL，由 URL scheme 决定 HTTP 或 HTTPS */
    char sha256[65];              /*!< 固件 SHA-256 十六进制字符串 */
    uint32_t size;                /*!< 固件大小，单位字节 */
    bool has_scheduled_at;        /*!< 是否携带 scheduled_at */
    uint64_t scheduled_at_ms;     /*!< UTC 毫秒计划时间 */
    uint32_t progress_interval_sec; /*!< 进度上报间隔，0 表示使用默认值 */
    bool reboot_after_upgrade;    /*!< 当前版本固定按 true 处理 */
    bool gateway_protocol;        /*!< gateway.ota uses YXOT and the gateway event queue */
    const char *gateway_version;  /*!< Full gateway version; borrowed until submit returns */
    const char *gateway_url;      /*!< Full gateway URL; borrowed until submit returns */
    uint64_t gateway_progress_interval_sec; /*!< 0 disables gateway progress events */
} gw_ota_upgrade_request_t;

/**
 * @brief OTA 管理状态。
 */
typedef struct {
    bool initialized;             /*!< 模块是否已初始化 */
    bool worker_started;          /*!< worker 任务是否已启动 */
    gw_ota_stage_t stage;         /*!< 当前或最近 OTA 阶段 */
    char running_request_id[65];  /*!< 当前任务 request_id */
    char target_version[16];      /*!< 当前或最近目标版本 */
    uint32_t image_size;          /*!< 目标固件大小 */
    uint32_t downloaded_size;     /*!< 已下载字节数 */
    uint8_t progress;             /*!< 下载进度 0..100 */
    bool has_scheduled_at;        /*!< 是否存在计划时间 */
    uint64_t scheduled_at_ms;     /*!< UTC 毫秒计划时间 */
    uint32_t accepted_count;      /*!< 已接收任务数 */
    uint32_t success_count;       /*!< 成功任务数 */
    uint32_t failed_count;        /*!< 失败任务数 */
    uint32_t progress_publish_failed; /*!< 进度或最终结果上报失败次数 */
    int32_t last_code;            /*!< 最近业务 code */
    int32_t last_http_status;     /*!< 最近 HTTP 状态码 */
    int32_t last_esp_err;         /*!< 最近 ESP-IDF 错误码 */
    char last_reason[64];         /*!< 最近稳定原因码 */
} gw_ota_status_t;

/**
 * @brief 初始化 OTA 管理模块并启动 OTA worker。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_ota_manager_init(void);

/**
 * @brief 提交 OTA 升级任务。
 *
 * 该接口只做参数校验、任务占用和暂存；原有路径保留版本递增校验。
 * 原有 `ota.upgrade.reply` 发布成功后启动；网关路径等待 ACK 完成一次发布尝试
 * 或被丢弃后，再调用 gw_ota_manager_start_pending_upgrade() 启动。
 * 网关请求的 gateway_version 和 gateway_url 在提交成功前复制，调用方保留原字符串所有权。
 *
 * @param request OTA 升级请求。
 * @param out_result 输出统一结果，不能为空。
 * @return ESP_OK 表示任务已接收并等待 accepted 应答发布；其他值表示应立即拒绝。
 */
esp_err_t gw_ota_manager_submit_upgrade(const gw_ota_upgrade_request_t *request,
                                        gw_result_t *out_result);

/**
 * @brief 判断是否存在已接收但尚未启动 worker 的 OTA 任务。
 *
 * @return true 表示存在待启动 OTA。
 */
bool gw_ota_manager_has_pending_upgrade(void);

/**
 * @brief 启动已暂存的 OTA 任务。
 *
 * 原有路径在 accepted 应答发布成功后调用；网关路径在 ACK 被尝试或丢弃后调用，
 * 使下载及最终结果排在接收应答之后。
 *
 * @param gateway_protocol 请求所属协议，与提交请求保持一致。
 * @param request_id 提交请求的 ID，不能为空；网关协议允许空字符串。
 * @return ESP_OK 表示任务已进入 OTA worker；ESP_ERR_NOT_FOUND 表示没有属于该请求的待启动任务。
 */
esp_err_t gw_ota_manager_start_pending_upgrade(bool gateway_protocol, const char *request_id);

/**
 * @brief 取消已暂存但尚未启动的 OTA 任务。
 *
 * 仅取消协议与 request_id 均匹配的暂存任务。
 *
 * @param gateway_protocol 请求所属协议，与提交请求保持一致。
 * @param request_id 提交请求的 ID，不能为空；网关协议允许空字符串。
 * @param reason 稳定原因码，可为空。
 */
void gw_ota_manager_cancel_pending_upgrade(bool gateway_protocol,
                                           const char *request_id,
                                           const char *reason);

/**
 * @brief 查询 OTA 管理状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_ota_manager_get_status(gw_ota_status_t *out);

/** @brief OTA upgrade or prepared rollback holds the shared reservation. */
bool gw_ota_manager_is_busy(void);

/** @brief Verify and select another old OTA image, retaining the busy reservation. */
esp_err_t gw_ota_manager_prepare_rollback(gw_result_t *result);

/** @brief Request reboot after the prepared rollback ACK has been attempted. */
esp_err_t gw_ota_manager_reboot_prepared_rollback(void);

/**
 * @brief OTA 阶段转稳定字符串。
 *
 * @param stage OTA 阶段。
 * @return 静态字符串。
 */
const char *gw_ota_stage_to_string(gw_ota_stage_t stage);

#ifdef __cplusplus
}
#endif
