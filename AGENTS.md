# Workspace and device provenance

This workspace contains the recovered `optimizations` working state plus the sports
update and Control Center paging. Its branch HEAD predates that work: preserve the
uncommitted and untracked source files. See `docs/RECOVERY_20261020.json` and the
hashed source archives under `build/recovery-20261020/`.

Build from this workspace. Before flashing, verify Solar/Orrery, Developer Tools,
the integrated home pager, and the sports apps are included. Use the ESP-IDF Python
environment to run `tools/flash_watch.py --port COM3`, or `tools/build.ps1 -Port COM3
-Flash`. These checkpoint preferences and verify that flash ranges exclude NVS.
Do not silently erase NVS or substitute firmware from another worktree.

Settings are authoritative on SD at `/watch/settings.0.bin` and
`/watch/settings.1.bin`, loaded into RAM at boot. Keep the checksummed alternating
checkpoint, fsync/readback verification, mount lease and SD lock. NVS is a
migration/card-absent fallback; a failed SD save must remain dirty for retry.

# Active board

The active target is the **Waveshare ESP32-S3-Touch-AMOLED-1.43-B watch**, selected
by the user on 2026-10-23. Read [CLAUDE.md](CLAUDE.md) for both boards' identities,
port hints, separate build targets and incompatible pin assignments. Keep the
1.43C audio board as a separate target; verify board identity before flashing.
