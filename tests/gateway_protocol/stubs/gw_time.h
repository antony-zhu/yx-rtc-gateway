#pragma once
#include <stdint.h>
typedef struct { uint64_t utc_ms; } gw_time_point_t;
gw_time_point_t gw_time_now(void);
