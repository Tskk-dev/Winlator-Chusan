# Physical-device verification — 31 October 2026

Target: Xiaomi `23073RPBFG`, SM6225 / Adreno 610, Android 15 (API 35), ARM64.
No device serial number, account, endpoint, original game bytes or private
configuration is included in this report.

## Isolated CPU translation proof

The version-pinned original routine at `0x00595550` in the
`chusan-local-dcd883dd` executable is nine bytes / four instructions.
The emitter admits only its bounded two-load/add/return pattern.

| Check | Result |
| --- | --- |
| Original x86 bytes versus generated C on Windows x86 | 4,096 cases; zero EAX or defined ADD-flag mismatches |
| The same C compiled to Android AArch64 | Successful |
| AArch64 execution on the physical tablet versus original-x86 vectors | 4,096 cases; zero mismatches |
| Invalid guest-memory range | Rejected by the bounded native probe |

Original executable SHA-256:
`dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
Reference vector SHA-256:
`f0da2aab703b1c249336a941b42783c06f56711a1df6d0d9e6fd576b2781adcc`.
ARM64 test executable SHA-256:
`c19f7955e5d3cd7e48234fbfefcfb60953d307c15b8684eb4d243df7bdc2c710`.

This verifies one isolated computation with valid inputs, not a complete ABI,
exception model, function-discovery system, translator, scheduler or game boot.
The caller meaning of this routine remains unclassified. It is not a speed test.

## Vulkan driver loading through libadrenotools

The debug diagnostic APK was built from commit
`f45ae83473cebffc10c0faea11b6ca7a67c962b6` and installed through authorized ADB.
Each driver test ran in a separate app process. The custom library was copied
only into that application's private files directory. No global driver or
device security setting was modified.

| Observed property | Stock driver | App-local custom driver |
| --- | --- | --- |
| Driver identity | Qualcomm Technologies Inc. Adreno Vulkan Driver | Turnip |
| Reported Vulkan API | 1.1.128 | 1.3.352 |
| Device extension count | 70 | 140 |
| Vulkan instance created | Yes | Yes |
| Graphics-queue logical device created | Yes | Yes |
| Game frame or draw verified | No | No |

There are 79 custom-driver extensions absent from the stock list; the custom
list also omits nine stock extensions, so these are not strict supersets.
Newly reported examples include `VK_KHR_dynamic_rendering`,
`VK_KHR_timeline_semaphore`, `VK_KHR_load_store_op_none`,
`VK_EXT_depth_clip_enable`, and `VK_EXT_graphics_pipeline_library`.
Enumeration does not verify every advertised feature or extension operation.

A custom metadata entry naming `../outside.so` was tested on the tablet and
rejected as an invalid library basename before loading. The original valid
metadata was restored and the successful Turnip probe rerun.

The Turnip package came from the existing repository asset
`app/src/main/assets/graphics_driver/adrenotools-turnip26.2.0.tzst`.
Its SHA-256 is
`607338ec3e4ba06e659cbbb6ca4653265a5bf096bfb217050d18700d60b1083a`.
The APK SHA-256 is
`6a84506e6b02553543893fb6f3c157744da5d006eae4022185f0ffb9b942c41f`.
The [CI build](https://github.com/Tskk-dev/Winlator-Chusan/actions/runs/41181616482)
passed both the Android build and 17 tooling tests. Dependency notices and all
required hook libraries were confirmed inside the APK.

The current upstream [DXVK requirements profile](https://github.com/doitsujin/dxvk/blob/master/VP_DXVK_requirements.json)
also requires `VK_KHR_maintenance5` and `VK_KHR_maintenance6`, neither of which
this tested Turnip package reports. A Vulkan 1.3 version string alone therefore
does not qualify this package for unmodified current DXVK. A compatible backend
or driver still needs selection and actual rendering tests.

Full machine-readable host/device/driver evidence remains in ignored `.work/`.
The complete game, D3D9/Cg/E-mote integration, audio, services and playable
performance remain unverified.
