# SVG layers on the 466×466 watch

Assessment, 2026-09-14. No SVG engine has been added to the firmware.

SVG is useful as an authoring format for the dial's sections, paths and outlined
lettering. The runtime cost depends on when those vectors are rasterized and
how often the resulting layers are composited.

| Approach | Work on the ESP32-S3 | Suitability |
|---|---|---|
| Rasterize and flatten during the PC build | Copy cached pixels; draw live values and hands | Best starting point for fixed-resolution dial artwork |
| Parse/rasterize on face selection, then flatten | One-time SVG loading, path rasterization, blending and conversion; reuse the result afterward | Useful for downloadable faces or runtime theme changes |
| Retain paths and rerasterize changed sections | No repeated XML parsing; rasterization on affected regions | Possible with an engine and measured refresh budget |
| Rerasterize and blend all layers every frame | Repeated path coverage, antialiasing, compositing and display traffic | Highest cost; unnecessary for static dial furniture |

Our native renderer already takes the cached approach: Ultra's vector geometry
and font outlines are rasterized into one RGB565 base; only its hands render
each animation frame. Currently a changed battery/steps/date value also
rebuilds that base. A future SVG background should separate fixed artwork from
those changing values so a step increment does not rerasterize all SVG paths.

## Memory and workload arithmetic

These are format calculations, not measured performance results:

- Pixels: `466 × 466 = 217,156`.
- One opaque RGB565 image: **434,312 bytes / 424.1 KiB**.
- One RGB565 image plus an 8-bit alpha plane: **651,468 bytes / 636.2 KiB**.
- One RGBA8888 scratch/layer image: **868,624 bytes / 848.3 KiB**.
- Two RGBA8888 layers plus one flattened RGB565 result: **2,171,560 bytes /
  2.07 MiB**, before vector geometry, renderer scratch space, other framebuffer
  buffers and application memory.
- Rendering layers sequentially into reusable scratch storage can reduce the
  peak; flattening lets us discard the source pixel buffers afterward.
- Two full-screen layers composited at 60 fps visit **26,058,720 layer pixels/s**;
  at 30 fps, **13,029,360/s**. This does not include path rasterization, blending
  instructions, memory reads/writes, or display transfer. Dirty rectangles and
  cropped layers can substantially reduce that workload.
- A full-screen RGB565 transfer alone carries **26.1 MB/s at 60 fps**, or
  **13.0 MB/s at 30 fps**, before bus/protocol overhead. Static images cannot
  remove the cost of transmitting any pixels that actually change.

This firmware configures a 240 MHz ESP32-S3 and 8 MB octal PSRAM at 80 MHz;
PSRAM capacity is shared with the rest of the system. Existing layer slots can
already consume about 1.66 MiB. These figures do not prove a specific SVG scene
will fit or sustain a particular refresh rate.

## Engine choices and a timing estimate

[ThorVG](https://github.com/thorvg/thorvg) supports SVG, software rendering,
clipping/compositing and embedded targets including ESP32. It is worth evaluating
if on-device vectors become a requirement. Its published desktop benchmarks do
not provide timing for this particular board, resolution and artwork.

[NanoSVG](https://github.com/memononen/nanosvg) offers a small parser/rasterizer,
but its own documentation notes feature and performance limitations and says
the project is not actively maintained. It is not a drop-in replacement for
browser SVG rendering.

No trustworthy millisecond number is available without benchmarking a candidate
scene on the physical watch. Measure parsing separately from cold rasterization,
cached compositing and dirty updates; log peak heap/PSRAM usage, allocation
failures and display transfer. Compare the result against the whole-frame budget
of **16.7 ms at 60 fps** or **33.3 ms at 30 fps**, allowing time for other work.

Recommended next step if SVG authoring is adopted: create static SVG dial groups,
render them during the PC build, flatten to one RGB565 image, and keep live
complications and hands in the existing renderer. Preserve editable SVG sources
and font licenses; evaluate on-device SVG separately if runtime editing is needed.
