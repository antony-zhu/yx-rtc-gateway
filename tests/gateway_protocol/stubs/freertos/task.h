#pragma once
#include <stddef.h>
typedef void *TaskHandle_t;
static inline int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out)
{ (void)fn;(void)name;(void)stack;(void)arg;(void)priority;*out=(void *)1;return 1; }
