# All startup initializers complete on Windows and ARM64

The strict native runtime now completes all four C and all 959 C++ initializer
callbacks on Windows x64 and the physical Snapdragon 680 tablet. Both stop at
the same next operation, `GetStartupInfoW`, in the remaining CRT startup path.
This is a startup milestone; the game has not booted or produced a content frame.

| Measurement | Each platform |
| --- | --- |
| Prepared blocks / instructions | 72,000 / 342,968 |
| Translation units | 174 |
| Executed block visits | 65,870 |
| Imported API dispatches | 3,151 |
| C / C++ callback returns | 4 / 959 |
| Stop | `missing_import`, `GetStartupInfoW`, IAT `0x0221a254` |
| Stop site / ESP | `0x00e3d093` / `0x0effef74` |

The complete published stop records match between the two compiled targets,
including stub counts and stack position. This does not independently prove
the entire translated startup path equivalent to original x86 execution.

## Reached fixes

**Scalar MOVSD.** The pinned emitter's recovery formatter treated every MOVSD
mnemonic as an implicit string instruction, erasing the operands of legacy
SSE2 scalar double moves. The game consequently read address zero through
ESI at `0x00523331`. The consumer now retains SSE operands and implements the
three legacy forms: memory loads clear the upper 64 bits; register copies
preserve the upper 64 bits; memory stores write exactly eight bytes. Unaligned
64-bit memory operands remain valid. Implicit string MOVSD remains separate.

An authored program checks memory/register/store forms, upper lanes, unchanged
ESI/EDI and arithmetic flags, unaligned data, and a subsequent real string
MOVSD with pointer advancement. A negative program checks unmapped input.
Both pass compiled execution on Windows and physical ARM64, plus direct original
x86 instruction execution on Windows. The existing three PADDD programs also
retain their direct-x86 checks.
The oracle relocates absolute fixture data pointers while preserving relative
control-flow displacements.

**Exception message ownership.** Native `__std_exception_copy` duplicates an
owned nonnull message into the guest heap, retains borrowed pointers, clears
ownership for null messages, and leaves the destination unchanged if allocation
fails. `__std_exception_destroy` frees owned storage and clears the pointer and
one-byte flag while preserving structure padding. Six fixtures cover ownership,
mutation independence, null data, allocation failure, invalid source and a
read-only destination. This is exception data lifetime support; throwing,
unwinding and exception-handler delivery remain unimplemented.

The five-byte field writes and failure behavior were inspected in x86
vcruntime140.dll 14.51.36247.0, exports at RVAs `0x4af0` and `0x4b60`, SHA-256
`2fa6efc053203460a23d3a25158f227d895d2dadc63acc1a372da97c3a4281c3`.

**Optional link capability queries.** Only the kernel module's exact
`CreateHardLinkW` and `CreateSymbolicLinkW` queries return null with error 127.
Their inspected wrappers check null and pass error 50 to the error helper.
This does not acknowledge any filesystem mutation or establish a copy fallback.
The [versioned record](../profiles/startup-compatibility-dcd883dd.json) lists
arguments, outputs, addresses and limitations. Both query hits are counted and
logged on first use. Wrong-case exports and wrong modules still stop explicitly.

**Wireless selection.** ADB can report an mDNS serial containing a space, such
as a collision suffix `(2)`. Device selection now preserves the tab-delimited
serial rather than splitting that name into words. A regression test covers
automatic and explicit selection of that format.

All 42 tooling tests and all 58 compiled scenarios pass on Windows and physical
ARM64 using the same runtime source. Five of those authored SIMD programs also
pass their direct-x86 return/fault checks. The actual 963-initializer device run
above has completed successfully up to its explicit next stop.

## Identities and remaining work

- Main x86 SHA-256:
  `dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
- recomp-kit: `487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d`.
- Runtime C SHA-256:
  `637214b42d24a1909af18e6fe937bc1ed9f2ffcdb219f9b90d9a700572aa1f82`.
- Canonical compact sorted translation hash-map SHA-256:
  `f27dd9f5e70861307d98778c30adb1fb44c741d6f88b6436b6c29b478bb600f3`.
- Android executable SHA-256:
  `4395bcfba252902259d964ec53c6c0b02efc257b8092447a3c9332e8d052eba3`.

Device: Xiaomi 23073RPBFG / SM6225, Android 15 / API 35. Private result:
`.work/startup-959-constructors.json`; device hashes were checked and temporary
files removed. No original bytes or generated translations are published.

Remaining work includes the reached CRT/Win32 startup calls, native DLL loading
and required ABI coverage, original graphics/audio/content, the full in-process
amdaemon replacement, calibrated touch controls and playable timing. The
[D3D9 pixel shader proof](d3d9-shader-2026-11-01.md) is still separate from game
rendering and presentation.
