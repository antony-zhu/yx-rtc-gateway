#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_4G_TEXT_SHORT_LEN 32U
#define GW_4G_TEXT_LONG_LEN  96U

/**
 * @brief 4G USB ECM 启动阶段。
 *
 * 该枚举只描述驱动内部阶段，平台和脚本侧仍只看到 public profile `4g`。
 */
typedef enum {
    GW_4G_ECM_STAGE_IDLE = 0,       /*!< 未启动 */
    GW_4G_ECM_STAGE_STARTING,       /*!< 正在准备 GPIO、USB Host 和 CDC/ECM */
    GW_4G_ECM_STAGE_WAIT_USB,       /*!< 正在等待 4G USB 枚举 */
    GW_4G_ECM_STAGE_PROBING,        /*!< 正在通过 AT 检查模组 */
    GW_4G_ECM_STAGE_SIM_CHECKING,   /*!< 正在检查 SIM */
    GW_4G_ECM_STAGE_REGISTERING,    /*!< 正在等待运营商网络注册 */
    GW_4G_ECM_STAGE_CONFIGURING,    /*!< 正在设置 APN 并进入 ECM 模式 */
    GW_4G_ECM_STAGE_GETTING_IP,     /*!< 正在等待 ECM 获取 IP */
    GW_4G_ECM_STAGE_READY,          /*!< 已获取 IP */
    GW_4G_ECM_STAGE_FAILED,         /*!< 当前一轮启动失败 */
    GW_4G_ECM_STAGE_STOPPED,        /*!< 已停止 */
} gw_4g_ecm_stage_t;

/**
 * @brief 4G 启动阶段回调。
 *
 * 回调运行在网络 worker 上下文中，只允许短耗时地更新状态或投递事件。
 */
typedef void (*gw_4g_ecm_stage_cb_t)(gw_4g_ecm_stage_t stage, const char *reason, void *user_ctx);

/**
 * @brief 4G USB ECM 启动参数。
 */
typedef struct {
    const char *apn;                 /*!< APN，可为空字符串 */
    uint32_t timeout_ms;             /*!< 本轮启动等待上限，0 表示使用默认值 */
    gw_4g_ecm_stage_cb_t stage_cb;   /*!< 阶段回调，可为空 */
    void *stage_ctx;                 /*!< 阶段回调上下文 */
} gw_4g_ecm_start_config_t;

/**
 * @brief 4G 模组信息缓存。
 *
 * 该结构由系统 AT 通道刷新。启动阶段 AT 通道归系统独占，脚本不能直接抢占。
 */
typedef struct {
    char manufacturer[GW_4G_TEXT_LONG_LEN];  /*!< 模组厂商 */
    char model[GW_4G_TEXT_LONG_LEN];         /*!< 模组型号 */
    char revision[GW_4G_TEXT_LONG_LEN];      /*!< 模组固件版本 */
    char imei[GW_4G_TEXT_SHORT_LEN];         /*!< IMEI */
    char iccid[GW_4G_TEXT_LONG_LEN];         /*!< SIM ICCID */
    char imsi[GW_4G_TEXT_SHORT_LEN];         /*!< IMSI */
    char operator_name[GW_4G_TEXT_LONG_LEN]; /*!< 运营商名称 */
    int rssi;                                /*!< CSQ RSSI，99 表示未知 */
    int ber;                                 /*!< CSQ BER，99 表示未知 */
    bool registered;                         /*!< 是否已注册运营商网络 */
    int64_t updated_at_ms;                   /*!< 最近刷新时间，单调毫秒 */
} gw_4g_ecm_modem_info_t;

/**
 * @brief 4G USB ECM 状态快照。
 */
typedef struct {
    bool initialized;                         /*!< 驱动是否已初始化 */
    bool ready;                               /*!< 是否已获取 IP */
    bool at_busy;                             /*!< AT 通道是否正被系统占用 */
    gw_4g_ecm_stage_t stage;                  /*!< 当前阶段 */
    char apn[65];                             /*!< 当前 APN */
    char ip[GW_4G_TEXT_SHORT_LEN];            /*!< 当前 IP */
    char netmask[GW_4G_TEXT_SHORT_LEN];       /*!< 子网掩码 */
    char gateway[GW_4G_TEXT_SHORT_LEN];       /*!< 网关 */
    char dns0[GW_4G_TEXT_SHORT_LEN];          /*!< DNS 0 */
    char dns1[GW_4G_TEXT_SHORT_LEN];          /*!< DNS 1 */
    char reason[GW_4G_TEXT_LONG_LEN];         /*!< 最近状态原因，已脱敏 */
    gw_4g_ecm_modem_info_t modem;             /*!< 模组信息缓存 */
} gw_4g_ecm_status_t;

/**
 * @brief 初始化 4G USB ECM 驱动。
 *
 * 初始化事件组、互斥锁和内部状态，但不分配 USB task，也不启动网络。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_4g_ecm_init(void);

/**
 * @brief 提前准备 4G GPIO、USB Host 和 CDC 控制通道。
 *
 * 应在 BLE 初始化完成后、其他大任务创建前调用，以提前保留 CDC 所需的连续
 * 片内任务栈。该函数不打开 4G 电源、不拨号；失败时会回滚已创建资源，后续
 * `gw_4g_ecm_start()` 仍可重试。
 *
 * @return ESP_OK 表示 USB/CDC 资源已经准备完成。
 */
esp_err_t gw_4g_ecm_prepare_usb(void);

/**
 * @brief 阻塞启动一轮 4G USB ECM 联网。
 *
 * 该函数应只在 `net_worker` 中调用。成功时已获取 IP，失败时返回 ESP-IDF
 * 错误码并保留最近失败原因。
 *
 * @param config 启动参数，可为空。
 * @return ESP_OK 表示网络已 ready。
 */
esp_err_t gw_4g_ecm_start(const gw_4g_ecm_start_config_t *config);

/**
 * @brief 停止当前 4G USB ECM 网络。
 *
 * 停止 ECM 数据链路并关闭 4G 电源控制脚，不清除 NVS 参数。
 *
 * @return ESP_OK 表示停止流程已执行。
 */
esp_err_t gw_4g_ecm_stop(void);

/**
 * @brief 获取 4G USB ECM 状态快照。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_4g_ecm_get_status(gw_4g_ecm_status_t *out);

/**
 * @brief 刷新 4G 模组缓存信息。
 *
 * 只能在 4G ready 后调用，内部串行占用 AT 通道。
 *
 * @param timeout_ms 单条 AT 命令超时，0 表示默认 2000 ms。
 * @return ESP_OK 表示刷新流程执行完成；单个字段失败不会导致整体失败。
 */
esp_err_t gw_4g_ecm_refresh_modem_info(uint32_t timeout_ms);

/**
 * @brief 发送一条系统 AT 指令。
 *
 * 当前仅供系统诊断和后续受控 API 使用，不向脚本直接暴露。
 *
 * @param command AT 指令，不需要带 CRLF；不能为空字符串。
 * @param timeout_ms 超时，单位毫秒。
 * @param out_response 输出原始响应，可为空。
 * @param response_size 输出缓冲区大小。
 * @return ESP_OK 表示响应中包含 OK。
 */
esp_err_t gw_4g_ecm_at(const char *command,
                       uint32_t timeout_ms,
                       char *out_response,
                       size_t response_size);

/**
 * @brief 获取阶段名称。
 *
 * @param stage 4G 内部阶段。
 * @return 静态字符串。
 */
const char *gw_4g_ecm_stage_to_string(gw_4g_ecm_stage_t stage);

#ifdef __cplusplus
}
#endif
