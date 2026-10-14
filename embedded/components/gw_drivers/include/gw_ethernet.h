#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_ETH_STAGE_IDLE = 0,
    GW_ETH_STAGE_STARTING,
    GW_ETH_STAGE_WAIT_USB,
    GW_ETH_STAGE_GETTING_IP,
    GW_ETH_STAGE_READY,
    GW_ETH_STAGE_DISCONNECTED,
    GW_ETH_STAGE_FAILED,
    GW_ETH_STAGE_STOPPED,
} gw_ethernet_stage_t;

typedef struct {
    bool initialized;
    bool ready;
    gw_ethernet_stage_t stage;
    char ip[32];
    char netmask[32];
    char gateway[32];
    char dns0[32];
    char dns1[32];
    char reason[64];
} gw_ethernet_status_t;

typedef void (*gw_ethernet_stage_cb_t)(gw_ethernet_stage_t stage,
                                       const char *reason,
                                       void *user_ctx);

typedef struct {
    uint32_t timeout_ms;
    gw_ethernet_stage_cb_t stage_cb;
    void *stage_ctx;
} gw_ethernet_start_config_t;

esp_err_t gw_ethernet_init(void);
esp_err_t gw_ethernet_prepare_usb(void);
esp_err_t gw_ethernet_start(const gw_ethernet_start_config_t *config);
esp_err_t gw_ethernet_stop(void);
esp_err_t gw_ethernet_get_status(gw_ethernet_status_t *out);
const char *gw_ethernet_stage_to_string(gw_ethernet_stage_t stage);

#ifdef __cplusplus
}
#endif
