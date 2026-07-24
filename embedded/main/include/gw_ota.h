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
 * OTA 只由设备管理 MQTT 协议触发，脚本不能直接启动固件升级。协议层负责解析
 * `ota.upgrade` 请求，本模块负责版本递增校验、计划时间等待、HTTP/HTTPS 流式下载、
 * SHA-256 校验、ESP-IDF OTA 写入和最终进度上报。
 */

/**
 * @brief OTA 执行阶段。
 */
typedef enum {
    GW_OTA_STAGE_IDLE = 0,          /*!< 当前没有 OTA 任务 */
    GW_OTA_STAGE_ACCEPTED,          /*!< 任务已接收，等待 accepted 应答发布成功 */
    GW_OTA_STAGE_WAITING_TIME_SYNC, /*!< 定时 OTA 等待可信 UTC */
    GW_OTA_STAGE_WAITING_SCHEDULE,  /*!< 定时 OTA 等待计划时间 */
    GW_OTA_STAGE_STOPPING_SCRIPT,   /*!< 正在请求脚本停止 */
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
 * 该接口只做参数校验、版本递增校验、互斥任务占用和暂存；实际下载必须在协议层
 * 成功发布 `ota.upgrade.reply` accepted 应答后，再调用
 * gw_ota_manager_start_pending_upgrade() 启动。
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
 * 该接口只能在 accepted MQTT 应答成功发布后调用，避免最终进度上报早于接收应答。
 *
 * @return ESP_OK 表示任务已进入 OTA worker；ESP_ERR_NOT_FOUND 表示当前没有待启动任务。
 */
esp_err_t gw_ota_manager_start_pending_upgrade(void);

/**
 * @brief 取消已暂存但尚未启动的 OTA 任务。
 *
 * @param reason 稳定原因码，可为空。
 */
void gw_ota_manager_cancel_pending_upgrade(const char *reason);

/**
 * @brief 查询 OTA 管理状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_ota_manager_get_status(gw_ota_status_t *out);

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
