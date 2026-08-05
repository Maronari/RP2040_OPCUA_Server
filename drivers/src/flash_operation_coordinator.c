#include "drivers/flash_operation_coordinator.h"

#include <stdbool.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/platform.h"

#define FLASH_OPERATION_LOCK_TIMEOUT_MS 1000u
#define FLASH_OPERATION_SAFE_TIMEOUT_MS 1000u

typedef struct
{
    uint32_t offset;
    const uint8_t *page;
} flash_operation_context_t;

static StaticSemaphore_t g_flash_operation_mutex_storage;
static SemaphoreHandle_t g_flash_operation_mutex;
static TaskHandle_t g_flash_operation_owner;

extern char __flash_binary_start;
extern char __flash_binary_end;

static bool flash_operation_source_is_executable_flash(const uint8_t *page)
{
    const uintptr_t page_start = (uintptr_t)page;
    const uintptr_t page_end = page_start + FLASH_PAGE_SIZE;
    const uintptr_t binary_start = (uintptr_t)&__flash_binary_start;
    const uintptr_t binary_end = (uintptr_t)&__flash_binary_end;

    return page_end >= page_start &&
           page_start < binary_end &&
           page_end > binary_start;
}

static void __not_in_flash_func(flash_operation_erase_callback)(void *parameter)
{
    const flash_operation_context_t *context = (const flash_operation_context_t *)parameter;
    flash_range_erase(context->offset, FLASH_SECTOR_SIZE);
}

static void __not_in_flash_func(flash_operation_program_callback)(void *parameter)
{
    const flash_operation_context_t *context = (const flash_operation_context_t *)parameter;
    flash_range_program(context->offset, context->page, FLASH_PAGE_SIZE);
}

bool flash_operation_coordinator_init(void)
{
    if (g_flash_operation_mutex != NULL)
    {
        return true;
    }

    g_flash_operation_owner = NULL;
    g_flash_operation_mutex = xSemaphoreCreateMutexStatic(&g_flash_operation_mutex_storage);
    return g_flash_operation_mutex != NULL;
}

static flash_operation_result_t flash_operation_acquire(void)
{
    const TaskHandle_t caller = xTaskGetCurrentTaskHandle();

    if (g_flash_operation_mutex == NULL)
    {
        return FLASH_OPERATION_NOT_INITIALIZED;
    }
    if (g_flash_operation_owner == caller && caller != NULL)
    {
        return FLASH_OPERATION_NESTED;
    }
    if (xSemaphoreTake(g_flash_operation_mutex,
                       pdMS_TO_TICKS(FLASH_OPERATION_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return FLASH_OPERATION_TIMEOUT;
    }

    g_flash_operation_owner = caller;
    return FLASH_OPERATION_OK;
}

static void flash_operation_release(void)
{
    g_flash_operation_owner = NULL;
    (void)xSemaphoreGive(g_flash_operation_mutex);
}

static flash_operation_result_t flash_operation_execute(void (*callback)(void *),
                                                        flash_operation_context_t *context)
{
    flash_operation_result_t result = flash_operation_acquire();
    if (result != FLASH_OPERATION_OK)
    {
        return result;
    }

    if (flash_safe_execute(callback, context, FLASH_OPERATION_SAFE_TIMEOUT_MS) != PICO_OK)
    {
        result = FLASH_OPERATION_SAFE_EXEC_FAILED;
    }
    flash_operation_release();
    return result;
}

flash_operation_result_t flash_operation_erase_sector(uint32_t flash_offset)
{
    flash_operation_context_t context = {.offset = flash_offset, .page = NULL};

    if ((flash_offset % FLASH_SECTOR_SIZE) != 0u ||
        flash_offset > (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE))
    {
        return FLASH_OPERATION_INVALID_ARGUMENT;
    }
    return flash_operation_execute(flash_operation_erase_callback, &context);
}

flash_operation_result_t flash_operation_program_page(uint32_t flash_offset, const uint8_t *page)
{
    flash_operation_context_t context = {.offset = flash_offset, .page = page};

    if (page == NULL || flash_operation_source_is_executable_flash(page) ||
        (flash_offset % FLASH_PAGE_SIZE) != 0u ||
        flash_offset > (PICO_FLASH_SIZE_BYTES - FLASH_PAGE_SIZE))
    {
        return FLASH_OPERATION_INVALID_ARGUMENT;
    }
    return flash_operation_execute(flash_operation_program_callback, &context);
}
