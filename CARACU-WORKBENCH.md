# Caracu PS2 Workbench backend

This fork preserves PCSX2 upstream history and licenses. Integration branch:
`codex/headless-host`, based on `98697735f1bb1a1452d975251269abd1019876d1`.

Status: bootstrap only. **There is no implemented or certified `caracu-ps2d`
target yet.** Do not treat the upstream Qt executable or GS dump runner as
that server. The parent toolkit implementation is tracked at
https://github.com/mariocjun/caracu/pull/4.

The selected contract is a Windows x64 MSVC host with `ENABLE_QT_UI=OFF`,
D3D11 surfaceless rendering, a CPU-thread command queue, and JSON-RPC on child
stdin/stdout. Logs use stderr; no network listener. Each child owns a private
datapath; never open the user's ordinary PCSX2 configuration or memory cards.

Before implementing the host, establish a supported compiler and dependency
build. Local preflight on 2026-09-08 found MinGW GCC 15.2.0 but no usable MSVC
or clang-cl installation. The parent task requested approval to install C++
Build Tools. This is not a claim that PCSX2 itself cannot build on Windows.

Acceptance requires actual boot, no Qt DLLs/windows, correct 128-bit register
values, real instruction execution (not PC increments), separately measured
frame stepping, identity invalidation on reset, CPU breakpoints/watchpoints,
and tests for timeouts, disconnects and two isolated sessions. CPU watchpoints
must not be advertised as coverage of every DMA write.

Do not commit BIOS, ROMs, savestates, dumps or reconstructed commercial game
source. Preserve SPDX headers and notices when adapting existing host code.
No upstream source in this fork is relicensed to the toolkit's MIT license.
