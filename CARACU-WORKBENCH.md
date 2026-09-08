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
Do not replace the missing `caracu-ps2d` with this GS dump runner.

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
