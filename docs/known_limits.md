# Known resource limits

This document records the limits compiled into the RP2350 + PSRAM + W5500
reference firmware. They are fixed firmware build settings, not remotely
configurable values, unless stated otherwise. Requests at a limit are rejected
with the subsystem's normal resource/status result; they never expand a queue,
session table, or staging buffer at runtime.

| Resource | Exact limit | Control and source | Memory / failure behavior | Configurable |
| --- | ---: | --- | --- | --- |
| Configured and runtime channels | 16 (reference default; supported compile-time range 1вЂ“16) | `PLATFORM_CHANNEL_LIMIT`, [services/include/services/platform_limits.h](../services/include/services/platform_limits.h) | The same bound sizes persisted channel definitions, manager-owned runtime slots, OPC UA adapter contexts, PubSub writer state and diagnostics. The reference profile allocates 30,272 bytes of bounded configuration/registry/rollback-bank state from PSRAM and fails initialization if that storage is unavailable; no internal-SRAM fallback is used. Registration rejects invalid metadata, duplicate IDs, stale generations and capacity. A frozen four-channel v0.4.0 payload is decoded into the expanded profile and rewritten only through the alternating-slot commit path. | Build-time only. |
| Local users | 8 | `SECURITY_SERVICE_MAX_USERS`, [services/include/services/security_service.h](../services/include/services/security_service.h) | Eight persisted user records plus eight bounded authentication/backoff records in [services/src/security_service.c](../services/src/security_service.c); adding past capacity fails instead of displacing an existing user. | No. |
| OPC UA secure channels and sessions | 4 each | `config->maxSecureChannels` and `config->maxSessions`, [opcua_model/src/model_core_lifecycle.c](../opcua_model/src/model_core_lifecycle.c) | open62541 rejects a new connection/session at capacity; long-lived open62541 allocations follow the active PSRAM profile. | No. |
| OPC UA subscriptions | 4 total; 4 per session | `config->maxSubscriptions`, `config->maxSubscriptionsPerSession`, [opcua_model/src/model_core_lifecycle.c](../opcua_model/src/model_core_lifecycle.c) | open62541 returns its resource-limit status; no unbounded subscription state is retained. | No. |
| Monitored items | 128 total; 64 per subscription; 32 per service call | `config->maxMonitoredItems`, `config->maxMonitoredItemsPerSubscription`, `config->maxMonitoredItemsPerCall`, [opcua_model/src/model_core_lifecycle.c](../opcua_model/src/model_core_lifecycle.c) | open62541 enforces the global, per-subscription, and per-call bounds. Notification queue depth is separately limited to 1–8 in the same module. | No. |
| In-memory history samples | 512 shared samples | `HISTORY_SERVICE_CHANNEL_CAPACITY`, [services/include/services/history_service.h](../services/include/services/history_service.h) | One 45,064-byte PSRAM ring in [services/src/history_service.c](../services/src/history_service.c) (`512 × 88-byte owned-ID samples` plus indices), plus one bounded 45,056-byte OPC UA HistoryRead scratch buffer in PSRAM instead of the server-task stack. Initialization fails if either required allocation is unavailable, with no internal-SRAM fallback. A static task mutex serializes ring scans without masking interrupts. A full ring overwrites the oldest sample deterministically; reads truncate to caller capacity and report truncation. | No. |
| Retained maintenance journal records | 15 logical records; 24 physical slots in 8 × 4096-byte sectors | `MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS`, `MAINTENANCE_JOURNAL_TOTAL_SLOTS`, and geometry constants in [drivers/include/drivers/maintenance_journal_backend.h](../drivers/include/drivers/maintenance_journal_backend.h) | Uses 32 KiB reserved QSPI Flash. The backend retains at most 15 exportable records, reserves erased sectors for bounded rotation, and blocks mutation on indeterminate, fatal, or unknown-schema recovery rather than accepting uncertain state. | No. |
| Journal export page | 4 logical records | `MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX` / `MAINTENANCE_SERVICE_EXPORT_PAGE_MAX`, [drivers/include/drivers/maintenance_journal_backend.h](../drivers/include/drivers/maintenance_journal_backend.h) and [services/include/services/maintenance_service.h](../services/include/services/maintenance_service.h) | A fixed service staging array holds one page. Cursor validation/recovery errors fail the call; raw Flash buffers are never exported. | No. |
| OTA write chunk | 4096 bytes | `FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES`, [services/include/services/firmware_update_service.h](../services/include/services/firmware_update_service.h) | The OTA service passes a bounded caller chunk to the Flash writer; an oversize, null, or empty chunk is rejected, and Flash write failure clears the active OTA session. | No. |

The configuration-import wire chunk has the same 4096-byte bound
(`CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES` in
[services/include/services/config_import_service.h](../services/include/services/config_import_service.h)).
Unlike OTA streaming, configuration import allocates one temporary payload of
at most `CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES`; allocation failure leaves no
active import session and reports `OUT_OF_MEMORY`.

Channel values and metadata are held in fixed manager-owned slots. Snapshot
reads copy a coherent value, quality and Unix-microsecond source timestamp;
driver I/O is performed outside the registry critical section. Compiled but
inactive native channel drivers do not allocate per-channel runtime contexts.
