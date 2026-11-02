# Host startup continuation

This intermediate Windows x64 checkpoint completed all four C and 674 of 959
C++ initializer callbacks. At that point the last physical ARM64 checkpoint was
[425 C++ callbacks](constructor-runtime-2026-11-01.md), with a new comparison
pending. It is superseded by the [963-initializer checkpoint on both platforms](startup-963-initializers-2026-11-01.md).
Full game boot remains unverified.

| Measurement | Windows x64 |
| --- | --- |
| Prepared blocks / instructions | 72,000 / 342,968 |
| Translation units | 174 |
| Executed block visits | 54,143 |
| Imported API dispatches | 2,753 |
| C / C++ callback returns | 4 / 674 |
| Stop | `dynamic_export`, `CreateHardLinkW` |
| Stop site | `0x00521d91` |
| ESP | `0x0effefac` |

The next reached operation is `GetProcAddress` for `kernel32.dll`'s
`CreateHardLinkW`. It stops explicitly; filesystem capability or hard-link
creation has not been claimed. There are 285 C++ callbacks remaining. These
counts measure startup only, not the fraction of the complete port finished.

## Translation changes and qualification

Retained batches now prioritize direct control flow from an observed missing
target, walking through already-retained blocks before expanding unrelated cold
callback tables. Three synthetic graph cases verify ordering, traversal through
retained blocks and termination on retained loops. All earlier block entries
are preserved. The 72,000-block preparation still records 4,396 pending direct
targets and 86 guarded blocks; no unsupported-instruction guard fires in this
host execution.

The reached legacy SSE2 `PADDD` XMM form now snapshots the four source lanes,
adds modulo 2^32 without modifying arithmetic flags, checks the full memory
operand before register writes, and stops on unaligned memory. Register-source,
memory-source, aliasing, overflow, preserved flags and all eight resulting lane
values are checked by a synthetic program. Two negative programs exercise
unaligned and unmapped operands. All three pass on Windows x64 and physical
ARM64; direct x86 execution of those authored programs supplies an independent
return/fault check. This is not an original-game startup differential test.

Generated shuffle code deliberately snapshots discarded lanes. Only generated
translation units allow unused-variable warnings; consumer runtime code keeps
the existing warnings-as-errors policy. No runtime guard is removed.

`GetModuleHandleA` now shares the existing scoped module lookup with the wide
variant: null returns the main image, and case-insensitive `kernel32.dll`
returns the compatibility module handle. Unknown modules still stop until their
loader behavior exists. Four host fixtures verify ANSI/wide input, case folding,
exact handles, null lookup and stdcall stack cleanup.

All 41 tooling tests and 46 compiled host scenarios pass. The new ARM64 startup
comparison is pending. The three new PADDD cases passed on the tablet before
the module lookup addition. No new optional-service stub was added; existing
stub counts remain one feature query, one empty environment and three
unavailable condition-variable exports.

## Identities and next work

- Main x86 SHA-256:
  `dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
- recomp-kit: `487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d`.
- Runtime C SHA-256:
  `514431378ec6c2302139a3d9f99b1c5766ae887dbdda3b8f556228d12e896621`.
- Compiled Android executable SHA-256:
  `4ec9a1ebe1907092709134774f245e396f870386b396ed3a2281561d9655c235`.
  Compilation completed; device selection failed afterwards, so this artifact
  has no completed physical startup run yet.
- Translation hash-map SHA-256 (UTF-8 JSON, sorted keys, compact separators):
  `b913e295b6da855515ab6282d7a927f3559818822d25a563eaaa790caeb3ff75`.

Private host report: `.work/startup-674-host-before-hardlink.json`. Original
bytes, generated translations and test artifacts remain under ignored `.work`.
The [translated D3D9 pixel shader proof](d3d9-shader-2026-11-01.md) separately
passes on the tablet with both drivers. Filesystem/native DLL behavior, the
remaining startup code, original graphics/audio, in-process amdaemon contracts,
touch integration and playable timing still need implementation and validation.
