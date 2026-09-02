# Flash storage ownership

The generated OTA layout is authoritative. For the 16 MiB reference target,
Bank A is `0x010000+0x7a4000`, Bank B is `0x7b4000+0x7a4000`, the reserved
maintenance-journal region is `0xf58000+0x8000`, and the reserved per-channel
history region is `0xf60000+0x20000`. Configuration begins at `0xf80000`
(unchanged from v0.4.2); identity, trust, security-state, and OTA-state
regions retain their v0.3.2 offsets. As of v0.5.0, the 128 KiB configuration
region's *outer* boundary and total size are unchanged — only its internal
layout changed, from a two-slot generation store to a three-role transaction
backend; see "Configuration transaction backend (v0.5.0)" below. No other
region moved.

As of v0.4.3, both OTA banks were shrunk by 64 KiB each (from `0x7b4000` to
`0x7a4000`) to carve out the new 128 KiB history region, placed immediately
after the maintenance journal and before the configuration slots, following
the same named-symbol mechanism (`OTA_HISTORY_SIZE_BYTES` /
`OTA_LAYOUT_HISTORY_OFFSET_BYTES` / `OTA_LAYOUT_HISTORY_ORIGIN`) used for the
maintenance journal. This reduces the maximum application image size for all
future releases; the last shipped OTA payload (3,297,616 bytes) leaves ample
headroom within the new ~7.66 MiB bank size. See
`drivers/include/drivers/history_flash_backend.h` for the per-channel ring
layout inside this region (a non-transactional cyclic buffer, unlike the
maintenance journal) and `docs/releases/v0.4.3.md` for the frozen budget
rationale.

Each 32-byte per-sample record binds to its owning channel with a 4-byte
`channel_id_hash` (the full, unfolded 32-bit FNV-1a hash of the channel's
stable id) rather than the physical channel slot index: since slot
assignment is a first-available claim over the channel registry made fresh
at every boot (`services/history_service.c`), it is not guaranteed stable
across a reconfiguration that adds, removes, or reorders channels, and a
slot can end up bound to a different channel_id after such a reboot. A
1-byte `RECORD_FORMAT_VERSION` marker (occupying the exact offset a
pre-2026-08-22 record always wrote as its fixed valid-marker byte) makes
this widening self-versioning: records written under the prior 8-bit-hash
layout are deterministically rejected as corrupt/foreign on restore rather
than risk misattributing history to the wrong channel, following the same
embedded-schema-version precedent as `maintenance_journal_backend`'s record
schema field. See `drivers/src/history_flash_backend.c` for the exact byte
offsets.

<a id="config-txn-backend"></a>
## Configuration transaction backend (v0.5.0)

The 128 KiB configuration region (`0xf80000`-`0xf9ffff`, generated as
`OTA_LAYOUT_CONFIG_SLOT_0`/`_1` by `ota/cmake/ota_layout.cmake`) is no
longer the pre-v0.5.0 two-slot generation store. `drivers/src/
config_transaction_backend.c` repartitions the same 128 KiB budget into
three equal `CONFIG_TXN_ROLE_SIZE_BYTES` (10 sectors x 4096 bytes = 40 KiB)
roles — STAGED, RUNNING, LAST_KNOWN_GOOD, in that order — at `0xf80000`,
`0xf8a000`, and `0xf94000`, leaving an 8 KiB reserved tail
(`0xf9e000`-`0xf9ffff`) unused. No OTA-bank shrink and no new Flash region
were needed: `device_profile_config_t` is 5,624 bytes at the 16-channel
reference limit, comfortably under each role's 40,448-byte payload capacity
(`CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES`) — see
`drivers/include/drivers/config_transaction_backend.h` for the frozen
reasoning (v0.5.0 plan T3 scope decision 2).

Each role holds at most one physical record and follows the maintenance
journal's commit-page/CRC classification pattern
(`config_transaction_backend_classify_commit()`), not a second physical
copy for atomic-swap safety: erase the whole role, program a header page,
program the payload, read back and byte-compare the body, then program a
single dedicated commit page as the last write. The commit page's own
physical exactness — never a program call's return value — is the sole
atomicity signal (`config_txn_commit_state_t`:
`COMMITTED`/`NOT_COMMITTED`/`INDETERMINATE`/`CORRUPT`/`UNKNOWN`). A torn
write to any role classifies as `NOT_COMMITTED`/`CORRUPT` on the next
`config_transaction_backend_recover()` and is safely discardable, never
mistaken for a valid record.

`config_transaction_backend_commit()` promotes STAGED to RUNNING by first
copying the current RUNNING (if committed) into LAST_KNOWN_GOOD and
confirming that write, and only then overwriting RUNNING — an ordering that
guarantees RUNNING and LAST_KNOWN_GOOD can never both be invalid after a
power loss at any point in the sequence (`config_transaction_recovery_state_t`'s
`FALLBACK_REQUIRED` outcome self-heals via
`config_transaction_backend_recover_fallback_to_last_known_good()`).
`services/src/config_store_service.c` is the sole caller — one lazily
initialized `config_transaction_backend_t` instance
(`config_transaction_backend_init_flash()` + one `recover()` on first use).
The backend is independent of the maintenance journal and history ring:
three separate mechanisms, three separate Flash regions, no shared state.

**Migrating an existing device**: this backend's on-Flash record format
(header magic `"CTX1"` / commit-page magic `"CTXC"`,
`drivers/include/drivers/config_transaction_backend.h:76-81`) shares no
magic value and no physical offset convention with the pre-v0.5.0 two-slot
format's header (formerly `CONFIG_STORE_SLOT_MAGIC` = `"CFG1"`). A device
already running pre-v0.5.0 firmware that upgrades in place would have its
existing two-slot-format bytes reinterpreted under the new role/commit-page
layout, which does not recognize them. This is a deliberately unaddressed
gap, not an oversight: this project has no fleet of field devices ever
running pre-v0.5.0 firmware, and every device is flashed directly with the
current tagged release rather than upgraded in place from an old image — see
[docs/known_limits.md](known_limits.md#v050-accepted-residual-risks) for the
full reasoning.

Application Flash mutations are serialized by the statically allocated Flash
operation coordinator. Each lock hold performs one aligned 4 KiB erase or one
complete aligned 256-byte page program through `flash_safe_execute`. Timeouts,
nested acquisition, invalid ranges, and safe-execute failures fail closed.
These rules do not imply page-program atomicity, repeated-page-program support,
or hardware latency guarantees.

The former final-sector configuration backend (`0xfff000-0xffffff`, which
belongs to the OTA-state region) is fully retired as of v0.5.0: both its
writer and its read-only pre-config-store importer
(`flash_storage_driver_load_partial()`) were removed, for the same reason as
the two-slot migration gap above -- no device this project ships is ever
field-upgraded from that old an image.
