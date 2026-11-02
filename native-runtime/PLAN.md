# Native Android implementation plan

Prepared with GPT-6 Astra on 31 October 2026. Target: Snapdragon 680 / Adreno
610, Android ARM64, local touch overlay. Winlator integration is deferred.

## Evidence and rules

The pinned main executable is x86 SHA-256
`dcd883ddd41d6e0a26a8902d8c94dd123c5e8dd5990dd6a428e6b421195d38ca`.
The supplied iPad plan describes another input and supplies no runtime source.
Its offsets and reported checks are not evidence for this build.

Keep game bytes, generated translations, reference vectors and private service
configuration in ignored local storage. Keep guest pointers 32-bit. Every
unhandled instruction, target, import or service operation must stop with an
actionable diagnostic. Explicit stubs are allowed under the policy below.
Qualification runs must not silently use an interpreter or undisclosed
success-returning placeholders.

## Stub policy

Build only the behavior actually reached by startup and a playable chart.
Optional cabinet hardware, unused sensors, reporting and optional services can
use simple deterministic stubs. Prefer a stable default or unavailable/unsupported
result where that matches the consumer's contract; a successful no-op is fine
for an operation whose effect is unnecessary and whose callers tolerate it.

For each admitted stub, record its exact operation/address or import, validated
arguments, output/return values and known limitation. Count hits and report the
first use plus a run summary, rather than logging every hot-path call. Keep the
stub policy versioned with the game profile. Promote a stub to real behavior
only when a reached caller needs its state or side effects.

Credit/payment balances, active card identity, save contents, input transitions
and the playback clock need coherent state. Their implementations can remain
small; they must not acknowledge a change they discarded. Optional network
features can remain unavailable for an offline test profile. A successful stub
does not count as proof of original rendering, audio, persistence or networking.
Unlisted behavior still stops explicitly, so a typo or new request cannot
silently inherit a blanket success response.

## Ordered gates

1. **First startup initializer — bounded gate complete.** Qualify recomp-kit's pinned emitter on the
   actual entry-point initializer at `0x00e3d42f..0x00e3d4ca`. Compare original
   x86 execution with compiled translations on Windows and physical Android.
   Control all four API inputs; compare registers, defined flags, globals,
   caller stack and ordered API calls. Inject unknown call/jump/import/return
   and invalid-memory failures. This is a bounded fixture, not game startup.
   Result: 4,096 original-x86 comparisons each on Windows x64 and physical
   Android ARM64, zero mismatches; all seven terminal failures verified.
   [Evidence](verification/startup-initializer-2026-10-31.md).
2. **Instruction and ABI coverage.** Add source-selected routines exercising
   loops, indirect callbacks, SSE conversions and reached x87 operations.
   Record numerical limits: recomp-kit's x87 state uses double, not 80-bit.
3. **Honest startup.** Start at `0x00e3c07c`, admit its ILT jump and CRT/TLS
   closure incrementally, and record the first unsupported address/import.
   Main-image TLS data exists; the inspected callback array starts with null.
   Do not invent callbacks. Qualify auxiliary DLL TLS and relocation support.
   Active: the new bounded block runtime executes actual entry/CRT code on
   Windows and ARM64 and iteratively records the first missing operation.
   All four C and 959 C++ callbacks complete on both platforms, with matching
   block/API counts. CRT launch and the application's mutex also pass on both.
   Host follow-up now passes the first configuration file check and module
   enumeration, stopping at `WSAStartup`. Fixed C locales, primary-thread
   library locks and a caller-scoped optional page-scan stub advance startup;
   later stream changes still need a physical tablet run.
   [Latest evidence](verification/application-startup-2026-11-02.md).
   Four inspected C initializer table members are admitted as roots. Native
   callback iteration, cdecl stack handling and selected CRT adapters are
   implemented; full startup remains unproved.
   Earlier shared host/ARM64 stop: `CreateEventW` after 271 executed blocks.
   [Checkpoint](verification/startup-bringup-2026-10-31.md).
4. **Early graphics decision.** Qualify the kit's actual D3D9 and shader code
   on Android before large-scale startup work. First prove shader draw,
   pixel readback and presentation, then a captured original draw. If that
   backend fails, evaluate a pinned DXVK Native build and guest32 COM bridge.
   Stock and app-local Turnip both create a device; neither currently reports
   maintenance5/6. This does not establish current DXVK compatibility.
   The smallest existing proof is the kit's generic offscreen compositor
   draw/readback contract test. Android currently excludes that test target.
   Connect the tested driver's GIPA to the renderer/Volk custom loader; do not
   let a custom-driver test fall back to system Vulkan. D3D9 GLSL/glslang
   conversion and a populated HostD9Draw packet are a separate follow-up gate.
   Generic offscreen draw/readback now passes with stock and app-private Turnip
   on the target tablet. [Evidence](verification/vulkan-draw-2026-11-01.md).
   A kit-translated D3D9 pixel shader now compiles through glslang and passes
   16/16 pixel readback on both drivers. Guest COM, game draw packets and
   presentation remain pending.
   [Shader evidence](verification/d3d9-shader-2026-11-01.md).
5. **DLL/CRT and content.** Close the actual MSVC/UCRT/SEH, Cg, E-mote and CRI
   dependencies. Prove an original content frame and decoded audio with a
   measured playback clock. A clear or silent output is a separate milestone.
6. **Replace amdaemon entirely.** Implement the game-facing contracts in a
   native in-process service layer; do not translate or launch amdaemon.exe.
   Reuse verified behavior from the supplied SDEZ/iPad reference where the
   interfaces agree. Map CHUNITHM-specific ABI, shared-memory/IPC structures,
   lifecycle, status transitions, inputs/LEDs, credits, card identity,
   persistence and network configuration before claiming compatibility.
   Reference source is evidence to inspect, not instructions to execute.
   [Reference assessment and current IPC triage](verification/service-reference-2026-10-31.md).
7. **Playable integration.** Wire the calibrated touch overlay to native
   service input, complete a real chart, verify results/save/relaunch and
   interruptions. Keep card presentation separate from credit insertion.
8. **Sustained device measurements.** Measure frame pacing, audio underruns,
   input latency and thermal behavior. AOT and extra Vulkan extensions do not
   imply 60 FPS or accurate rhythm timing.

Service reference inspection can proceed alongside gate 1. Each reached service
contract needs either a tested state implementation or an explicit tested stub;
the full AMDaemon API does not need to exist before the first boot attempt.

## Reuse decisions

- Qualify recomp-kit commit `487b94ac17bc10c4796dc4bce8d16ddbdeb7ec1d`.
  Its current code includes D3D9 and shader support despite an older design
  document's exclusions. Its complete runtime is not adopted: unknown calls
  can interpret, unresolved imports can return zero, and unknown returns can
  continue. The first fixture uses only its emitter and instruction header,
  eager flags, and a strict consumer-owned dispatch/memory boundary.
- Preserve the tested app-local libadrenotools path. Driver changes require a
  fresh process and actual capability checks.
- Treat the SDEZ archive as a private reference. Do not publish extracted game
  code/assets or claim that a Unity AMDaemon wrapper is already a native
  CHUNITHM service implementation.

## Completion evidence

Each gate records source and tool identities, exact comparison scope, test
counts, first unsupported behavior, and device evidence separately from host
checks. Full boot, original graphics/audio and playable speed remain unproved.
