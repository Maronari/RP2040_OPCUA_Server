# Flash storage ownership

The generated OTA layout is authoritative. For the 16 MiB reference target,
Bank A is `0x010000+0x7a4000`, Bank B is `0x7b4000+0x7a4000`, the reserved
maintenance-journal region is `0xf58000+0x8000`, and the reserved per-channel
history region is `0xf60000+0x20000`. Configuration begins at `0xf80000`
(unchanged from v0.4.2); identity, trust, security-state, and OTA-state
regions retain their v0.3.2 offsets.

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

Application Flash mutations are serialized by the statically allocated Flash
operation coordinator. Each lock hold performs one aligned 4 KiB erase or one
complete aligned 256-byte page program through `flash_safe_execute`. Timeouts,
nested acquisition, invalid ranges, and safe-execute failures fail closed.
These rules do not imply page-program atomicity, repeated-page-program support,
or hardware latency guarantees.

The former final-sector configuration backend is retired as a writer because
`0xfff000-0xffffff` belongs to the OTA-state region. Its remaining read-only
importer accepts only the legacy magic/version/length/CRC format. It is tried
only when both configuration slots are invalid; decoded data is selected only
after migration to, and read-verification from, an authoritative configuration
slot. The legacy sector is never erased or rewritten.
