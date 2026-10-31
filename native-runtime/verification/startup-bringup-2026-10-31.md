# Original entry / CRT bring-up

The new bounded AOT runtime starts at the pinned game's actual PE entry,
`0x00e3c07c`, follows its ILT/security initializer and CRT startup, and executes
translated static-initializer callbacks. It is a shell executable on Android,
not yet the game APK or a completed boot.

## Measured checkpoint

| Property | Result |
| --- | --- |
| Prepared AOT blocks | 4,096 |
| Prepared instructions | 19,774 |
| Executed blocks before stop, Windows x64 | 271 |
| Executed blocks before stop, physical Android ARM64 | 271 |
| Native API calls before stop on each | 21 |
| Stop on each | `CreateEventW`, IAT `0x0221a314`, from `0x00e3b4b0` |
| ESP on each | `0x0effef98` |
| Full boot, original frame/audio or gameplay | Not verified |

Both platforms execute compiled translations in this run. Matching traces are
cross-platform consistency evidence, not a full original-x86 startup oracle.
The earlier 4,096-case hardware-reference comparison remains separate.
The first two C initializer callbacks returned and execution entered the third.

Input SHA-256:
`dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
recomp-kit: `487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d`.
Generated block C SHA-256:
`815f6ff482474fcd2a18575db8821d1141257674ce153b633560c288ac6a3f25`.
ARM64 executable SHA-256:
`ff2ed05e181f8381fc0a1e8d8ee3da033d6ae7deda39968c8b7eca017c7e8dd2`.
Device: Xiaomi `23073RPBFG`, SM6225, Android 15 / API 35. Toolchains:
llvm-mingw clang 23.1.4 and Android NDK 27.2.12479018.

## Runtime boundary

- Compiled basic blocks dispatch through guest EIP and a checked guest stack;
  no kit interpreter, permissive import runtime or general loader is linked.
  Every branch, including self-loops, returns to the execution budget.
- The PE's sections/IAT and main-image TLS data are mapped. The inspected main
  TLS callback list starts with null. TEB/FS state preserves the guest SEH chain;
  host-frame SEH instrumentation is disabled and exception dispatch is pending.
- Bounds/permissions are inserted into all 15 pinned-header memory accessors,
  including accesses from inline string/stack helpers. Only a private generated
  header copy is changed. The pinned checkout is unchanged.
- Native `_initterm`/`_initterm_e` retain callback continuation frames, cdecl
  argument ownership and the nonzero-result short circuit. Four actual C table
  members are extra roots: `0x00e3bd95`, `0x00e3be62`, `0x00e3b418`, `0x013179d0`.
- The preparation is deliberately bounded: 245 direct targets remain pending;
  indirect sites are reported separately. One guarded XGETBV instruction is
  unimplemented and was not reached under the baseline feature profile.

The consumer adapter also fixes the pinned recovery formatter's missing
16-byte operand width and a half-vector store lowering that unnecessarily read
the memory destination. Synthetic SIMD load/store checks cover these paths.

## Explicit startup stubs and limits

The checkpoint used one optional-CPU-feature query returning false, an empty
process environment initialization, and three unavailable optional condition
variable exports. The latter select the inspected CRT legacy path; they are
`InitializeConditionVariable`, `SleepConditionVariableCS`, and
`WakeAllConditionVariable`. All counts are in the local run report.

Clocks, basic CRT configuration, list-head initialization, the reached 53-bit
control-word request, exit-callback registration and critical-section object
initialization have native adapters. Full synchronization/threading, exit callback
execution, locale conversion and general floating-point behavior remain pending.
The critical-section wrapper's reached ABI is cdecl, not Win32 stdcall.

Validation: 27 host tooling tests pass. Twelve compiled synthetic scenarios pass
on Windows and the physical ARM64 tablet: loop/call/return, self-loop budget,
inline-helper memory guard, read-only writes, 53-bit control state, full/half
SIMD stores, critical-section initialization, kernel module lookup, optional
export error state, `_initterm_e` short circuit and `_initterm` full iteration.
CI runs the same synthetic programs on Linux without any original game inputs.

The callback traversal follows [Microsoft's CRT contract](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/initterm-initterm-e).
The reached FP setup is limited to the documented [53-bit control request](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/controlfp-s);
it does not qualify 24/64-bit arithmetic or exceptions.

Original code/image data, generated binaries and raw run logs remain in ignored
`.work/startup-bringup/`. The runner checks device artifact hashes and removes
its temporary Android files. No original installation, account or game save
was modified. The next work is the reached synchronization call, then remaining
CRT constructors/imports and the separate early GPU draw/readback gate.
