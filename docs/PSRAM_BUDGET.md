# NOS PSRAM budget (8 MiB)

## Settings per-entry sprites (2026-10-15)

Settings no longer uses the shared 90 px `ui_fblur` bands. Its 36 entries use
fixed columns, one sharp 8-bit intensity sprite and three vertical box-blurred
sprites (16/32/48 px), cropped to each entry's 60/90/120 px pitch and then to
nonblack bounds. Neutral and accent LUTs preserve bezel dimming and Reset's red.
At rest the live renderer preserves the existing value/toggle/arm animations.
Active tweens and armed/pressed pills also stay live during a fling, so the
settled cache cannot hide a pending confirmation. Cached rows retain build-in
position and brightness during an immediate flick.
Each entry's value, unit, toggle, hardware availability and relevant accent form
its signature. Only changed settled entries rebake while resident; evicted
entries rebuild on reopening. No geometry depends on the row's screen position.

All allocations are attributed to **`set_settings.c`**. `mem` also prints
`SETTINGS_CACHE entries=.../36 bytes=... limit=... bakes=...` for coverage and
invalidation checks. The owner has a **3,145,728-byte (3 MiB) payload cap**, below
the unchanged global 6.5 MiB charged limit and the device's 1 MiB free reserve.
The main allocation is `sizeof(*RC)` (entry metadata, two 17x256 RGB565 LUTs,
343x120 RGB565 canvas and two 343x120 byte work buffers); each of the 144 sprite
allocations is `cropped_width * cropped_height`. No screen-sized allocation.

Measured simulator payloads (64-bit ABI; board `mem` remains authoritative):

| State | Settings payload | Live blocks |
|---|---:|---:|
| Warm, all 36 entries | 2,470,214 bytes = **2,412.3 KiB** | 145 |
| Cold open / transient service values | 2,507,042 bytes = **2,448.3 KiB** | 145 |
| Home, first five entries prewarmed | 504,207 bytes = **492.4 KiB** | 21 |
| Other app/subpage after transition, or planet card preparation | **0** | 0 |

Each allocation additionally has the existing 8-byte charged header (1,160
bytes for the complete list). Partial entry allocations roll back on failure;
unavailable entries draw live and retry at idle. A settled changed entry can
rebake during a fling; normal motion only blits. The first five entries prewarm
one per idle home slice; opening synchronously completes the remaining entries
so an immediate long fling has all three levels throughout the list.

Lifetime is essential to this budget: opening Settings releases inactive
generic-list, notification and page blur arenas, plus existing offscreen launcher
and orbit scratch trims. On returning home, Settings retains only its first
screenful and releases the inactive generic-list arena. Other lists retain their
unchanged `ui_fblur` implementation and rebuild normally. All cache releases
occur on the UI thread, with outgoing Settings rows retained through transitions.
Opening a planet card releases every Settings allocation before requesting card
buffers. This replaces the old simultaneous cache inventory, rather than adding
2.4 MiB to it.

`python tools/test_psram.py` passes all 43 scenarios with zero unexpected
allocation failures. Heavy-session peak is **6,185,026 bytes** (previously
6,467,251); Earth open remains **6,138,499**, and after repeated closes/home
prewarming is **5,865,538**. Reopening Settings after that heavy session produces
36/36 ready entries at **5,442,601** total payload, with no failures. The older
tables below document the preceding band-cache implementation and its audit.

Validation: strict sim build, both `tools/nos_blur*.txt`, the memory stress suite,
and `tools/test_settings_blur.py` pass. The Settings runner also compiles a native
test of every entry/level against a scalar box-filter reference (including all
cropped pixels), unchanged signatures, armed Reset rendering during a fling,
and complete cache release. Windows test startup sets `SetErrorMode`, routes
CRT diagnostics to stderr, and configures `_set_abort_behavior` by export for
available CRTs (the installed older MinGW's msvcrt lacks that export). Settings is
**59/59** fast frames blurred in `nos_blur`, with no late Settings frames; a cold
`open!` followed immediately by a fling also has complete coverage. Both standard
scripts retain one pre-existing 53 ms initial Orrery frame outside their checked
sections, reproduced with the saved baseline executable. The 621-frame capture
has zero late frames. Damage/full-repaint comparisons pass for all 621 capture
frames and 160 value/toggle/theme frames. Counters prove idle does not rebake,
a toggle changes one entry, a clock value changes one entry, and theme changes
only the Theme value and Reset accent. Five injected cache allocation failures
recover to 36/36 entries.

Before/after PNGs: `build/settings_blur/{before,after}/rest_{top,middle,bottom}.png`
and `blur_{16,32,48}.png`; 2-column x 4-row overview `build/settings_blur/sheet.png`;
all three blur levels `build/settings_blur/blur_levels.png`. Reproduce with
`python tools/test_settings_blur.py --before build/settings_blur/baseline.exe`.
That baseline links HEAD (`f46418a`) Settings/PSRAM with the guarded simulator
entry point; it has the original rendering and cannot pop a test-failure dialog.

Expected board motion cost, **not new board measurements**: fast-frame median
blits **3.16 ms**, total raster **4.21 ms** (previously **12.80 ms**), modeled
frame including bus **18.23 ms** (previously **23.65 ms**). Cold synchronous open
baking and cache rebuilds on return need separate board timing; script frame
statistics do not measure work performed by the synchronous `open` command.
Reclaimed page/notification/launcher caches also need idle time to warm again.
Memory pressure, an allocation failure or an active confirmation/tween can leave
a row sharp;
`mem` coverage and the existing blur counters expose that fallback. No firmware
was flashed and no serial connection was opened.


Scope: `watch-test`, baseline `d549194`, 466 × 466, 1.43-B. All sizes below
are **bytes**, unless marked KiB/MiB (1024-based). No firmware was flashed or
serial port opened. Simulator sizes include its 64-bit pointers; `mem` on the
watch supplies authoritative target-ABI sizes. Formulas using `sizeof` are the
exact allocation expressions, not estimates of the struct's contents.

## Before / after

| Owner | Before | After / lifetime |
|---|---:|---|
| List/Settings fling arena (`ui.c`) | 2,662,400; descending allocation attempts could leave too few slots | **786,432 (768 KiB)**; 11 Settings bands (444 x 90), versus about 40; visible bands and neighbours evict distant bands; released before card allocation |
| Notification blur (`kblur.c`) | 1,740,800; descending allocation attempts, newest cards only | **917,504 (896 KiB)**; six slots (347 x 189 maximum): visible cards plus neighbours and CLEAR ALL; correct LRU; released before card allocation |
| Page/drawer blur (`pblur.c`) | 894,616 when all seven pages are populated | Same maximum: seven 108,578-byte copies, one staging copy and 25,992-byte work struct in the simulator; all freed before a card, rebuilt on return |
| Orrery state, belt, radial maps, scene/dot lists, UI commands | 606,795 in the exercised scene, plus optional zoom controls | Same published scene, preserving return/pan speed |
| Orrery orbit coverage and sorting workspace | Board report ~687 KB; sim sweep 671,808 | Same active capacity; **352,320** retained after trimming (319,488 scratch bytes reclaimed in that scene); scratch freed on card open or after five seconds without a face draw; growth frees invalid old buffers first |
| Orrery zoom-control masks | 113,732 | Same; retained to avoid rebuilding controls on the first return frame |
| Planet card, excluding night texture | **2,117,536** in simulator; retained after closing | Same pixels/LUT resolution, **zero after closing**; transactional allocation and complete cleanup on failure; day/night textures use 16 x 64 KiB pages each |
| Earth night texture | 1,048,576, freed on non-Earth selection/leave | Same; released with all other card buffers after the closing animation |
| Launcher tables and sprites | 1,274,633 in this app registry | Same when warm; **697,809** during the exercised card open, by evicting only offscreen blurred variants; sharp sprites and visible rows survive; idle prewarm rebuilds evicted variants |
| Dot font/icon tables and all 160 mask sizes | 600,112 fully prebaked | Same useful data; losing concurrent mask allocations now freed |
| Segment face cache | 440,352 | Same; partial allocation now rolled back and retried |
| Other fonts, services, apps, platform buffers | See inventory below | Bounded allocations retained; failures attributed and retry latches removed where found |

The two arenas alone save **2,699,264 bytes**. A card actually needs about
**3.02 MiB including Earth's night map**, rather than 3 MiB plus another 1 MiB.
Opening it releases 2,598,552 bytes of blur arenas/work plus idle orbit scratch
and offscreen launcher blur variants before requesting its buffers.

## Budget and headroom

`NOS_MEM_BUDGET = 6,815,744` (6.5 MiB) caps tracked allocations **including their
8-byte ownership headers** and tracked SDK task stacks. This leaves 1.5 MiB
of an 8 MiB chip for SDK/libc heap consumers, allocator metadata and free space.
On ESP32, each tracked allocation additionally checks actual SPIRAM free space
and refuses to consume the final **1 MiB**. It never falls back to internal RAM.
Concurrent tracked requests reserve their budget before calling the heap.

The measured stress sequence fully prebakes dots, exercises home/Settings/
notifications, visits all nine faces and the ordinary apps, zooms the orrery,
then opens Earth, covers/reveals it, closes it, and repeats five times:

| Point | Tracked payload | Allocation failures |
|---|---:|---:|
| Heavy session, zoomed orrery, all blur caches warm | **6,467,251** | 0 |
| Earth card open, including day and night maps | **6,138,499** | 0 |
| After repeated closes and home prewarming | **6,147,763** | 0 |
| High-water mark over the complete sequence | **6,467,251** | 0 |

At this peak, about **1.92 MB remains before platform allocations/heap overhead**.
Allowing 0.25–0.5 MiB for device-only/platform consumers gives an expected
**1.4–1.65 MB minimum**, and approximately **1.7–2.0 MB free after the heavy
session**. A less extensive home/Settings/notifications session should be near
2 MB or above. These are projections, **not a new board measurement**; the old
214,604-byte free reading included pressure-shrunk arenas and cannot simply have
the nominal arena savings added to it.

SDK allocations (Wi-Fi, NimBLE, TLS, stdio/FATFS, task metadata) are outside the
portable allocator. Their allowance must be checked on the board, especially
with network/OTA activity. The 1 MiB check stops NOS from spending the reserve;
it does not prevent the SDK from independently allocating afterward. `mem`
reports actual free, largest free block and minimum-ever-free for that reason. `sdk_and_overhead` is actual heap use
minus tracked payload, exposing the otherwise unattributed consumers.

## Allocation inventory: linked NOS application

“Retained” means no normal free before reboot. Entries with unchanged sizes have
the same before/after size; their allocation failure is now counted by owner.
Sprite wrappers attribute memory to their **calling source file**, not to the
generic sprite allocator. Baked font/data arrays are flash `.rodata`, not heap.

| Owner / allocation site | Exact payload size / bound | Allocated when | Before → after free policy |
|---|---|---|---|
| `ui.c`: `FB.arena` | 2,662,400 → 786,432; includes slots, LUT, recorder, strip canvas, grey source and filter scratch | First list/prewarm | Retained → card preparation releases whole arena |
| `kblur.c`: one arena | Caller supplied; notifications 1,740,800 → 917,504 | First notification prewarm | Retained → card preparation releases arena |
| `pblur.c`: `PB` | `sizeof(struct pb_work)`; 25,992 on sim | First page bake | Retained → release with page cache |
| `pblur.c`: `stage` | `466 * 233 = 108,578` | First page bake | Retained → release with page cache |
| `pblur.c`: `slot[i].px`, i=0..6 | 108,578 each; maximum 760,046 | Each home/drawer page's first bake | Retained → release with page cache; stage/page pointers swap on publication |
| `orrery_card.c`: `C.b`, `C.nb` | `2 * sizeof(body_state)`; 31,776 total | Card enter | Retained → close/failure rollback |
| Card: `C.texture[16]` | 1,048,576 total; 16 x 65,536 pages (formerly one contiguous block) | Card enter | Retained → close/failure rollback |
| Card: `C.night[16]` | 1,048,576 total; 16 x 65,536 pages (formerly one contiguous block) | Earth load stage | Non-Earth/leave → non-Earth or completed close |
| Card: `C.lut` | `236 * 236 * sizeof(globe_lut)` = 445,568 | Card enter | Retained → close/failure rollback |
| Card: `C.rings` | `416 * 236 * 2` = 196,352 | Card enter | Retained → close/failure rollback |
| Card: `C.ring_tex` | 2,048 | Card enter | Retained → close/failure rollback |
| Card: `C.lighting` | 512 | Card enter | Retained → close/failure rollback |
| Card: globe and neighbour `sprite_alloc` | `2 * (416 * 236 * 2)` = 392,704 | Card enter | Retained → close/failure rollback; body swipe swaps ownership |
| `face_orrery.c`: `s_orr` | `sizeof(orr_state)`; 89,976 in sim (the old ~48 KiB comment predates the retained geometry) | First face operation | Retained (settings, astronomy and event state survive cache trimming) |
| Orrery: belt `bk`, `bp`, `f`, `bh` | `n*48`, `n*16`, `n*8`, `n`; n=3,999, total **291,927** | First belt preparation | Retained; partial failure frees all four |
| Orrery: `belt_radius[4]` | `4 * n * sizeof(float)` maximum **63,984** | First use of each view | Retained |
| Orrery: `S.list` | `sizeof(orr_list)` = 203,936 in sim; includes both dot streams, row indices, 2,048 commands, 8,192 refs, 256 text entries, 64 arcs | First scene build | Retained; intentionally preserved across covering screens |
| Orrery: `S.ui_cache` | `sizeof(orr_ui_cache)` = 4,960 in sim | First captured controls | Retained |
| `face_orrery_runs.h`: `S.orbit_pixels` | `sizeof(orr_orbit_cache)` = 32,832 in sim | First orbit build | Retained |
| Orbit `px`, `scratch` | Each `6 * round_up(need,4096)`; sweep capacity 53,248 pixels = 319,488 each | First build / growth | Old arrays freed after allocating replacements → old invalid arrays freed **before** replacements; scratch also reclaimed idle/on card open |
| `face_orrery_controls.h`: `orr_zmask` | `sizeof(struct orr_zoom_masks)` = 113,732 | First zoom controls | Retained |
| `sys_launcher.c`: `L` | `sizeof(*L)` = 58,904 in sim; row metadata, 17×256 LUT, pressed LUT, 320×64 RGB565 canvas, curve | First prewarm/open | Retained |
| Launcher: three icon sizes per row | `3 * 76 * 76` = 17,328 per app; MAXAPP=32 | Row bake | Retained |
| Launcher: name per row | `min(text_width+6,320) * min(line_height+6,64)` | Row bake | Retained |
| Launcher: three blurred icon/name variants each | `width * (cropped_y1-cropped_y0+1)` for each of six sprites per row; band height 92 | Row bake | Retained → offscreen variants evicted for card; visible rows plus margin retained |
| `dot.c`: `g_tab` | `(DOT_FONT_COUNT*NSLOT + DOT_ICON_COUNT) * sizeof(dglyph)` = 8,424 | First dot text/icon | Retained; transient failure now retries |
| Dot: `g_masks` | `161 * sizeof(dmask*)` | Prebaker start / first mask | Retained |
| Dot: each `dmask`, q=1..160 | `sizeof(dmask) + (S*S + 4*S)*nph*nph`; `S=2*floor(q/8)+3`, nph=4 through 16px, otherwise 2 | Background prebaker / first size use | Winning mask retained; duplicate race allocation previously leaked → freed |
| `faces.c`: `s_dc` | `sizeof(dc_cache)` = 37,944 in sim; 2,048 dots, bounds, 64 call entries | First generic dot-face capture | Retained |
| Faces: scaled glyph `px` | `w*h`, each dimension ≤255; up to 11 glyphs per scaled font | Scaled font preparation | Retained; failed temporary tap-table allocation previously leaked glyph → frees it |
| Faces: `tx` tap table | `2 * 255 * 4 * sizeof(int16_t)` = 4,080 | First scaled glyph | Retained |
| `face_segment.c`: `G` | `sizeof(seg_cache)` | First segment prewarm | Failed partially allocated cache leaked → rollback and retry |
| Segment: four quadrant sprites | Total `466*466*2` = 434,312 | Segment cache init | Retained on success; partial failure now frees earlier quadrants |
| `face_matrix.c`: `M` | `sizeof(mx_state)` = 4,500 | First matrix face | Retained |
| `nos.c`: `s_mb_work` | `sizeof(struct mb_work)` = 6,760 in sim | First live blur | Retained; failed allocation no longer disables live blur permanently |
| Engine: `match` | `1024*sizeof(uint16_t) + GFX_MAX_CMDS` = 2,368 | First unequal-length damage comparison | Retained; failure falls back to full damage |
| Engine: `s_scratch` | `466*466*2` = 434,312 | `nos_init` only if caller provides no framebuffer | Retained; **not allocated on normal striped firmware**, which passes stripe scratch |
| `svc.c`: `B` | `sizeof(svc_big)` = 9,800 | Service init | Retained; notification array + line + JSON tokens |
| `svc_track.c`: `T.pts` | `4096*sizeof(svc_track_pt)` = 98,304 | First track start | Retained even after stop; full track decimates in place |
| `sys_notifs.c`: `NL` | `20*sizeof(float) + 21*sizeof(float) + 20*sizeof(unsigned)` = 244 | First list layout | Retained |
| Notifications: `s_fly`, `s_dn` | Each `sizeof(svc_notif)` | First dismissal / detail | Retained |
| `sys_boot.c`: particles | `64*sizeof(particle)` = 4,096 | Boot screen enter | Retained |
| `fit_heart.c`: trace / illustration dots | `(420+64)*sizeof(hdot)` = 7,744 | Heart enter | Retained |
| `con_music.c`: snapshot | `sizeof(*snapshot)` = 144 | First widget snapshot | Retained |
| `time_timer.c`: snapshot | `sizeof(*snapshot)` = 396 | First widget snapshot | Retained |
| `con_memo.c`: `s_memos` | `24*sizeof(memo)` = 2,208 | First memo scan | Retained |
| `con_cell.c`: `s_sms` | `16*sizeof(hal_sms)` = 3,328 | First SMS refresh | Retained |
| `con_find.c`: ripple lists | `2048*sizeof(rip_dot)` = 32,768 **per list** | First ripple preparation for find phone/watch | Retained |
| `out_nav.c`: `L.xy` | `4096*2*sizeof(float)` = 32,768 | First nonempty track projection | Retained |
| Navigation: `N.dots`, `N.odots` | Each `360*2*sizeof(float)` = 2,880 | First current/old breadcrumb path | Retained |
| `out_baro.c`: `B.hist` | `144*sizeof(float)` = 576 | First history refresh | Retained |

## Firmware/platform inventory

These allocations join the same owner accounting; sizes unchanged unless stated.
Driver structs use target `sizeof` because platform configuration changes their
members. `mem` gives each instantiated driver's exact current size.

| Owner / site | Payload | Allocation / release |
|---|---:|---|
| `nos_main.c` stripe scratch | `466*16*2` = 14,912 | Boot; retained; now tracked |
| `cover_art` (`hal_esp32.c`, BLE or HTTP) | `250*250*2` = 125,000 | First media cover; shared between transports; retained |
| `ble_nus` assembly + ring | 2,048 + 8,192 = 10,240 | BLE service start; retained |
| `ext_audio.c` | `sizeof(audio_state)`; includes 8,192-sample playback ring, 16,384-sample record ring, transfer and synth state | Module init; retained |
| `ext_env.c` | `sizeof(*E)` | Sensor init; free on failed probe |
| `ext_light.c` | `sizeof(*Lt)` | Sensor init; free on failed probe |
| `ext_mag.c` | `sizeof(*M)` | Sensor init; free on failed probe |
| `ext_hr.c` | `sizeof(*H)` | Module init; retained |
| `ext_gps.c` | `sizeof(gps_state)` | Module init; retained |
| `ext_lte.c` | `sizeof(lte_state)` (including 12 SMS entries) | Module init; retained |
| `ext_task_stack`, `audio_task_stack` | 4,096 each, plus SDK-owned task metadata | `xTaskCreatePinnedToCoreWithCaps(...MALLOC_CAP_SPIRAM)`; persistent; payload and creation failures explicitly registered |
| `sd_xfer` | 8,192 | Each transfer; freed on completion/failure |
| `weather_http` | 16,384 | Each weather request; zeroed; freed afterward; formerly generic `calloc` eligible for PSRAM |
| `ota` upload/SD chunks | 4,096 each | During update; freed afterward; formerly generic `malloc` could spill to PSRAM |
| `ota` URL | `content_len+1`, maximum 513 | HTTP request → download task; freed on all failure/completion paths; now explicitly PSRAM instead of generic `malloc` |
| `native_menu_assets.c`: pack arena | 710,204 (B watch, level-0 sharp pack); 5,795,462 only in the simulator (full pack, host heap) | Boot (`nos_init`) or `native_rom 1` | Retained for the session; allocated with the tracked `hal_big_alloc` (owner `native_menu_assets.c`, plus a transient 88 KB file buffer and the 11 KB inflate state), so it counts against the tracked 6.8 MB budget and shows in `mem`. Not yet re-measured against the heavy-session peak on the board. On failure nothing stays allocated and the dynamic bake runs. See [SD_ASSETS.md](SD_ASSETS.md) |
| Allocator ownership header | 8 per live tracked allocation | Freed with payload; charged to budget; registry is a fixed 64-entry internal array (~1.3 KiB on device) |
| SDK/libc allocations | Variable; not source-owned `hal_big_alloc` calls | Wi-Fi/LWIP, NimBLE external allocator, mbedTLS external allocator, HTTP clients, FATFS LFN/stdio buffers, RTOS metadata. See `sdkconfig.defaults`; actual heap totals/minimum/largest block are printed by `mem` |

The four display DMA/rotation stripes are **internal/DMA RAM**, not PSRAM.
There is no runtime stb font rasterizer in the linked NOS core: font bitmaps
are baked flash data. Simulator windows, screenshots, GIFs, scripts, histories,
store copies, and simulated audio use the host heap and are excluded.

The legacy program is **not linked** by `firmware/main/CMakeLists.txt` or
`sim/nos_build.ps1`: `core/layer.c` full-screen layers, `core/wos.c` transition
buffers/recorder snapshots, `core/gfx_bench.c` benchmark buffers, old
`core/apps/*` launcher/name/mask/music/stack/Earth caches, and
`firmware/main/main.c`'s full framebuffer. They consume zero bytes in NOS.
`core/stb_truetype.h`'s default malloc/free macros likewise have no linked caller.

## Failure and eviction behaviour

`mem` prints owner, current bytes, peak bytes, live allocation count and cumulative
allocation failures. `info` prints per-owner failure counts alongside existing
blur coverage and frame-budget counters. Counts include denied-budget requests,
underlying heap failures and task creation failures; they survive frees/reopens.
Source/header names are used as owners, so orbit runs and zoom controls are
separate from the face state. A failed optional cache remains retryable.

Important corrected failure paths:

- Partial card allocation leaked/latched a failed `C.b`; it now rolls back every
  allocation. Textures are paged, large sprites/LUTs allocate before the pages,
  and failed night-page allocation rolls back and retries. Closing defers cleanup until no outgoing frame can reference it.
  A covering screen keeps the card alive for reveal.
- Page staging/slot failures, live-blur scratch, launcher, dot glyph table and
  scaled/segment font failures previously disabled features for the rest of boot.
- Notification eviction scanned only the first occupied slot; it now compares
  every candidate. Its bake window follows the visible cards instead of only the
  newest notifications.
- Dot prebaker/UI races lost the losing mask allocation. Segment partial sprite
  allocation and scaled-font tap allocation also leaked.
- Orbit growth held old and new pairs simultaneously; invalid old storage now
  goes away before growth. Idle eviction keeps the published scene and releases
  sorting scratch, avoiding a measured first-return-frame regression.
- A small Settings cache alone reduced acceptance-script coverage to 24/59 fast
  frames. Bounded moving bake slices now request only the needed level, and
  uncached bands use budgeted reduced live blur **without redrawing cached bands**.
  Notifications likewise fall back per missing card. The existing 26 ms CPU guard
  remains in force; refusal remains visible in `info`, rather than being hidden.

## Validation and remaining board checks

- `sim/nos_build.ps1 -Strict` succeeds with `C:\mingw64\bin` on PATH.
- `tools/test_mblur.ps1` passes kernel, gate, page recovery/release, and notification
  slicing checks. `tests/psram_test.c` checks ownership, 8-byte alignment,
  overflow, budget denial, fault accounting and free/reuse.
- `python tools/test_psram.py`: heavy repeated Earth opens, cover/reveal, 24
  separate card-allocation failures with successful reopen, all 16 night-page
  failures with recovery in place, and twenty notifications scrolled deep into
  the list all pass. A 512 KiB maximum-allocation test also passes (no contiguous
  megabyte required). Card bytes return to zero;
  heavy-session allocation failures remain zero.
- All 11 `tools/orrery/*.txt` scripts pass and remain pixel-identical to the saved
  pre-change simulator. All eight card scripts remain pixel-identical. Seven
  card scripts pass; **`card_face_idle.txt` already fails on the baseline and still
  fails unchanged**: 10 late frames in `rings`, 40 in `zones`. This is not claimed
  as a passing performance test.
- Both `tools/nos_blur*.txt` pass. `nos_blur_pages` is pixel-identical. Settings
  coverage is **59/59** fast frames, notifications **11/11**, no late frames in
  the acceptance sections. `nos_blur` differs during cache misses because those
  bands use reduced live filtering instead of the precomputed box-filter copy.
- The twenty-notification stress run blurs **179/185** fast frames; six are still
  rejected by the existing CPU-budget guard. The old “newest cards only” policy
  no longer permanently excludes older cards. This remaining CPU limit is visible
  in coverage/gate counters, and is not an allocation failure.

No ESP-IDF firmware build was run in this session. Simulator costs are not new
1.43-B measurements. Claude should check `mem`/`info` after the
same heavy session and during Earth loading, and compare Settings/older-notification
fallback cost, home pager and Activity with the reported 45 ms / 14-of-47 board
results. The projected free-space range above is conditional on the measured
platform consumers staying within the allowance; the actual heap telemetry is
the acceptance authority.
