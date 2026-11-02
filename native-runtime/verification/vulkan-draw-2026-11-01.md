# Selected-driver Vulkan draw and readback

On 1 November 2026, the diagnostic APK rendered with both drivers on the Xiaomi
23073RPBFG (Snapdragon 680 / Adreno 610, Android 15 / API 35).

| Result | System Qualcomm | App-private Turnip |
| --- | --- | --- |
| Vulkan version | 1.1.128 | 1.3.352 |
| Device extensions | 70 | 140 |
| Graphics queue and logical device | Created | Created |
| Matching red pixels after draw | 16 / 16 | 16 / 16 |
| Center RGBA | 255, 0, 0, 255 | 255, 0, 0, 255 |
| Coherent readback memory | Yes | Yes |

Each mode runs in a fresh process. Every Vulkan entry point comes through the
selected driver's instance/device lookup functions. A 4x4 RGBA8 attachment is
cleared blue, then a vertexless fullscreen triangle draws opaque red using
the included original GLSL/SPIR-V shaders. A render-pass dependency makes color
writes visible to the transfer, followed by a staging-buffer copy, a host-read
barrier, a fence wait and pixel comparison. Noncoherent memory has an explicit
invalidation path, but these two device runs selected coherent memory.

The runner requires all draw fields and the requested driver mode to agree;
creating a device alone cannot pass. It also rejects `../outside.so` before
loading and verifies that the custom driver's identity differs from the system
driver. Turnip adds 79 extensions absent from the stock list. Extension counts
do not establish support for a particular D3D9/DXVK implementation.

APK SHA-256:
`fde8956a58d08cf40ebe51b968859446d43b5c44ecd6010869cd24d4749a9a10`.
Driver archive SHA-256:
`607338ec3e4ba06e659cbbb6ca4653265a5bf096bfb217050d18700d60b1083a`.
Build: Android NDK 27.2.12479018, SDK 35, Java 17, pinned libadrenotools
`8483dfdaa2abf97ee89ad0e5f337e7b508550c6b`. Raw reports remain private in
`.work/vulkan-draw-results.json`.

This proves a generic offscreen Vulkan draw, not original game content, D3D9
shader translation, presentation, audio or playable timing. The fence has a
10-second wait limit; Vulkan cleanup can still wait on a stalled queue. No
driver-stall or noncoherent-memory fault was injected. The next graphics gate
is the D3D9 shader and guest COM bridge using the same selected driver.
