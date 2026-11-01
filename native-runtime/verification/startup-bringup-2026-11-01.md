# C++ startup checkpoint on Windows and physical ARM64

The pinned entry-point translation completes all four C initializers and 239
C++ initializer callbacks on both platforms. It then stops at the missing
`_mktime64` import. The complete game has not booted.

| Evidence | Windows x64 and Android ARM64, each |
| --- | --- |
| Prepared blocks / instructions | 28,000 / 134,856 |
| Executed block visits | 12,980, including repeats |
| Imported API dispatches, including the stop | 517 |
| Completed C / C++ initializer callbacks | 4 / 239 |
| Next unsupported import | `_mktime64`, IAT `0x0221b19c` |
| Calling block / instruction | `0x005f89a9` / `0x005f8a05` |
| ESP | `0x0effef24` |
| Full boot, original frame/audio, gameplay | Unverified |

The C++ initializer table contains 959 non-null callbacks. These are startup
callbacks, not a count of all functions required by the game. Both executions
run compiled C translations; agreement is cross-platform consistency evidence,
separate from the earlier 4,096 original-x86 initializer comparisons.

## Implemented behavior

- Manual/auto-reset events retain signal state, named-object sharing and
  independent generation-tagged handles. Closed handles do not revive when a
  slot is reused. An unsignaled zero-time wait returns timeout; blocking waits
  stop explicitly until a scheduler exists.
- Critical sections retain primary-thread ownership and recursion. Releasing
  the last acquisition clears the owner. Invalid use after deletion stops.
  Win32 stdcall and the vcruntime cdecl initialization wrapper use different
  argument cleanup.
- Win32 TLS allocation/get/set/free uses separate inline and expansion slots,
  preserving the module's static TLS. Get-value clears last-error on success.
- Exception-filter registration returns the previous filter. Guest exception
  delivery is still unsupported and stops explicitly.
- Checked native memset/copy/move operations and ordinary/aligned allocations
  now serve reached constructors. The heap recycles freed ranges, checks
  allocation kind, rejects double frees and returns ENOMEM for exhaustion.
  It currently has a 176 MiB arena and 8,192 live allocation records.

The existing optional stubs remain: one baseline feature query, one empty
environment setup and three unavailable condition-variable exports. These
select inspected CRT paths. No new service success stubs were added.

## Discovery and build

Preparation validates the original initializer-table bounds and callback
targets. The incremental-link table at `0x00401005..0x00471715` contains 92,112
five-byte relative jumps. Each jump and destination is checked before immediate
callback pointers in admitted MOV/PUSH instructions seed further translation.
The observed guard thunk `0x0041dd86` is also an explicit root. Other indirect
targets remain unknown until supported by code/data or execution evidence.

The bounded graph retains 119 pending direct targets and 74 guarded translation
errors. No unsupported-instruction guard fired in this checkpoint. Compilation
uses 120 cached translation units and a response file to avoid the Windows
command-length limit. There is no interpreter fallback or default success for
unknown calls.

Validation: all 33 tooling tests pass. All 29 compiled synthetic scenarios pass
on Windows and the physical tablet, including the event modes/lifetimes, TLS
expansion and last-error behavior, recursive locks, exception-filter state,
memory primitives, allocation alignment/independence/exhaustion and double-free
rejection. Existing control-flow, callback, memory-protection, SIMD and control-
word checks also pass. The linking fixture includes an output path with a space.
Every case used the runtime source identity below.

## Identities and remaining work

Original x86 executable SHA-256:
`dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
recomp-kit: `487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d`.
Runtime C SHA-256:
`19f45e8b0a12494dafaa4fc0b4ac040723d1da997560d7ecf9757975227fab43`.
Canonical translation-unit hash-map SHA-256 (sorted compact JSON):
`5395748623f24a006c6ec2518102c062ff1d6f8c614c6adfc6d5deaee0cfbda5`.
Android executable SHA-256:
`6b25261871c41f9386dcb5c68abe1befc9a7eac10ad711e08a9869ea2a82b7eb`.

Device: Xiaomi 23073RPBFG, SM6225, Android 15 / API 35. Toolchains:
llvm-mingw clang 23.1.4 and Android NDK 27.2.12479018. Private source images,
translations, binaries and reports remain under `.work/startup-expanded/` and
`.work/runtime-tests/`. Device artifacts were hash-checked and temporary shell
files removed. The original installation was unchanged.

Next: implement the reached calendar-time conversion, continue the remaining
constructors/imports, and integrate D3D9 using the selected driver. The separate
[generic Vulkan draw](vulkan-draw-2026-11-01.md) passes on both drivers. Original
content/audio, the full in-process amdaemon replacement, touchscreen integration,
save/relaunch behavior and sustained playable timing remain required.
