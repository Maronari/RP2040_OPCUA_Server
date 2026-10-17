#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
#define configTICK_RATE_HZ 1000u
typedef uint32_t TickType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t StackType_t;
void *pvPortMalloc(size_t size);
void vPortFree(void *pointer);
#endif
