# Constructor runtime checkpoint

Windows x64 and the physical Snapdragon 680 tablet both complete four C and
425 C++ initializer callbacks, then stop at the untranslated constructor
`0x00a9a500`, called from `0x00e3c30e`. This extends the earlier
[239-callback checkpoint](startup-bringup-2026-11-01.md). Full game boot remains
unverified.

| Measurement | Each platform |
| --- | --- |
| Prepared blocks / instructions | 44,000 / 197,095 |
| Translation units | 167 |
| Executed block visits, including repeats | 27,089 |
| Imported API dispatches | 1,679 |
| C / C++ callback returns | 4 / 425 |
| Stop | `missing_code`, `0x00a9a500` |
| ESP | `0x0effef68` |

The table has 959 C++ callbacks. Callback progress is not a percentage of the
complete port. Both platforms execute compiled translations; this is not a new
original-x86 differential test of the full startup path.

## Native behavior added

- `_mktime64` marshals the guest's nine 32-bit `tm` fields, normalizes through
  the platform calendar implementation, writes successful normalization back,
  and returns the 64-bit result through EDX:EAX. Invalid UTC dates outside
  1970–3000 return -1 with EINVAL. Four UTC fixtures cover 2015, a nonzero upper
  result word in 2200, normalization and invalid range boundaries. The actual
  startup input is 2015-01-01 04:00 in the devices' current UTC+8 zones. These
  checks do not qualify differing historical DST rules between platforms.
  [Microsoft calendar contract](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mktime-mktime32-mktime64).
- `wcsncpy_s` copies guest UTF-16 units without depending on Android's
  `wchar_t` size. Six fixtures cover non-ASCII text, explicit count, short
  sources, exact fit, `_TRUNCATE`, zero count, preserved trailing storage and
  the explicit invalid-parameter stop on insufficient capacity. Registered
  invalid-parameter handler delivery remains unsupported.
  [Microsoft string contract](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/strncpy-s-strncpy-s-l-wcsncpy-s-wcsncpy-s-l-mbsncpy-s-mbsncpy-s-l).
- `__std_type_info_compare` compares the x86 decorated-name data, skips its
  cached pointer and first prefix byte, and returns -1/0/1 using unsigned byte
  ordering. The fixture covers equal descriptors, equal names at different
  addresses, both ordering directions and high bytes. The ABI was inspected
  in Microsoft x86 vcruntime140.dll 14.51.36247.0, SHA-256
  `2fa6efc053203460a23d3a25158f227d895d2dadc63acc1a372da97c3a4281c3`.

All 40 compiled synthetic scenarios pass on Windows and physical ARM64 using
the same runtime source. All 35 tooling tests pass. No optional-service stubs
were added; the earlier stub counts remain unchanged.

## Retained translation batches

An unrestricted expansion of read-only callback tables displaced earlier CRT
code at the bounded graph cutoff. Preparation now starts with a baseline and
retains its block entries before admitting additional read-only table targets.
It validates source/translator identities, executable addresses and inclusion
of every retained block. Only consecutive pointers into the verified ILT are
followed from read-only tables referenced by admitted MOV/PUSH instructions.

The fresh 28,000-block baseline reproduces the earlier translation-unit hashes
exactly. The expanded batch retains all those entries and discovers the
previously missing virtual method and its body. It still records 2,519 pending
direct targets and 76 guarded translation errors. No instruction guard fired
in this execution. Future batches can retain `preparation.json` directly.

## Artifact identities and next work

Main x86 SHA-256:
`dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
recomp-kit: `487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d`.
Runtime C SHA-256:
`ba714864129c3cbca48d5583604a12cab0fb97ccf20cb8aacdee271d3746771e`.
Canonical translation-unit hash-map SHA-256:
`14f77b7cb36f443b21e26f3b627dc1775502dba17e07555d6fff68b627a03283`.
Android executable SHA-256:
`229c4163f8c1e7e1a6c6772beb501d4807c974f52f5e7429032037a13ed7f501`.

Device: Xiaomi 23073RPBFG / SM6225, Android 15 / API 35. Private reports are
`.work/startup-425-constructors.json` and `.work/runtime-tests/`. Game bytes and
generated translations remain private. Device temporary files were removed.

The next observed constructor target must be admitted before continuing
startup. The remaining constructors, D3D9/content/audio integration, complete
in-process service replacement, touch overlay and playable timing remain
required. The separate generic Vulkan draw proof is unchanged.
