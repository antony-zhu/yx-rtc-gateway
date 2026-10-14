#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_FS_CFGFS_LABEL       "cfgfs"
#define GW_FS_DATAFS_LABEL      "datafs"
#define GW_FS_LOGFS_LABEL       "logfs"

#define GW_FS_CFGFS_BASE_PATH   "/cfgfs"
#define GW_FS_DATAFS_BASE_PATH  "/datafs"
#define GW_FS_LOGFS_BASE_PATH   "/logfs"

#define GW_FS_MAX_PATH_LEN      256U

/**
 * @brief LittleFS 分区标识。
 */
typedef enum {
    GW_FS_PARTITION_CFGFS = 0,
    GW_FS_PARTITION_DATAFS,
    GW_FS_PARTITION_LOGFS,
} gw_fs_partition_t;

/**
 * @brief 文件系统初始化模式。
 */
typedef enum {
    GW_FS_INIT_MODE_RUNTIME = 0,  /*!< 正常运行启动，挂载失败先重试，仍失败再格式化 */
    GW_FS_INIT_MODE_FACTORY,      /*!< 出厂流程，可按选项显式格式化分区 */
} gw_fs_init_mode_t;

/**
 * @brief 文件系统初始化选项。
 */
typedef struct {
    gw_fs_init_mode_t mode;       /*!< 初始化模式 */
    bool force_format_cfgfs;      /*!< 挂载前强制格式化 cfgfs，仅出厂/维护流程使用 */
    bool force_format_datafs;     /*!< 挂载前强制格式化 datafs，仅出厂/维护流程使用 */
    bool force_format_logfs;      /*!< 挂载前强制格式化 logfs，仅出厂/维护流程使用 */
} gw_fs_init_options_t;

/**
 * @brief 单个文件系统分区状态。
 */
typedef struct {
    bool mounted;              /*!< 是否挂载成功 */
    bool formatted;            /*!< 本次初始化是否执行过格式化 */
    esp_err_t last_error;      /*!< 最近挂载或查询错误 */
    esp_err_t format_error;    /*!< 最近格式化错误；未格式化时为 ESP_OK */
    size_t total_bytes;        /*!< 分区总容量，未知时为 0 */
    size_t used_bytes;         /*!< 已使用容量，未知时为 0 */
    uint8_t mount_attempts;    /*!< 本次初始化的挂载尝试次数 */
} gw_fs_partition_status_t;

/**
 * @brief 文件系统整体状态。
 */
typedef struct {
    gw_fs_partition_status_t cfgfs;
    gw_fs_partition_status_t datafs;
    gw_fs_partition_status_t logfs;
} gw_fs_status_t;

/**
 * @brief 配置双区基础扫描结果。
 */
typedef struct {
    bool cfg_a_manifest_exists;
    bool cfg_a_config_exists;
    bool cfg_a_basic_valid;
    bool cfg_b_manifest_exists;
    bool cfg_b_config_exists;
    bool cfg_b_basic_valid;
} gw_cfg_slot_scan_t;

/**
 * @brief 初始化并挂载 cfgfs、datafs、logfs。
 *
 * 正常运行启动时，首次挂载失败后会再重试 GW_FS_MOUNT_RETRY_COUNT 次；
 * 仍失败则格式化该分区并重新挂载一次。出厂或维护流程如需预先清空
 * 分区，应调用 gw_fs_init_with_options 并显式设置 force_format_*。
 *
 * @return ESP_OK 表示挂载流程已执行；具体分区状态通过 gw_fs_get_status 查询。
 */
esp_err_t gw_fs_init(void);

/**
 * @brief 按指定选项初始化并挂载 cfgfs、datafs、logfs。
 *
 * 出厂模式允许调用方通过 force_format_* 先格式化对应分区；正常运行
 * 模式下不应设置强制格式化选项。
 *
 * @param options 初始化选项；为空时等同 gw_fs_init。
 * @return ESP_OK 表示挂载流程已执行；具体分区状态通过 gw_fs_get_status 查询。
 */
esp_err_t gw_fs_init_with_options(const gw_fs_init_options_t *options);

/**
 * @brief 查询文件系统状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_fs_get_status(gw_fs_status_t *out);

/**
 * @brief 刷新并读取指定 LittleFS 分区容量状态。
 *
 * 该接口会查询底层 LittleFS 实时用量并更新模块内缓存。业务写入配额、
 * 日志清理和诊断查询如需当前用量，可调用此接口，避免上层直接依赖
 * LittleFS 组件头文件。
 *
 * @param partition 分区枚举。
 * @param out 输出分区状态，可为空；为空时只更新内部缓存。
 * @return ESP_OK 表示查询成功。
 */
esp_err_t gw_fs_refresh_partition(gw_fs_partition_t partition,
                                  gw_fs_partition_status_t *out);

/**
 * @brief 将业务路径规范化到 datafs。
 *
 * 输入使用业务相对路径，例如 /a 或 /dir/../a。输出同时提供 VFS
 * 路径和诊断显示路径：VFS 路径用于 fopen/stat，显示路径用于日志和协议诊断。
 *
 * @param user_path 业务路径，必须以 / 开头。
 * @param out_vfs_path 输出 VFS 路径，例如 /datafs/a，可为空。
 * @param out_vfs_size VFS 输出缓冲区大小。
 * @param out_display_path 输出显示路径，例如 datafs:/a，可为空。
 * @param out_display_size 显示路径输出缓冲区大小。
 * @return ESP_OK 表示路径合法；ESP_ERR_INVALID_ARG 表示路径穿透或格式非法。
 */
esp_err_t gw_fs_resolve_data_path(const char *user_path,
                                  char *out_vfs_path,
                                  size_t out_vfs_size,
                                  char *out_display_path,
                                  size_t out_display_size);

/**
 * @brief 初步扫描 cfgfs:/cfg_a 和 cfgfs:/cfg_b。
 *
 * P2 阶段只检查 manifest.jsonl、config.json 是否存在，不解析
 * manifest 内容。
 *
 * @param out 输出扫描结果，不能为空。
 * @return ESP_OK 表示扫描完成；cfgfs 未挂载时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t gw_fs_scan_config_slots(gw_cfg_slot_scan_t *out);

/**
 * @brief 获取分区显示名称。
 *
 * @param partition 分区枚举。
 * @return 静态字符串。
 */
const char *gw_fs_partition_name(gw_fs_partition_t partition);

#ifdef __cplusplus
}
#endif
