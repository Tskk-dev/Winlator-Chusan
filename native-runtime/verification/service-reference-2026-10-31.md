# amdaemon replacement reference assessment

The intended runtime replaces amdaemon.exe entirely with native in-process
services. It does not translate or launch the original x86-64 process.

## What the supplied archive contributes

The three-volume `SDEZ_1.65_with_iPad-master.7z` contains a Unity export with a
modified C# `AMWithoutDaemon.NET` implementation. It is substantial behavior
reference material, rather than just AMDaemon DLL wrappers. Static review found:

| Contract | Existing reference behavior | Android/CHUNITHM work |
| --- | --- | --- |
| Core lifecycle | Ordered input/card/save pump and hooks | Native ownership, threading, polling, shutdown and readiness |
| Credit | Coin conversion, per-player balance, payment and event acknowledgement | Confirm game-specific rules; durable accounting and overflow/error cases |
| Backup | Registered record buffers, restore and deferred save | Validate layouts/lengths; atomic saves; report failures truthfully |
| Aime | Scan, confirm, cancel and LED state; virtual/file/LAN input options | CHUNITHM request/response layout, stable profile identity and asynchronous ownership |
| IO | Current/previous values and switch/analog views | Ground/air input, cabinet buttons and LED output; cancellation semantics |
| Network | Interface/test status progression and auth/card checks | Actual connectivity, endpoint configuration, failure and timeout states |

The reference includes unconditional readiness/database availability, successful
no-ops, omitted bookkeeping, optimistic network statuses and incomplete fallback
input mappings. Some are suitable intentional stubs where the reached consumer
does not require the missing side effect; keep those explicit and counted.
Its backup path can accept short reads, has incomplete index validation, may miss buffer
synchronization, and can report success after a write error. These are reasons
to reproduce the useful contracts independently and test failure paths.

The AMDaemon subtree exposes managed C# objects to a Unity consumer. It does not
provide a native export surface or the CHUNITHM shared-memory/pipe protocol.
Its SDEZ/MAI2 identity, pathing and input topology are not CHUNITHM profiles.
Static review establishes source behavior only; no reference code was executed.
Extracted source/assets remain private in ignored `.work/`. No source license
was identified in the archive, so no extracted implementation is vendored.

## Observed CHUNITHM boundary

For main SHA-256
`dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`:

- There is no separately imported amdaemon client DLL in the main import table.
  Embedded library diagnostics identify `amdaemonLib Ver.91f83d14`, built
  20 May 2024. The main contains client-side manager/control/IPC names.
- Static imports include named mappings, map/unmap, mutexes, events and pipes.
  Executable-section byte scans locate candidate indirect IAT call sites:
  `MapViewOfFile` at `0x005ec857`, `OpenFileMappingW` at `0x005ec9e9` and
  `0x005ed120`, `CreateFileMappingW` at `0x005eca0d`, mutex operations near
  `0x005ed7bb..0x005ed7cd`, and `CreateNamedPipeW` at `0x005f47f1`.
  These are triage candidates, not recovered function boundaries or evidence
  that every call belongs to the amdaemon client.
- Embedded diagnostics describe missing/existing shared memory and mismatched
  mapping sizes/library versions. Their presence supports investigating a
  versioned IPC boundary; it does not establish its names, layout or lifecycle.

## Next concrete service gate

Recover the source-selected mapping creation/open path and manager initialization
for this hash. Record mapping names and sizes, headers/version checks, client
and server fields, synchronization, request IDs and response transitions. Then
choose either native client-method adapters or an in-process implementation of
the reached IPC contracts. Do not intercept every generic Win32 mapping call
as an amdaemon operation.

Implement and qualify core readiness, input/LED, credit, card and backup state
transitions against those observed layouts. Stub optional operations as needed
under the versioned policy; unhandled operations stop explicitly. A stub must
not turn a failed save or a discarded balance change into success. The supplied C#
code reduces behavior-design work, but does not yet satisfy this native gate.
