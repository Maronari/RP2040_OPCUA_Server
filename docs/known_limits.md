# Known resource limits

This document records the limits compiled into the RP2350 + PSRAM + W5500
reference firmware. They are fixed firmware build settings, not remotely
configurable values, unless stated otherwise. Requests at a limit are rejected
with the subsystem's normal resource/status result; they never expand a queue,
session table, or staging buffer at runtime.

| Resource | Exact limit | Control and source | Memory / failure behavior | Configurable |
| --- | ---: | --- | --- | --- |
| Persisted configured channels | 4 | `CONFIG_SERVICE_MAX_CHANNELS`, [services/include/services/config_service.h](../services/include/services/config_service.h) | Four `config_channel_definition_t` entries are embedded in `device_profile_config_t`; validation rejects duplicate IDs and invalid/unknown channel drivers before a candidate can be applied. | No; compile-time source edit and firmware rebuild. |
| Native registry channels | 16 | `CHANNEL_MANAGER_MAX_CHANNELS`, [services/include/services/channel_manager.h](../services/include/services/channel_manager.h) | Static pointer registry (64 bytes on this 32-bit target). Registration rejects null/empty IDs, duplicate IDs, and capacity; configuration currently exposes only the four persisted channel slots above. | No; compile-time source edit and firmware rebuild. |
| Local users | 8 | `SECURITY_SERVICE_MAX_USERS`, [services/include/services/security_service.h](../services/include/services/security_service.h) | Eight persisted user records plus eight bounded authentication/backoff records in [services/src/security_service.c](../services/src/security_service.c); adding past capacity fails instead of displacing an existing user. | No. |
| OPC UA secure channels and sessions | 4 each | `config->maxSecureChannels` and `config->maxSessions`, [opcua_model/src/model_core_lifecycle.c](../opcua_model/src/model_core_lifecycle.c) | open62541 rejects a new connection/session at capacity; long-lived open62541 allocations follow the active PSRAM profile. | No. |
| OPC UA subscriptions | 4 total; 4 per session | `config->maxSubscriptions`, `config->maxSubscriptionsPerSession`, [opcua_model/src/model_core_lifecycle.c](../opcua_model/src/model_core_lifecycle.c) | open62541 returns its resource-limit status; no unbounded subscription state is retained. | No. |
| Monitored items | 128 total; 64 per subscription; 32 per service call | `config->maxMonitoredItems`, `config->maxMonitoredItemsPerSubscription`, `config->maxMonitoredItemsPerCall`, [opcua_model/src/model_core_lifecycle.c](../opcua_model/src/model_core_lifecycle.c) | open62541 enforces the global, per-subscription, and per-call bounds. Notification queue depth is separately limited to 1–8 in the same module. | No. |
| In-memory history samples | 512 shared samples | `HISTORY_SERVICE_CHANNEL_CAPACITY`, [services/include/services/history_service.h](../services/include/services/history_service.h) | One static ring in [services/src/history_service.c](../services/src/history_service.c), approximately 10 KiB on the 32-bit target (`512 × 20-byte sample`); a full ring overwrites the oldest sample deterministically. Reads truncate to caller capacity and report truncation. | No. |
| Retained maintenance journal records | 15 logical records; 24 physical slots in 8 × 4096-byte sectors | `MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS`, `MAINTENANCE_JOURNAL_TOTAL_SLOTS`, and geometry constants in [drivers/include/drivers/maintenance_journal_backend.h](../drivers/include/drivers/maintenance_journal_backend.h) | Uses 32 KiB reserved QSPI Flash. The backend retains at most 15 exportable records, reserves erased sectors for bounded rotation, and blocks mutation on indeterminate, fatal, or unknown-schema recovery rather than accepting uncertain state. | No. |
| Journal export page | 4 logical records | `MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX` / `MAINTENANCE_SERVICE_EXPORT_PAGE_MAX`, [drivers/include/drivers/maintenance_journal_backend.h](../drivers/include/drivers/maintenance_journal_backend.h) and [services/include/services/maintenance_service.h](../services/include/services/maintenance_service.h) | A fixed service staging array holds one page. Cursor validation/recovery errors fail the call; raw Flash buffers are never exported. | No. |
| OTA write chunk | 4096 bytes | `FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES`, [services/include/services/firmware_update_service.h](../services/include/services/firmware_update_service.h) | The OTA service passes a bounded caller chunk to the Flash writer; an oversize, null, or empty chunk is rejected, and Flash write failure clears the active OTA session. | No. |

The configuration-import wire chunk has the same 4096-byte bound
(`CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES` in
[services/include/services/config_import_service.h](../services/include/services/config_import_service.h)).
Unlike OTA streaming, configuration import allocates one temporary payload of
at most `CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES`; allocation failure leaves no
active import session and reports `OUT_OF_MEMORY`.

The configured-channel limit (4) and native registry capacity (16) deliberately
remain distinct in this release: the latter is an internal registry capacity,
while the persisted public configuration schema has four stable slots. Raising
either is out of scope for v0.4.0 because it changes memory sizing and/or the
configuration contract.
