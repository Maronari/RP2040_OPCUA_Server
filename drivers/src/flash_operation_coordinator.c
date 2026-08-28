/**
 * @file
 * @brief Single serialization point over on-chip Flash erase/program on
 *        RP2350 -- the only place in the firmware that calls
 *        `flash_range_program`/`flash_range_erase`.
 *
 * Responsibilities:
 * - Serialize every erase/program call across the whole firmware behind one
 *   FreeRTOS mutex, with same-task re-entrant acquisition detected and
 *   rejected rather than deadlocking.
 * - Run the actual hardware call inside `flash_safe_execute()` (pico_flash),
 *   which suspends the other core and disables interrupts for the duration
 *   since XIP Flash is unavailable while erasing/programming.
 * - Reject any program-page request whose source buffer overlaps the
 *   currently executing firmware image (that memory becomes unreadable
 *   mid-program while XIP is suspended).
 *
 * Ownership: g_flash_operation_mutex/g_flash_operation_owner are
 * process-lifetime globals; callers (config_store_service,
 * security_service, security_state_service, maintenance_journal_backend,
 * history_flash_backend) never touch Flash registers directly.
 *
 * Concurrency: the mutex is the sole synchronization primitive in this
 * module; it is created once by flash_operation_coordinator_init() and must
 * exist before any Flash-writing service initializes.
 */
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

/** @brief Parameter block passed through flash_safe_execute() to the erase/program callbacks. */
typedef struct
{
    uint32_t offset;
    const uint8_t *page;
} flash_operation_context_t;

static StaticSemaphore_t g_flash_operation_mutex_storage;
/** Coordinator mutex; NULL until flash_operation_coordinator_init() runs. */
static SemaphoreHandle_t g_flash_operation_mutex;
/** Task currently holding g_flash_operation_mutex, or NULL; used to detect same-task re-entry. */
static TaskHandle_t g_flash_operation_owner;

extern char __flash_binary_start;
extern char __flash_binary_end;

/** @brief True if the FLASH_PAGE_SIZE region starting at @p page overlaps the executing firmware image. */
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

/**
 * @brief flash_safe_execute() callback: erases one FLASH_SECTOR_SIZE sector.
 *
 * Runs with the second core suspended and interrupts disabled (XIP
 * unavailable); marked `__not_in_flash_func` so its own code stays resident
 * in SRAM/RAM-mapped Flash, not the region being erased.
 */
static void __not_in_flash_func(flash_operation_erase_callback)(void *parameter)
{
    const flash_operation_context_t *context = (const flash_operation_context_t *)parameter;
    flash_range_erase(context->offset, FLASH_SECTOR_SIZE);
}

/**
 * @brief flash_safe_execute() callback: programs one FLASH_PAGE_SIZE page.
 *
 * Same XIP-suspended execution constraints as flash_operation_erase_callback().
 */
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

/**
 * @brief Acquire the coordinator mutex, detecting same-task re-entrancy first.
 * @return FLASH_OPERATION_NOT_INITIALIZED if the mutex was never created;
 *         FLASH_OPERATION_NESTED if the calling task already owns it;
 *         FLASH_OPERATION_TIMEOUT if it could not be taken within
 *         FLASH_OPERATION_LOCK_TIMEOUT_MS; FLASH_OPERATION_OK on success
 *         (records the caller as owner).
 */
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

/** @brief Clear the recorded owner and release the coordinator mutex. */
static void flash_operation_release(void)
{
    g_flash_operation_owner = NULL;
    (void)xSemaphoreGive(g_flash_operation_mutex);
}

/**
 * @brief Acquire the mutex, run @p callback inside flash_safe_execute(), then release.
 * @return The acquisition failure code from flash_operation_acquire() if
 *         that fails; FLASH_OPERATION_SAFE_EXEC_FAILED if
 *         `flash_safe_execute()` itself reports failure; FLASH_OPERATION_OK
 *         otherwise. Always releases the mutex if it was acquired.
 */
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
