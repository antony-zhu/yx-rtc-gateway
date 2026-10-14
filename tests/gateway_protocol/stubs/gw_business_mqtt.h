#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { char *topic; uint8_t *payload; size_t payload_len; bool retained; } gw_business_mqtt_owned_message_t;
typedef void (*gw_business_mqtt_gateway_done_fn)(void *,esp_err_t);
esp_err_t gw_business_mqtt_publish_gateway(const void *,size_t,gw_business_mqtt_gateway_done_fn,void *);
esp_err_t gw_business_mqtt_recv_gateway(gw_business_mqtt_owned_message_t *,uint32_t);
void gw_business_mqtt_message_release(gw_business_mqtt_owned_message_t *);
