# Application startup beyond the CRT

All four C and 959 C++ callbacks pass on Windows x64 and the physical Snapdragon
680 tablet. Host follow-up now passes the first configuration file check and
module enumeration, reaching `WSAStartup`. The newer stream and module behavior
still needs physical ARM64 execution. The game has not booted or produced an
original content frame.

## Shared host/tablet CRT launch checkpoint

| Measurement | Each platform |
| --- | --- |
| Prepared blocks / instructions | 72,000 / 342,968 |
| Executed block visits | 65,886 |
| Imported API dispatches | 3,157 |
| C / C++ callback returns | 4 / 959 |
| Stop | `missing_import`, `MSVCP140.dll` basic_ios<char> constructor |
| IAT / caller | `0x0221a514` / `0x0055a3e2` |
| ESP | `0x0effeac0` |

The Windows and ARM64 stop records match, including counters, stack position
and existing stub counts. This is compiled-path agreement, not an independent
original-x86 comparison of the whole application startup path.

## Implemented behavior

- `GetStartupInfoW` writes the 68-byte x86 structure for the current GUI launch:
  no console inheritance or window overrides. It checks the full destination
  before writing and leaves adjacent storage and guest last-error state intact.
  The CRT then selects its default show mode. Four fixtures check all fields,
  surrounding sentinels, null, read-only and page-crossing destinations.
  [API contract](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getstartupinfow),
  [structure](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfow).
- `_get_narrow_winmain_command_line` returns a stable guest-owned empty argument
  buffer. The inspected original launch scripts pass no game arguments; the
  WinMain string excludes the executable name. A fixture checks readable empty
  contents and repeated pointer identity. Configurable arguments remain pending.
  [WinMain contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-winmain).
- Events and mutexes now use typed, generation-checked kernel handles with a
  shared name table. Mutexes track primary-thread ownership and recursive waits;
  release by a non-owner fails. ANSI and wide creation share names; reopening
  returns error 183 and ignores the new initial-owner request. Event/mutex name
  collisions and wrong-kind operations return error 6. Closing the last handle
  destroys the object; stale handles remain invalid. Four mutex fixtures cover
  these transitions alongside the existing event tests.
  [Creation](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createmutexa),
  [release](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-releasemutex),
  [waits](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject).
- `GetErrorMode` and `SetErrorMode` preserve process state, return the previous
  value on replacement and preserve guest last-error state. The game's request
  adds `0x8002`; the admitted mask is `0x8003`. Alignment-control and unknown
  flags stop explicitly. Three fixtures check replacement, reset and unsupported
  requests. The host process's error settings are not changed.
  [Get](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-geterrormode),
  [set](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-seterrormode).

Mutex qualification covers the primary guest thread and in-process named objects.
Guest scheduling, abandoned mutexes, security attributes, non-ASCII ANSI names
and Windows session/private namespace behavior remain unqualified. Existing
blocking event waits and unsupported mutex waits still stop; no wake-up is
manufactured. No new optional-success stub was added.

All 70 compiled regression cases passed on both Windows x64 and physical ARM64,
including the five new launch cases, four mutex cases, four event regressions,
and three error-mode cases.

## Shared checkpoint identities

- Main x86 SHA-256:
  `dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
- recomp-kit: `487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d`.
- Runtime C SHA-256:
  `a8b31fce53f4466bdf9ab017375e87eeb62b7458b54b3f6ff52326f928b45758`.
- Android executable SHA-256:
  `106aa3371cf92f0eff6a5f40493750a65b4e2e46503ed12e36bf711970fa2a18`.

Private report: `.work/startup-winmain-stream-stop.json`. Device hashes were
checked and temporary files removed.

## Host follow-up: fixed C streams and optional scan

The host opens the actual `project.conf` from an explicit read-only content root.
The inspected immediate caller checks stream failure and continues into process
setup; it does not read the configuration contents on this path. `data.conf`
exists under `option/A001` in this installation, not beside the executable.
Opening the file is not evidence of configuration parsing or asset loading.

| Measurement | Windows x64 follow-up |
| --- | --- |
| Prepared blocks / instructions | 72,000 / 342,968 |
| Executed block visits / API dispatches | 89,460 / 11,015 |
| C / C++ callback returns | 4 / 959 |
| Next stop | `missing_import`, `WS2_32.dll!WSAStartup` |
| IAT / caller / ESP | `0x0221ac10` / `0x0055bcac` / `0x0effeaec` |
| Runtime C SHA-256 | `531c0f5b57662a30d6268dd7b7ae37d3ba724c9a6aaa79209e5e177393694f3a` |
| Built ARM64 executable SHA-256 | `f1a60f4ad7b15ad432722bff301d31d602a9c839b22a70e4594a7c89bb94f8b4` |

The ARM64 executable builds successfully. This build identity is not a newer
physical tablet execution result.

The small MSVCP adapter establishes the reached `basic_ios`, `basic_istream`
and `basic_streambuf` layouts, including the stream buffer's pointer aliases.
`getloc` copies a guest-owned locale reference, and reference-count virtual calls
balance temporary copies. The required codecvt data import points to readable
guest data. Its fixed ID and facet array let the original game helper perform
its own lookup; no game function or executable instruction was replaced.
`fail()` reads the actual stream state.

The new admitted defaults/stubs are:

| Operation | Behavior and limit |
| --- | --- |
| Stream locales and narrow codecvt | Fixed classic C locale, space fill and identity character conversion. Only the canonical codecvt ID/facet is admitted. Custom locales and conversion methods remain unsupported. Two locale nodes and two codecvt queries occur in this run. |
| MSVCP `_Lockit` | Successful single-thread lock scopes; two calls. Guest thread creation/scheduling is still unsupported, so no other guest thread can contend. |
| `GetSystemInfo` | One virtual x86 processor, 4 KiB pages, 64 KiB allocation granularity and the bounded 256 MiB guest arena. These are compatibility-machine values, not reported Android hardware. One call. |
| Optional startup page scan | `VirtualQuery` returns zero/error 50 only at pinned caller `0x007dedd4`, for a 28-byte request inside the main image. The inspected loop skips failures and ignores its final count. All 7,838 queries are counted; other callers stop explicitly. |

Each stub logs its first use and reports a counter. Unimplemented C++ virtual
methods still stop explicitly. The constructors do not imply complete stream
destruction, exception handling or standard-library support.

`_Fiopen` and `fclose` use actual read-only host files behind opaque guest
handles. Narrow relative paths are resolved under the explicit root; parent
traversal and write modes are rejected. `_get_stream_buffer_pointers` returns
addresses of guest-owned pointer/count cells, preserving inline stream access
without exposing a host `FILE*`. This matches the address indirection used by
[Microsoft's filebuf implementation](https://github.com/microsoft/STL/blob/main/stl/inc/__msvc_filebuf.hpp).
File reads, seeks, wide paths and writable save storage remain pending.

The process pseudo-handle, main-module information and enumeration describe
the currently loaded guest PE. Enumeration reports that single module; DLL
loading remains pending. The virtual machine's system information and optional
page scan do not change host process memory or CPU affinity.

Twenty new stream/locale/file fixtures passed on Windows, including a native
original-x86 MSVCP constructor comparison. Two further fixtures check module
metadata/system fields/enumeration and rejection of an unqualified memory-query
caller. The tooling suite passes 43 tests. These are focused runs across this
development sequence; the earlier full 70-case host/tablet run remains the
shared baseline. The six basic-ios constructor fixtures also passed on physical
ARM64 before the later stream additions. Wireless ADB subsequently advertised
an endpoint that refused connections; no newer physical result is claimed.

Private host evidence: `.work/startup-fixed-c-streams-host.json`,
`.work/stream-19-host-results.json`, `.work/ios-fail-state-results.json` and
`.work/module-scan-host-results.json`. Next close Winsock startup, then follow
the reached content/service path. Original graphics/audio, native amdaemon
contracts, touch controls and playable timing remain incomplete.
