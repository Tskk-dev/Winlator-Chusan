# First startup initializer: Windows and physical ARM64

The actual first entry-point call resolves through an ILT thunk to initializer
`0x00e3d42f..0x00e3d4ca`. This bounded 156-byte, 53-instruction routine now has
an original-x86 reference and a recomp-kit-generated translation.

## Identities

- Main executable SHA-256:
  `dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
- Initializer SHA-256:
  `50374ef59e830613476022f01720f3db27968652db8232247a32684a329a0233`.
- recomp-kit: `487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d`, eager flags enabled.
- Reference vector SHA-256:
  `b45e4223bcad0e731da43669833df1446373cbc3836be51b52daedcb8c4dcbf1`.
- ARM64 executable SHA-256:
  `faaaa947b51fc35c6785c06aabb9155af4086f317161437d081d20c85a08ae01`.

## Results

| Execution | Comparison | Result |
| --- | --- | --- |
| Original x86 bytes on Windows | Produce controlled reference records | 4,096 cases |
| Compiled kit translation, Windows x64 | Compare against original records | 4,096 cases, zero mismatches |
| Compiled kit translation, physical Android ARM64 | Compare against identical original records | 4,096 cases, zero mismatches |
| Windows x64 and physical ARM64 | Unknown call/jump/API/return, invalid read/write and invalid fixture output | All seven terminate with the expected diagnostic and nonzero exit |

Device: Xiaomi `23073RPBFG`, SM6225, Android 15 / API 35. ARM64 executes directly;
neither Wine nor an x86 interpreter is linked. Only the kit emitter and its
instruction header are consumed; its loader, import runtime, interpreter and
general dispatch tables are not linked.

## Exact comparison scope

The oracle maps the validated routine at its original address, plus its two
globals, four IAT slots, deterministic API trampolines and a fixed guest stack.
It does not run the original entry point or any DLL initializer. Both sides use
the same controlled GetSystemTimeAsFileTime, thread/process ID and performance
counter results. Fixture wrappers preserve registers and flags except their
specified EAX outputs and stdcall argument cleanup; they are not general Windows
API implementations.

Each record compares all eight general registers, final EIP/ESP, both globals,
ordered API trace including arguments/outputs/return PCs, and 64 stack bytes
from initial ESP minus 40 through plus 23. This covers the original routine's
frame/call writes and upper canaries. Lower fixture-adapter scratch is excluded.
Defined CF/PF/ZF/SF/DF/OF bits are compared; AF is excluded because logical
instructions leave it undefined. Inputs force initialized/default/zero-high-word,
generated collision and ordinary paths, then deterministic random cases.

Generated rd32/wr32 accesses are guarded. Only the four fixture APIs and the
top-level return sentinel are admitted. Decoder checks reject unadmitted
instructions and branches outside or into the middle of an instruction.

## Limits

This proves one bounded routine and fixture ABI. It does not qualify a full
x86 CPU/fault model, original CRT/TLS startup, callbacks, floating point,
amdaemon services, original graphics/audio, APK lifecycle or gameplay speed.
Generated code, original bytes, vectors, tool/source hashes and device reports
remain local under `.work/startup-probe/`.
