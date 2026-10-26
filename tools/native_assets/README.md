# Canonical native menu artwork

Normal builds run `python tools/generate_native_menu_assets.py --verify`. It needs only the tracked packs, the generated header/manifest and the checked-in artwork inputs, not any build artifacts. It checks the input fingerprints, then every pack body against the manifest's per-asset SHA-256, the palettes, curve and registry, and that the sharp pack is the level-0 prefix of the full pack.

To regenerate after an artwork/font/registry change, install a host GCC compiler and run from the repository root:

    python tools/generate_native_menu_assets.py --export
    python tools/generate_native_menu_assets.py --verify

`--stage` copies `assets/sd/menu/native_menu_sharp.pack` to `build/sd/menu/` for `tools/sdsync.py`. Use `--cc PATH` if GCC is not on PATH. The exporter builds fresh host objects under `build/native_menu_asset_export/obj`, without the normal simulator build or its stale-art gate; both checked-in C exporters disable the ROM and render the authored runtime reference. No connected device is accessed.

The output is two binary packs in `assets/sd/menu/` (format and loader behaviour: `docs/SD_ASSETS.md`), `nos/native_menu_assets_id.h` (the input fingerprint the firmware compares with a pack's header) and `nos/native_menu_assets_manifest.inc`. `nos/native_menu_assets.c` and its header are hand-written. Every image is dense neutral grayscale, content-deduplicated; descriptors preserve each independent crop position. Ready RGB565 palettes cover neutral ink and both supported theme accent classes.

All returned pixel pointers are immutable views of the loaded arena. Consumers track them separately from allocated pixels and never free or rasterize into them. Unsupported fonts, registry identities, palette classes or a missing/invalid pack use the retained runtime renderer. Explicit `native_rom 0` / simulator `set native_rom 0` enables the authored bake/fallback path for diagnostics and reference tests.
