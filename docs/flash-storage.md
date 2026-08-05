# Flash storage ownership

The generated OTA layout is authoritative. For the 16 MiB reference target,
Bank A is `0x010000+0x7b4000`, Bank B is `0x7c4000+0x7b4000`, and the reserved
maintenance-journal region is `0xf78000+0x8000`. Configuration begins at
`0xf80000`; identity, trust, security-state, and OTA-state regions retain their
v0.3.2 offsets.

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
