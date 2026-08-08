#ifndef CONFIG_VALIDATE_TEST_FREERTOS_H
#define CONFIG_VALIDATE_TEST_FREERTOS_H

#include <stddef.h>

void *pvPortMalloc(size_t size);
void vPortFree(void *pointer);

#endif
