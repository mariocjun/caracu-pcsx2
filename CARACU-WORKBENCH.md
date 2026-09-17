# Caracu PS2 Workbench backend

This fork preserves PCSX2 upstream history and licenses. Integration branch:
`codex/headless-host`, based on `98697735f1bb1a1452d975251269abd1019876d1`.

Status: **`caracu-ps2d` is implemented with bounded live tests; the complete
Workbench/native steering proof is not finished.** Do not substitute the
upstream Qt executable or GS dump runner for this server. Integration is tracked at
https://github.com/mariocjun/caracu/pull/4.

The selected contract is a Windows x64 MSVC host with `ENABLE_QT_UI=OFF`,
D3D11 surfaceless rendering, a CPU-thread command queue, and JSON-RPC on child
stdin/stdout. Logs use stderr; no network listener. Each child owns a private
datapath; never open the user's ordinary PCSX2 configuration or memory cards.

The user approved package installation and completed Windows UAC on 2026-09-08.
Build Tools 2022 **17.14.39** installed successfully, with **MSVC 19.44.35228.0**
(toolset directory 14.44.35207) and **Windows SDK 10.0.26100.0**. The installer
reports complete/launchable and no reboot required. Existing emulators and
profiles were not changed.

The official Windows dependency bundle has been downloaded and extracted into
an isolated `PS2Dev/pcsx2-deps-20260903-007aac34` directory outside both repos.
Its SHA-256 matches `caracu-windows-dependencies.lock.json`. The release tag
can change: use the hash/asset ID, not the word "latest", as the identity.
The pinned upstream dependency script prefers VS2022 to VS2026. The bundle
contains Qt as well as core libraries; its presence on disk does not mean the
future executable links Qt. That needs a separate dependency/runtime check.

## Reproducible build preflight

From PowerShell, pass the bundle directory containing the archive and `deps`:

```powershell
./tools/caracu/build-windows.ps1 -DependencyBundle C:/path/to/verified-bundle -Autoteste
./tools/caracu/build-windows.ps1 -DependencyBundle C:/path/to/verified-bundle
./tools/caracu/build-windows.ps1 -DependencyBundle C:/path/to/verified-bundle -Target unittests
```

`-CheckOnly` checks prerequisites without configuring or building. Wrong archive
identity is rejected before configuration; software installation is separate.
The default output is `build-caracu-windows`. The script preserves the caller's
PATH/INCLUDE/LIB/LIBPATH and console encoding, and does not run an emulator.
It configures `ENABLE_QT_UI=OFF`, `USE_OPENGL=OFF`, `USE_VULKAN=OFF` and uses
the native Ninja bundled with VS. Existing build trees are never cleaned.

MSVC's Portuguese `/showIncludes` output exposed a CMake CP850/UTF-8 mismatch:
the first builds recorded zero header dependencies. Merely setting VSLANG=1033
did not fix this installation, which only has Portuguese language resources.
The script aligns the console encoding before the initial CMake compiler probe.
Use a **new build directory** when migrating from a corrupted compiler cache.
`-Autoteste` generates an original isolated fixture: its output must change
17 → 29 → 41 after two header-only edits, and Ninja must record `value.hpp`.
Its generated files are retained for inspection, not committed.

The initial core build also found D3D.cpp including Vulkan headers while Vulkan
was disabled. Its include guard now follows ENABLE_VULKAN, matching its usage.
After that correction, all 692 steps completed, linking `pcsx2-gsrunner.exe`.
The executable's direct PE imports contain no Qt DLLs. It was **not launched**:
the upstream runner initializes configuration even before handling help/version
arguments. No game boot, rendering, process-window or runtime-DLL gate is implied.
The separate host implementation and runtime evidence follow below.

`-Target unittests` also completed: **2/2 CTest executables passed**
(`common_test`, `core_test`), without BIOS or media. This validates their
selected unit cases, not instruction stepping, synchronization or gameplay.

Acceptance requires actual boot, no Qt DLLs/windows, correct 128-bit register
values, real instruction execution (not PC increments), separately measured
frame stepping, identity invalidation on reset, CPU breakpoints/watchpoints,
and tests for timeouts, disconnects and two isolated sessions. CPU watchpoints
must not be advertised as coverage of every DMA write.

Do not commit BIOS, ROMs, savestates, dumps or reconstructed commercial game
source. Preserve SPDX headers and notices when adapting existing host code.
No upstream source in this fork is relicensed to the toolkit's MIT license.

## Implemented host (AI-assisted)

`caracu-ps2d/Main.cpp` owns the CPU loop and a bounded JSON-RPC stdin queue;
stdout is reserved for protocol replies, including when core code prints.
The input pipe is polled so EOF/shutdown can join the reader without waiting
forever on a console read. There is no TCP listener or Qt dependency.

Each launch requires an absolute, previously nonexistent `--session-dir`.
The host never calls the personal/portable profile fallback. `--bios` is copied
privately, including the location where fresh NVM/MEC may be generated. Settings
stay in memory, physical input sources are inert, memory-card slots are disabled,
audio uses an emulated SPU2 with null output, and D3D11 has a surfaceless target.
`--help` and `--version` are processed before all VM/config initialization.

Protocol v1 is newline-delimited JSON-RPC requests (not batch/notifications),
1 MiB maximum line and 64 queued callbacks. `hello` lists implemented methods.
All other calls need `params.session` and `params.generation`. Reset, stop and
state load invalidate generations. RAM access requires explicit pause, supports
bounded physical/KSEG aliases, and rejects MMIO. EE/IOP registers use hex with
128-bit storage; the category also declares its real width.

Methods cover boot/pause/resume/reset/stop/shutdown, RAM read/write, registers,
CPU execution/read/write breakpoints, frame advance, controller bindings/input,
private savestate tokens and PNG capture. `ee.step` calls the real interpreter
with a valid jump context and reports dispatched instructions, cancellation and
event exit. A branch can include its delay slot. This does not implement IOP
step or certify all exception/COP/VU paths. CPU watchpoints do not cover DMA.
Frame advance replies only after returning paused at the requested VSync count.

Use the MIT Python process adapter in the separate Caracu repo, not copied core
code. It journals commands, hashes the executable/media/BIOS, and disallows
further commands after transport timeout because their outcome is uncertain.
The exact executable SHA identifies dirty builds; `hello` also reports the base
commit and whether the source was dirty at configure time.

### Optional EE call-graph recording (branch `codex/decomp-calltrace`)

Absent by default. `--calltrace methods` (or `CARACU_CALLTRACE=methods` in the
child environment; the argument wins) adds four methods to `hello.methods`;
`record` also starts recording at host start and writes one final trace on
`shutdown`/exit. Without the option the method list, replies and generated EE
code are unchanged, and `calltrace.*` return -32601 like any unknown method.

- `calltrace.start {clear?=true}` / `calltrace.stop`: VM paused. Both clear the
  CPU execution caches, because the recompiler emits the record call only in
  blocks compiled while recording is on.
- `calltrace.dump`: writes a new `<session-dir>/calltrace/calltrace-N.json`
  (`format caracu-calltrace-1`) with the host identity (ELF, `pcsx2_elf_id`,
  media, build, generation) and aggregated edges
  `{from, to, kind, word, count}`; never overwrites.
- `calltrace.configure {ee_core?: interpreter|recompiler, limiter?: nominal|unlimited}`:
  switches the EE core through the settings layer (effective at the next
  `Execute`) and the speed limiter, for measurements; replies with `calltrace`
  status including `ee_cpu_last_used`.

Kinds: `jal`, `jalr`, `j`, and `jr` with `rs != ra` (returns are not recorded).
`from` is the PC of the jump, `word` the executed instruction. The interpreter
records before the delay slot (its event test may leave through fastjmp); the
recompiler records after the delay slot, immediately before the block exit.
Recording runs on the EE thread into an open-addressing table; no locking.
Branch-and-link `bgezal/bltzal` are not recorded. IOP is not instrumented.

### RAM hash for differential tests (branch `codex/rota-c-hash-ram`)

`memory.hash {cpu: ee|iop, address, length, algorithm?: xxh3_128|md5}` hashes a
range of main RAM (or its KSEG0/KSEG1 alias) inside the host and replies with
`{algorithm, hash, address, length}`. Unlike `memory.read` it accepts the whole
RAM (32 MiB for the EE, 2 MiB for the IOP) in one call, because the digest is
computed on the CPU thread at the dispatch barrier, directly over `eeMem->Main`
/`iopMem->Main`. `xxh3_128` (default) is the canonical big-endian hex of
XXH3-128; `md5` exists so a client can check the range against its own reading.
It reads nothing outside main RAM: no scratchpad, VU, GS or hardware registers.

### Bounded evidence and remaining limits

Caracu's `mcp/tests/test_sessions_live.py` and `test_sessions_mcp.py` passed eight
integration tests against the locally packaged host without a dependency PATH
override. Original MIPS code increments a counter and loads both nonzero halves
of a 128-bit GPR. Tests cover separate sessions, stale generations, invalid JSON,
oversized requests, profile reuse, pause/resume, writes/readback, reset, executable
breakpoints, a real arithmetic/store/branch+nonempty-delay-slot sequence, a CPU
write watchpoint, VSync advance, private save/load, input binding validation,
transport timeout policy and actual MCP requests.

Windows process inspection at the synthetic guest's paused state found no
top-level windows or Qt modules; D3D11 was loaded. This is a sampled runtime
check, not proof about every possible driver/platform. Original Armageddon and
GoW II ISOs each ran 600 VSyncs, returned the expected ELF identifiers and
produced nonuniform 640x480 offscreen PNGs. GoW II reached its menu; Armageddon
was at its opening legal screen. Repeated paused captures matched. Input hashes
and metadata of 1,387 personal-profile files were unchanged. This is not an
entire-game, gameplay determinism, or steering equivalence certification.

The test that armed an EE breakpoint before BIOS/ELF startup exceeded its
25-second boot observation window. The retained test now establishes an
executing ELF before arming the breakpoint. Pre-ELF debugger latency remains
unqualified; do not promise a universal boot deadline.

### Local packaging

```powershell
./tools/caracu/build-windows.ps1 -DependencyBundle C:/path/to/verified-bundle -Target caracu-ps2d -PackageDirectory C:/new/package-directory
```

Packaging resolves the PE dependency closure, rejects Qt, copies non-system
DLLs/resources/notices and refuses an existing destination. Windows and the
MSVC runtime remain prerequisites; DLLs loaded on demand need separate tests.
No BIOS, game image, state or game-derived code belongs in this package. Keep
the corresponding source and notices with distributions; the package is a local
development artifact, not a signed or universally compatible release.
