#ifndef TEST_ASYNC_CONTEXT_FREERTOS_H
#define TEST_ASYNC_CONTEXT_FREERTOS_H
#include <stdbool.h>
#include "FreeRTOS.h"
#define ASYNC_CONTEXT_DEFAULT_FREERTOS_TASK_STACK_SIZE 32u
typedef struct { int core; } async_context_freertos_t;
typedef struct { StackType_t *task_stack; } async_context_freertos_config_t;
async_context_freertos_config_t async_context_freertos_default_config(void);
bool async_context_freertos_init(async_context_freertos_t *, async_context_freertos_config_t *);
#endif
