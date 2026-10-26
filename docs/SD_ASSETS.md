# Native menu artwork on the SD card

The pre-rendered Settings and launcher artwork (about 1,073 images, 5.8 MB decoded) is no
longer C source or flash `.rodata`. `tools/generate_native_menu_assets.py` renders it from the
authored code and writes two binary packs; `nos/native_menu_assets.c` is a small hand-written loader.

| Pack | Content | Size | Used by |
|---|---|---|---|
| `assets/sd/menu/native_menu_sharp.pack` | level 0 only: 169 records, 128 bodies, 710,204 B decoded | 88,144 B | B watch (SD), 1.43C (embedded) |
| `assets/sd/menu/native_menu_full.pack` | all blur levels: 1,073 records, 876 bodies, 5,795,462 B decoded | 1,519,048 B | simulator and host tests |

Both are tracked. The sharp pack is also copied to `build/sd/menu/` (what `tools/sdsync.py` mirrors).
The device build defines `NOS_RENDER_SHARP`, which never draws a blur level, so level 0 is complete art.

## Format (`NMAP` version 1, little endian)

1. Header, 152 bytes: magic `NMAP`, version, flags (bit 0 = sharp), `hdr_bytes`, `file_bytes`,
   `payload_crc32`, record/body/app counts, registry string bytes, section offsets and sizes
   (meta, palettes, body table, bodies), `arena_bytes`, level-0 body count / decoded bytes / stored bytes,
   `build_input_sha256`, `blob_sha256`, `hdr_crc32` (zlib CRC-32 of the header without this field).
2. Meta: 24-byte records `{family, entry, layout, level, arena offset, bytes, revision, w, h, x, y}`;
   three index tables (settings 36x2x9, icons apps x3x9, names apps x9; 65535 = none); the 650-entry
   launcher curve; the launcher registry (icon id, name offset) and its NUL-terminated strings.
3. Palettes: one zlib blob, 5 grey-to-RGB565 tables in the order of `PAL_ORDER` (settings neutral,
   settings red 0, settings red 1, launcher neutral, launcher press).
4. Body table: 24-byte `{stored offset, arena offset, decoded bytes, compressed bytes, FNV-1a-32, row stride}`.
   Compressed bytes 0 means stored raw; a nonzero stride means row-delta then deflate.
5. Bodies. All level-0 bodies are the first contiguous section, so the sharp pack is a byte-for-byte
   prefix of the full pack's body list. `desc.offset` is the offset into one decoded arena.

`build_input_sha256` is the generator's input fingerprint and is also compiled into the firmware
(`nos/native_menu_assets_id.h`). A pack made from other artwork or fonts is refused as stale.

## Loader behaviour

- **B watch:** `native_menu_assets_init()` (from `nos_init`) reads `/sdcard/menu/native_menu_sharp.pack` through the
  asset worker, bounded to 1.5 s (a stalled worker is cancelled and drained for at most 1 s more; a buffer the worker
  still owns after that is parked and freed when the job is finally taken). It checks the header, payload CRC,
  build binding, table consistency and every body hash while inflating (ROM tinfl) into one PSRAM arena, then publishes.
  Published pointers stay valid until reboot. Success prints `NOSASSETS loaded=<bytes> ms=<t> ...`.
- **Any failure** (no card, missing/short/corrupt/stale file, timeout, no PSRAM, worker unavailable) prints
  `NOSASSETS ERROR sd: <reason>; dynamic artwork fallback`, frees everything and leaves
  `native_menu_assets_compatible()==0`, so Settings and the launcher run their dynamic bake. Under `NOS_RENDER_SHARP`
  that bake no longer renders the 16 launcher blur sprites per row.
- A failed load is retried automatically from the UI loop (`native_menu_assets_retry_due`): after a card (re)mounts,
  1 s, then 5, 15 and 45 s later, at most 4 tries per mount, never during a touch, injected gesture or capture. On
  success the launcher and Settings caches are retired exactly as `native_rom` does. Serial `native_rom 1` still
  retries on demand; `NOSNATIVE` reports
  `pack=loaded|failed|untried src= sharp= pack_bytes= records= load_ms= error=` after the original fields.
- **1.43C** has no SD slot: CMake embeds the sharp pack (`NOS_MENU_PACK_EMBEDDED`) and the Orrery catalogs
  `belt.bin` / `moons.bin` (`NOS_SKY_EMBEDDED`) only for that board.
- **Simulator and host tests:** the full pack is read from `assets/sd/menu` (or `NOS_MENU_PACK=<file>`) on first use.

## Regenerate and push

    python tools/generate_native_menu_assets.py --export    # rebuild references and both packs (needs host gcc)
    python tools/generate_native_menu_assets.py --verify    # no build artifacts needed; checks hashes against the manifest
    python tools/generate_native_menu_assets.py --stage     # copy the sharp pack to build/sd/menu
    python tools/sdsync.py                                  # mirror build/sd (menu/ and sky/) onto the card

`tools/build_identity.py prepare` (run by `tools/build.ps1` and `build_c143.ps1`) copies the tracked sharp pack and
the two sky catalogs from `assets/sd/` into `build/sd/` when they are missing or differ, checks the pack's CRC32s and
build hash against `nos/native_menu_assets_id.h`, and warns on stderr when `sd_assets.complete` is false (for example
the 11 textures are absent). The identity binds the staged files, i.e. what `sdsync.py` sends.

Commit the changed packs, `nos/native_menu_assets_id.h` and `nos/native_menu_assets_manifest.inc` together.
Run `--verify` after any change to fonts, graphics or the registry; the build fails on stale packs. The archived
build identity records the sharp pack's hash under `sd_assets`.
