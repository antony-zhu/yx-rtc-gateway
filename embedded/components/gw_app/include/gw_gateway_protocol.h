#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the gateway-only timer/OTA command worker after transport initialization. */
esp_err_t gw_gateway_protocol_init(void);

/** Serialize an event using a new ID. The caller retains ownership of pd.
 * wait_for_attempt waits for one QoS 0 publication attempt or disposal, never PUBACK.
 */
esp_err_t gw_gateway_publish_event(const char *mt,
                                   const char *dt,
                                   const cJSON *pd,
                                   bool wait_for_attempt);

#ifdef __cplusplus
}
#endif
