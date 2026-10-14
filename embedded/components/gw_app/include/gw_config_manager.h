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
 * @brief 配置更新阶段。
 */
typedef enum {
    GW_CONFIG_STAGE_IDLE = 0,
    GW_CONFIG_STAGE_ACCEPTED,
    GW_CONFIG_STAGE_WAIT_NETWORK,
    GW_CONFIG_STAGE_DOWNLOADING_MANIFEST,
    GW_CONFIG_STAGE_VALIDATING_MANIFEST,
    GW_CONFIG_STAGE_DOWNLOADING_FILES,
    GW_CONFIG_STAGE_VALIDATING_SLOT,
    GW_CONFIG_STAGE_SWITCHING_SLOT,
    GW_CONFIG_STAGE_SUCCESS,
    GW_CONFIG_STAGE_FAILED,
} gw_config_stage_t;

/**
 * @brief 配置更新请求。
 */
typedef struct {
    char request_id[65];       /*!< MQTT request_id */
    uint32_t sequence;         /*!< 目标配置流水号 */
    char manifest_url[512];    /*!< manifest.jsonl 下载 URL */
    char manifest_sha256[65];  /*!< manifest SHA-256 小写或大写十六进制 */
    uint32_t manifest_size;    /*!< manifest 字节大小 */
    bool apply_after_download; /*!< 下载校验通过后是否应用；当前按 true 处理 */
} gw_config_update_request_t;

/**
 * @brief 当前生效配置摘要。
 */
typedef struct {
    char active_slot[8];       /*!< cfg_a 或 cfg_b */
    uint32_t seq_id;           /*!< 当前生效配置流水号 */
    char manifest_sha256[65];  /*!< 当前生效 manifest SHA-256，可为空 */
    bool loaded_from_nvs;      /*!< 是否从 NVS 读到有效状态 */
} gw_config_active_t;

/**
 * @brief 自检配置文件描述。
 *
 * 该结构仅用于固件自检或工厂测试，生产协议仍然只能通过 HTTP/HTTPS 下载配置包。
 */
typedef struct {
    const char *name;          /*!< manifest 中的相对路径 */
    const void *data;          /*!< 文件内容 */
    size_t size;               /*!< 文件大小 */
} gw_config_test_file_t;

/**
 * @brief 配置管理模块状态。
 */
typedef struct {
    bool initialized;                  /*!< 模块是否已初始化 */
    bool worker_started;               /*!< worker 是否已启动 */
    gw_config_stage_t stage;           /*!< 当前或最近阶段 */
    gw_config_active_t active;         /*!< 当前生效配置摘要 */
    uint32_t accepted_count;           /*!< 已接收更新任务数 */
    uint32_t failed_count;             /*!< 失败任务数 */
    uint32_t success_count;            /*!< 成功任务数 */
    uint32_t final_publish_failed;     /*!< 最终 MQTT 应答发布失败次数 */
    char running_request_id[65];       /*!< 当前任务 request_id */
    uint32_t running_sequence;         /*!< 当前任务目标 sequence */
    char last_reason[64];              /*!< 最近失败原因或 accepted */
    int32_t last_code;                 /*!< 最近业务 code */
    int32_t last_http_status;          /*!< 最近 HTTP 状态码 */
    int32_t last_esp_err;              /*!< 最近 ESP-IDF 错误码 */
} gw_config_status_t;

/**
 * @brief 初始化配置管理模块并启动配置 worker。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_config_manager_init(void);

/**
 * @brief 提交配置整包更新任务。
 *
 * 该接口只做参数校验、版本递增校验、互斥任务占用和暂存；实际 HTTP 下载必须在
 * 协议层成功发布 accepted 应答后，再调用 gw_config_manager_start_pending_update()
 * 进入 config worker。失败原因通过 out_result 返回给协议层生成 MQTT 应答。
 *
 * @param request 配置更新请求。
 * @param out_result 输出统一结果，不能为空。
 * @return ESP_OK 表示任务已接收并等待 accepted 应答发布；其他返回值表示应立即拒绝。
 */
esp_err_t gw_config_manager_submit_update(const gw_config_update_request_t *request,
                                          gw_result_t *out_result);

/**
 * @brief 判断是否存在已接收但尚未启动 worker 的配置更新。
 *
 * 仅供协议层决定 accepted 应答是否需要等待 MQTT PUBLISHED 事件。
 *
 * @return true 表示存在待启动配置更新。
 */
bool gw_config_manager_has_pending_update(void);

/**
 * @brief 启动已暂存的配置更新任务。
 *
 * 该接口只能在 config.update 的 accepted MQTT 应答成功进入发送队列后调用，
 * 用于保证最终 success/failed 应答不会早于 accepted 应答入队。
 *
 * @return ESP_OK 表示任务已进入 config worker；ESP_ERR_NOT_FOUND 表示当前没有待启动任务。
 */
esp_err_t gw_config_manager_start_pending_update(void);

/**
 * @brief 取消已暂存但尚未启动的配置更新任务。
 *
 * 当 accepted 应答发布失败时调用，避免任务在服务端没有收到 accepted 的情况下继续执行。
 *
 * @param reason 稳定原因码，可为空。
 */
void gw_config_manager_cancel_pending_update(const char *reason);

/**
 * @brief 获取当前生效配置摘要。
 *
 * @param out 输出摘要，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_config_manager_get_active(gw_config_active_t *out);

/**
 * @brief 获取配置管理状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_config_manager_get_status(gw_config_status_t *out);

/**
 * @brief 校验当前 active 配置槽完整性。
 *
 * 该接口会读取 active 槽的 manifest.jsonl，并按 NVS 中记录的 seq_id 和
 * manifest_sha256 校验 manifest 及其声明的所有文件内容。
 *
 * @param out_result 输出统一结果，不能为空。
 * @return ESP_OK 表示 active 槽完整有效；其他值表示缺失或校验失败。
 */
esp_err_t gw_config_manager_validate_active(gw_result_t *out_result);

/**
 * @brief 安装内存中的配置包，用于硬件自检验证 cfg 双区切换。
 *
 * 该接口不走 MQTT/HTTP，不触发重启，也不放宽对外协议 URL 规则；它复用正式路径中的
 * manifest 解析、临时文件提交、槽位完整校验和 NVS active 状态写入。
 *
 * @param sequence 目标配置流水号，必须大于当前生效流水号。
 * @param files 配置文件数组，必须包含 config.json。
 * @param file_count 配置文件数量。
 * @param out_result 输出统一结果，不能为空。
 * @return ESP_OK 表示安装成功；其他值表示安装失败。
 */
esp_err_t gw_config_manager_install_test_bundle(uint32_t sequence,
                                                const gw_config_test_file_t *files,
                                                size_t file_count,
                                                gw_result_t *out_result);

/**
 * @brief 配置更新阶段转字符串。
 *
 * @param stage 阶段枚举。
 * @return 静态字符串。
 */
const char *gw_config_stage_to_string(gw_config_stage_t stage);

#ifdef __cplusplus
}
#endif
