# Known resource limits

This document records the limits compiled into the RP2350 + PSRAM + W5500
reference firmware. They are fixed firmware build settings, not remotely
configurable values, unless stated otherwise. Requests at a limit are rejected
with the subsystem's normal resource/status result; they never expand a queue,
session table, or staging buffer at runtime.

| Resource | Exact limit | Control and source | Memory / failure behavior | Configurable |
| --- | ---: | --- | --- | --- |
| Configured and runtime channels | 16 (reference default; supported compile-time range 1вЂ“16) | `PLATFORM_CHANNEL_LIMIT`, [services/include/services/platform_limits.h](../services/include/services/platform_limits.h) | The same bound sizes persisted channel definitions, manager-owned runtime slots, generic OPC UA channel objects/callback contexts, PubSub writer state and diagnostics. The reference profile uses bounded PSRAM state and fails initialization if it is unavailable; no internal-SRAM fallback is used. Registration rejects invalid metadata, duplicate IDs, stale generations and capacity. Model construction uses a fixed field/context capacity and rolls back a failed startup transaction; topology changes use the existing controlled reboot path. | Build-time only. |
| Local users | 8 | `SECURITY_SERVICE_MAX_USERS`, [services/include/services/security_service.h](../services/include/services/security_service.h) | Eight persisted user records plus eight bounded authentication/backoff records in [services/src/security_service.c](../services/src/security_service.c); adding past capacity fails instead of displacing an existing user. | No. |
| OPC UA secure channels and sessions | 4 each | `config->maxSecureChannels` and `config->maxSessions`, [opcua_model/src/model_core_lifecycle.c](../opcua_model/src/model_core_lifecycle.c) | open62541 rejects a new connection/session at capacity; long-lived open62541 allocations follow the active PSRAM profile. | No. |
| OPC UA subscriptions | 4 total; 4 per session | `config->maxSubscriptions`, `config->maxSubscriptionsPerSession`, [opcua_model/src/model_core_lifecycle.c](../opcua_model/src/model_core_lifecycle.c) | open62541 returns its resource-limit status; no unbounded subscription state is retained. | No. |
| Monitored items | 128 total; 64 per subscription; 32 per service call | `config->maxMonitoredItems`, `config->maxMonitoredItemsPerSubscription`, `config->maxMonitoredItemsPerCall`, [opcua_model/src/model_core_lifecycle.c](../opcua_model/src/model_core_lifecycle.c) | open62541 enforces the global, per-subscription, and per-call bounds. Notification queue depth is separately limited to 1–8 in the same module. | No. |
| In-memory history samples | 256 samples per channel (16 channels = 4096 samples total) | `PLATFORM_HISTORY_SAMPLES_PER_CHANNEL`, [services/include/services/platform_limits.h](../services/include/services/platform_limits.h) | One typed, owned 32-byte-record PSRAM ring per channel in [services/src/history_service.c](../services/src/history_service.c) — 131,072 bytes (128 KiB) total across all 16 channel slots, plus one bounded `HISTORY_SERVICE_MAX_RETURN_COUNT` (128) sample OPC UA HistoryRead scratch buffer in PSRAM. Initialization fails if any required allocation is unavailable, with no internal-SRAM fallback. Reads use bounded `HISTORY_SERVICE_READ_CHUNK_SAMPLES` (32) chunk-sized critical sections per channel, never one lock across a whole-channel or multi-channel scan. A full per-channel ring overwrites that channel's oldest sample deterministically; reads truncate to caller capacity, report truncation via a bounded continuation point, and report a ring-generation mismatch if the retained window moved since the point was issued. | No. |
| Flash-backed history samples | 256 samples per channel (matches the PSRAM capacity above) | `HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL`, [drivers/include/drivers/history_flash_backend.h](../drivers/include/drivers/history_flash_backend.h) | 128 KiB reserved QSPI Flash (`OTA_HISTORY_*`, [ota/cmake/ota_layout.cmake](../ota/cmake/ota_layout.cmake)), 8 KiB per channel as two 4 KiB double-buffered erase sectors. A non-transactional cyclic buffer (unlike the maintenance journal): each 32-byte record carries a CRC32, a valid marker, and a channel-id hash, so a torn or foreign record is skipped on restore rather than exposed. Writes are batched 8 records/page to respect QSPI page-program granularity; losing up to 7 of the most recent, not-yet-flushed samples per channel after power loss is accepted, documented behavior. Restoring this region shrank both OTA application banks from `0x7b4000` to `0x7a4000` bytes (64 KiB each) — see the OTA transfer measurement below for current headroom. | No. |
| Alarm condition instances | 1 per alarm-eligible channel (up to 16) | `ALARM_SERVICE_MAX_CONDITIONS`, [services/include/services/alarm_service.h](../services/include/services/alarm_service.h) | 2,432 bytes of PSRAM (16 × 152-byte `alarm_condition_status_t`) in [services/src/alarm_service.c](../services/src/alarm_service.c), keyed by stable channel id. Every numeric or Boolean channel present at startup gets one independent condition (high/high-high/low/low-low with configurable hysteresis and debounce for numeric channels, a discrete state for Boolean channels); String channels are not eligible. A quality fault (stale/invalid channel quality) is tracked as a separate field and never overwrites the last good-quality process/discrete state. Acknowledgement is independent per condition; alarm transitions are live OPC UA Alarms & Conditions events only, with no Flash persistence for alarm history in this release. | No. |
| Retained maintenance journal records | 15 logical records; 24 physical slots in 8 × 4096-byte sectors | `MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS`, `MAINTENANCE_JOURNAL_TOTAL_SLOTS`, and geometry constants in [drivers/include/drivers/maintenance_journal_backend.h](../drivers/include/drivers/maintenance_journal_backend.h) | Uses 32 KiB reserved QSPI Flash. The backend retains at most 15 exportable records, reserves erased sectors for bounded rotation, and blocks mutation on indeterminate, fatal, or unknown-schema recovery rather than accepting uncertain state. | No. |
| Journal export page | 4 logical records | `MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX` / `MAINTENANCE_SERVICE_EXPORT_PAGE_MAX`, [drivers/include/drivers/maintenance_journal_backend.h](../drivers/include/drivers/maintenance_journal_backend.h) and [services/include/services/maintenance_service.h](../services/include/services/maintenance_service.h) | A fixed service staging array holds one page. Cursor validation/recovery errors fail the call; raw Flash buffers are never exported. | No. |
| OTA write chunk | 4096 bytes | `FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES`, [services/include/services/firmware_update_service.h](../services/include/services/firmware_update_service.h) | The OTA service passes a bounded caller chunk to the Flash writer; an oversize, null, or empty chunk is rejected, and Flash write failure clears the active OTA session. | No. |

## Measured OTA transfer time

On 2026-08-13, firmware `v0.4.1` (`a5ebc3231faf`) was updated on the
`waveshare_core2350b_16mb_psram8mb` reference device over authenticated
OPC UA (`Basic256Sha256`, `SignAndEncrypt`) through the W5500 Ethernet
interface. The 3,297,616-byte payload used 4096-byte chunks and required:

- about 7 minutes 15 seconds of active transfer and finalize time, or about
  7.4 KiB/s effective throughput;
- 8 minutes 17 seconds wall-clock time from `BeginFirmwareUpdate` to a
  successful `FinalizeFirmwareUpdate`, including about one minute lost to a
  client timeout and reconnection; and
- about 45 seconds from the reboot request until the updated OPC UA server was
  available again.

The current implementation sends each chunk as a separate synchronous OPC UA
Method call and writes/verifies it through the Flash staging path. The measured
rate is therefore dominated by per-chunk round trips and synchronous Flash
work, not by Ethernet bandwidth. Improving this requires a separately scoped
change, such as a larger bounded chunk size or buffered/batched Flash writes;
the figures above are measurements, not performance acceptance targets.

The configuration-import wire chunk has the same 4096-byte bound
(`CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES` in
[services/include/services/config_import_service.h](../services/include/services/config_import_service.h)).
Unlike OTA streaming, configuration import allocates one temporary payload of
at most `CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES`; allocation failure leaves no
active import session and reports `OUT_OF_MEMORY`.

Channel values and metadata are held in fixed manager-owned slots. Snapshot
reads copy a coherent value, quality and Unix-microsecond source timestamp;
driver I/O and generation-safe writes are performed outside the registry
critical section. Concurrent driver operations use no unbounded queue. Compiled but
inactive native channel drivers do not allocate per-channel runtime contexts.
