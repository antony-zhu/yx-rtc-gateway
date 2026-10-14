#pragma once
#include "esp_err.h"
#include "gw_state.h"
typedef struct {
    gw_network_status_t public_status;
} gw_net_status_snapshot_t;
esp_err_t gw_net_get_status(gw_net_status_snapshot_t *out);
