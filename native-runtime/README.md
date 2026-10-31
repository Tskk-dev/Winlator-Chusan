# Native Android runtime investigation

This is the start of a separate native CHUNITHM project in Winlator-Chusan.
The Windows-container overlay work is deferred. Nothing in this directory
currently boots CHUNITHM or replaces Wine in the existing Android app.

## Verified on 31 October 2026

- Read-only inspection recorded 25 top-level PE executables and DLLs from the
  local installation. The main executable is x86; amdaemon is x86-64.
- `chusanApp.exe` imports 812 functions across 35 DLLs and has a TLS directory.
  Declared dependencies include modern MSVC/UCRT, D3D9/D3DX9, Cg, E-mote,
  WinHTTP, sockets, threads and dynamically resolved APIs. Import counts are a
  lower bound: they omit COM methods and dynamically discovered interfaces.
- The main SHA-256 is
  `dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
  It matches neither executable recorded in the supplied iPad plan. Its game
  version is not independently established. Addresses, translated functions and
  validation results from that plan cannot be reused as evidence for this input.
- An isolated four-instruction source routine at `0x00595550` was checked on
  Windows against a bounded C translation: 4,096 input cases, zero EAX or
  defined ADD-flag mismatches. The original bytes run directly on x86 hardware.
  The emitted C also compiles to a freestanding Android AArch64 ELF object.
- **Not verified:** execution on Android, a complete calling convention or
  fault model, function discovery, indirect control flow, SSE/x87, DLL startup,
  game boot, original graphics/audio, network/service behavior or playable speed.
  No Android tablet is currently connected. This is not an ARM64 execution test.

The profile records identities and the bounded probe only. The generated source,
original routine bytes, executable, ARM64 object and reports stay in ignored
`.work/`. No original game bytes or assets are committed.

## Architecture to investigate

```mermaid
flowchart TD
    A[Private version-pinned Windows inputs] --> B[Offline analysis and x86 translation]
    B --> C[Compiled Android ARM64 game code]
    D[Android launcher, settings and touch overlay] --> E[Native runtime boundary]
    C --> F[32-bit guest memory, CPU state and Win32 compatibility]
    F --> E
    E --> G[D3D9 and middleware adapters to Vulkan]
    E --> H[Native audio and measured playback clock]
    E --> I[In-process cabinet and amdaemon service contracts]
    J[Private game and option content] --> F
```

Keep guest addresses as 32-bit values and translate them through a validated
memory arena; never cast them to Android host pointers. Unsupported calls and
untranslated destinations must produce explicit diagnostics. Successful-looking
placeholder services are not a working runtime. The native service adapter is
a new implementation; replacing amdaemon removes the need to translate that
x86-64 process, but its observed interfaces still have to be reproduced.

The iPad design provides useful boundaries and acceptance criteria. UIKit,
Metal and the iOS audio/lifecycle implementation do not carry over. The Android
host target is NDK `arm64-v8a` with Vulkan and a native audio backend. Oboe is
a candidate for Android audio; it does not decode CRI content or establish the
game's audio clock by itself. Device capability and latency probes come first.

For input, retain the requested top-aligned 16:9 image, calibrated narrower
GROUND SLIDER, full-width six-band Sensing Range, transparent ground interiors,
LEDs clipped below the image, and modal input cancellation with fresh-touch
resumption. The game/service boundary should receive direct native input;
Brokenithm remains a behavior/protocol reference for the deferred Winlator route.

## Reuse assessment

| Candidate | Relevant existing work | Remaining CHUNITHM work |
| --- | --- | --- |
| recomp-kit, inspected commit `487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d` | x86-to-C translator, guest memory/loader, Win32 shims, Android/SDL3 target | Its documented first-version scope excludes shader-model D3D8/9. The inspected DirectX adapters are for older interfaces. D3D9/Cg/E-mote, this compiler/runtime, services and timing need separate qualification. Not adopted yet. |
| Remill | x86 instruction lifting to LLVM bitcode | Not an application compatibility layer; needs discovery, ABI bridges, runtime, graphics, audio and services. |
| McSema | binary control-flow recovery and lifting tooling | Upstream repository is archived; not the default foundation for a new port. |
| DXVK Native | D3D9/11 translation to Vulkan without Wine; SDL3/SDL2/GLFW presentation seams | Native Android build, device/driver features and 32-bit guest COM to 64-bit host bridges must be proven. Desktop/native support is not proof of Android compatibility. |

Starting the project from scratch does not require writing every library from
scratch. Qualify a reusable translator/runtime before committing to either a
custom implementation or a fork. The current probe intentionally supports one
small instruction pattern and is not that translator selection.

## Next observable gates

1. Run the ARM64 routine proof on the actual Snapdragon 680 tablet; compare a
   deterministic vector set and record the OS, ABI and device identity.
2. Qualify a translation foundation with real routines covering memory,
   branches, indirect callbacks, integer flags and floating point. Fail on an
   unknown instruction or target rather than accepting a silent stub.
3. Establish the pinned game's entry/CRT/TLS/DLL startup closure and implement
   the contracts it actually reaches. Record missing addresses and imports.
4. Prove one original D3D9 draw and a sound-content path on Android. A native
   clear or silence-only audio fixture is a separate, smaller result.
5. Reproduce the observed cabinet/service boundary and connect the native
   overlay, LEDs, card and credit actions. Keep account configuration private.
6. Complete a real chart and validate audio clock, frame pacing, interruptions,
   saved state and sustained thermal behavior on the target tablet.

Snapdragon 680 / Adreno 610 performance is unknown. Ahead-of-time compilation
does not establish native-source speed, 60 FPS or accurate rhythm timing.

## Reproduce the tooling checks

From the repository root, with Python 3.12 or later:

```powershell
python -m venv native-runtime/.venv
native-runtime/.venv/Scripts/python -m pip install -r native-runtime/requirements.txt
native-runtime/.venv/Scripts/python -m unittest discover -s native-runtime/tests -v
native-runtime/.venv/Scripts/python native-runtime/tools/inventory.py --bin-dir '<private game bin>' --profile native-runtime/profiles/chusan-local-dcd883dd.json --output native-runtime/.work/inventory.json
```

On Linux/macOS use `native-runtime/.venv/bin/python`. The inventory refuses
writing into the source installation; a profile mismatch exits with status 2.
It scans top-level PE files only and does not claim a complete dependency graph.

The isolated hardware-reference probe currently needs Windows and llvm-mingw
(tested with clang 23.1.4). All generated output stays outside the game folder:

```powershell
native-runtime/.venv/Scripts/python native-runtime/tools/probe_routine.py --exe '<private game bin>/chusanApp.exe' --profile native-runtime/profiles/chusan-local-dcd883dd.json --clang '<llvm-mingw>/bin/clang.exe' --output native-runtime/.work/routine-probe
```

The host runner invokes only the validated nine-byte isolated routine, not
the game entry point or DLL initializers. It compares EAX and six defined
arithmetic flags for fixed boundary and deterministic random inputs. It does
not establish complete x86 semantics, the original routine's caller behavior,
an Android ABI bridge, graphics, audio or full-game translation.

## Primary references

- [recomp-kit](https://github.com/veritr1x/recomp-kit) and its
  [first-version exclusions](https://github.com/veritr1x/recomp-kit/blob/487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d/docs/superpowers/specs/2026-09-13-recomp-kit-design.md).
- [Remill](https://github.com/lifting-bits/remill) and
  [McSema](https://github.com/lifting-bits/mcsema).
- [DXVK Native](https://github.com/doitsujin/dxvk#dxvk-native).
- [Android ABIs](https://developer.android.com/ndk/guides/abis),
  [Vulkan](https://developer.android.com/ndk/guides/graphics),
  [Oboe](https://developer.android.com/games/sdk/oboe).
- [Snapdragon 680 specifications](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/product_brief_-_snapdragon_680_4g_mobile_platform.pdf).

The supplied iPad plan was used as an architectural reference. Its source code
is unavailable here; none of its reported execution checks were rerun or adopted.
