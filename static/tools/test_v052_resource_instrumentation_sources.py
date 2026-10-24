#!/usr/bin/env python3
"""Fail-closed source checks for the v0.5.2 native T2 vertical."""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(source: str, signature: str, next_marker: str) -> str:
    start = source.index(signature)
    end = source.index(next_marker, start)
    return source[start:end]


def main() -> int:
    config = read("port/FreeRTOS-Kernel/inc/FreeRTOSConfig.h")
    diag_header = read("services/include/services/diag_service.h")
    diag_source = read("services/src/diag_service.c")
    allocator = read("port/psram/psram_allocator.c")
    ua_allocator = read("port/FreeRTOS-Kernel/inc/ua_port_alloc/ua_port_alloc.c")
    lwip_provider = read("port/lwip/lwip_psram_heap.c")
    app = read("app/basic_profile/main.c")
    lifecycle = read("opcua_model/src/model_core_lifecycle.c")
    history = read("opcua_model/src/model_history.c")

    require("#define DIAG_SERVICE_MAX_SNAPSHOT_TASKS 16u" in diag_header,
            "task snapshot bound changed")
    require("#define DIAG_SERVICE_SNAPSHOT_PERIOD_MS 1000u" in diag_header,
            "snapshot cadence changed")
    require("traceTASK_CREATE(pxNewTCB)" in config and
            "traceTASK_DELETE(pxTaskToDelete)" in config,
            "FreeRTOS lifecycle hooks are not wired")
    require("uxTCBNumber" in config,
            "task hook does not capture the reuse-resistant task number")
    require("vTaskGetInfo(" not in diag_source,
            "diagnostics reintroduced a retained-handle TCB dereference")
    require("sampled_count == snapshot->task_count" in diag_source,
            "uxTaskGetSystemState overflow is not fail-closed")
    require("DIAG_SERVICE_QUALITY_UNCERTAIN_LAST_USABLE" in diag_source and
            "DIAG_SERVICE_QUALITY_BAD_NO_DATA" in diag_source,
            "unavailable/stale task qualities are missing")
    require("psram_calloc(1u, sizeof(*g_snapshot_storage))" in diag_source and
            "pvPortMalloc(sizeof(*g_snapshot_storage))" in diag_source,
            "bounded snapshot storage lacks PSRAM plus deterministic SRAM fallback")
    require("model_sample_resource_diagnostics(server);" in lifecycle and
            "DIAG_SERVICE_SNAPSHOT_PERIOD_MS" in lifecycle,
            "the one-second sampler is not driven by OpcUaTask")

    require("StaticSemaphore_t g_psram_mutex_storage" in allocator and
            "PSRAM_LOCK_TIMEOUT_TICKS pdMS_TO_TICKS(10u)" in allocator,
            "PSRAM allocator mutex is not static and bounded")
    require("g_pending_free_list" in allocator and
            "psram_drain_pending_frees" in allocator,
            "bounded intrusive deferred-free path is missing")
    require("PSRAM_BLOCK_REALLOC_RESERVED" in allocator and
            "block->pending_free = PSRAM_BLOCK_PENDING_FREE" in allocator,
            "free/realloc ownership is not reserved before mutation")
    require("psram_malloc_internal(size, old_requested_size)" in allocator and
            "psram_free_internal(ptr, false, false, true);" in allocator,
            "moved realloc is double-counted as a public free")
    require("size > (SIZE_MAX - (PSRAM_ALIGNMENT - 1u))" in allocator,
            "aligned-size overflow guard is missing")
    require("psram_free(header);\n        return;" in ua_allocator,
            "UA_free touches a PSRAM header after allocator ownership transfer")
    require("out_stats->supported = false" in lwip_provider and
            "never fabricate zero" in lwip_provider,
            "unavailable lwIP fragmentation is presented as measured data")

    malloc_hook = function_body(app,
                                "void vApplicationMallocFailedHook(void)",
                                "/**\n * @brief FreeRTOS static-allocation callback")
    require("diag_service_record_heap_failure" in malloc_hook,
            "malloc hook does not record the failure")
    require("diag_service_record_heap_failure_from_isr" in malloc_hook and
            "taskENTER_CRITICAL_FROM_ISR" in diag_source,
            "malloc-failure ISR path does not use an ISR-safe recorder")
    for forbidden in ("app_fault_loop", "app_stop_watchdog_kicks", "printf("):
        require(forbidden not in malloc_hook,
                f"generic malloc hook still owns terminal/logging policy: {forbidden}")

    require("g_task_ingress[DIAG_SERVICE_MAX_SNAPSHOT_TASKS]" in diag_source and
            "diag_find_live_duplicate" in diag_source,
            "bounded duplicate-name recovery tracking is missing")
    require("diag_now - last_diag_snapshot_tick" in lifecycle,
            "diagnostic sampling cadence is not tick-wrap safe")
    require("UA_STATUSCODE_BADNODATA" in history and
            "diag_service_snapshot_is_available" in history,
            "snapshot-allocation failure is not exposed as unavailable data")

    create_task = function_body(app,
                                "static bool create_task(",
                                "/**\n * @brief Resolves the effective watchdog timeout")
    require("DIAG_FAULT_CODE_MALLOC_FAILED" in create_task and
            "app_fault_loop(true, 250u)" in create_task,
            "required worker-task allocation failure is not fatal")

    print("v0.5.2 native resource instrumentation source suite: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, ValueError) as exc:
        print(f"v0.5.2 native resource instrumentation source suite: FAIL: {exc}",
              file=sys.stderr)
        raise SystemExit(1)
