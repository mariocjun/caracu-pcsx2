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
or clang-cl installation. The user subsequently approved package installation.
Build Tools 2022 17.14.39 was launched with the VCTools workload, recommended
components, `--quiet --wait --norestart`; at this checkpoint, Windows UAC still
requires the user's confirmation. No successful MSVC build is claimed yet.
This is not a claim that PCSX2 itself cannot build on Windows.

The official Windows dependency bundle has been downloaded and extracted into
an isolated `PS2Dev/pcsx2-deps-20260903-007aac34` directory outside both repos.
Its SHA-256 matches `caracu-windows-dependencies.lock.json`. The release tag
can change: use the hash/asset ID, not the word "latest", as the identity.
The pinned upstream dependency script prefers VS2022 to VS2026. The bundle
contains Qt as well as core libraries; its presence on disk does not mean the
future executable links Qt. That needs a separate dependency/runtime check.

After UAC/install completion, verify the actual MSVC toolset and Windows SDK,
then configure an isolated build with `ENABLE_QT_UI=OFF`, `USE_OPENGL=OFF`,
`USE_VULKAN=OFF`, and `CMAKE_PREFIX_PATH` pointing to the extracted `deps`.
Linking the upstream `pcsx2-gsrunner` can check the dependency closure, but
cannot satisfy the game-host gate. Do not replace the missing server with it.

Acceptance requires actual boot, no Qt DLLs/windows, correct 128-bit register
values, real instruction execution (not PC increments), separately measured
frame stepping, identity invalidation on reset, CPU breakpoints/watchpoints,
and tests for timeouts, disconnects and two isolated sessions. CPU watchpoints
must not be advertised as coverage of every DMA write.

Do not commit BIOS, ROMs, savestates, dumps or reconstructed commercial game
source. Preserve SPDX headers and notices when adapting existing host code.
No upstream source in this fork is relicensed to the toolkit's MIT license.
