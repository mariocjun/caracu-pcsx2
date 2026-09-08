# SPDX-FileCopyrightText: 2026 Caracu contributors
# SPDX-License-Identifier: GPL-3.0+
<#
.SYNOPSIS
Validate the pinned dependencies and build the Windows core without Qt.
.DESCRIPTION
Does not install software, run an emulator, or modify a PCSX2 profile.
The default target is the upstream GS dump runner, NOT the Caracu game server.
Use a new build directory when changing compiler language or toolchains.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DependencyBundle,
    [string]$BuildDirectory,
    [ValidateSet('caracu-ps2d', 'pcsx2-gsrunner', 'PCSX2', 'unittests')]
    [string]$Target = 'pcsx2-gsrunner',
    [string]$PackageDirectory,
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [switch]$CheckOnly,
    [switch]$Autoteste
)

$ErrorActionPreference = 'Stop'
if ($CheckOnly -and $Autoteste) { throw 'Choose CheckOnly or Autoteste, not both.' }
if ($PackageDirectory -and ($Target -ne 'caracu-ps2d' -or $CheckOnly -or $Autoteste)) {
    throw 'PackageDirectory requires a real caracu-ps2d build.'
}
$taskRepo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskLock = Get-Content (Join-Path $taskRepo 'caracu-windows-dependencies.lock.json') -Raw | ConvertFrom-Json
$taskBundle = (Resolve-Path -LiteralPath $DependencyBundle).Path
$taskArchive = Join-Path $taskBundle $taskLock.archive
if ((Get-Item -LiteralPath $taskArchive).Length -ne $taskLock.size_bytes -or
    (Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash -ne $taskLock.sha256) {
    throw 'Dependency archive differs from the lock. Do not silently update the lock.'
}
$taskDeps = Join-Path $taskBundle 'deps'
foreach ($taskRequired in @('include/zlib.h', 'include/libavutil/ffversion.h', 'lib/z.lib')) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskDeps $taskRequired) -PathType Leaf)) {
        throw "Extract the verified archive first: missing deps/$taskRequired"
    }
}
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $taskVswhere)) {
    throw 'Install Visual Studio Build Tools 2022 with the C++ workload and Windows SDK first.'
}
$taskVsPath = & $taskVswhere -latest -products '*' -version '[17,18)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $taskVsPath) {
    throw 'No complete VS2022 C++ installation was found. Check the installer/UAC, then retry.'
}
$taskVcvars = Join-Path $taskVsPath 'VC/Auxiliary/Build/vcvars64.bat'
$taskNinja = Join-Path $taskVsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$taskCmake = (Get-Command cmake.exe -ErrorAction Stop).Source
foreach ($taskExe in @($taskVcvars, $taskNinja)) {
    if (-not (Test-Path -LiteralPath $taskExe -PathType Leaf)) { throw "Missing build tool: $taskExe" }
}
# The sole cmd.exe input is a locally discovered batch path, not a project argument.
if ($taskVcvars -match '[%&^|<>"\r\n]') { throw 'Unsupported metacharacter in Visual Studio installation path.' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $taskRepo 'build-caracu-windows' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if ($BuildDirectory.TrimEnd('\', '/') -eq $taskRepo.TrimEnd('\', '/')) {
    throw 'An out-of-source build directory is required.'
}
Write-Output "Archive SHA-256 verified: $($taskLock.sha256)"
Write-Output "Visual Studio: $taskVsPath"
Write-Output "Ninja: $taskNinja"
Write-Output "Build: $BuildDirectory; target: $Target; Qt: OFF"
Write-Output 'Preflight does not certify a build, an unmodified extracted tree, or a game host.'
if ($CheckOnly) { return }

$taskSavedEnv = @{}
foreach ($taskName in @('PATH', 'INCLUDE', 'LIB', 'LIBPATH')) {
    $taskSavedEnv[$taskName] = [Environment]::GetEnvironmentVariable($taskName, 'Process')
}
$taskSavedEncoding = [Console]::OutputEncoding
try {
    # MSVC emitted UTF-8 while CMake decoded its /showIncludes probe as CP850.
    # Match the child console code page BEFORE the first compiler probe.
    # VSLANG=1033 alone does not help when only Portuguese resources are installed.
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $taskVcEnv = & $env:ComSpec /d /s /c "call `"$taskVcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'vcvars64 failed.' }
    foreach ($taskLine in $taskVcEnv) {
        if ($taskLine -match '^(PATH|INCLUDE|LIB|LIBPATH)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    $env:PATH = (Join-Path $taskDeps 'bin') + ';' + $env:PATH
    if ($Autoteste) {
        # Generated, original fixtures only. Never change a tracked source or a
        # user's existing build to test header invalidation.
        $taskProbe = Join-Path $BuildDirectory ('probe-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $taskProbe -Force | Out-Null
        foreach ($taskFixture in @('CMakeLists.txt', 'main.cpp')) {
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot "build-probe/$taskFixture") -Destination $taskProbe
        }
        $taskProbeBuild = Join-Path $taskProbe 'build'
        $taskHeader = Join-Path $taskProbe 'value.hpp'
        [IO.File]::WriteAllText($taskHeader, "#pragma once`nconstexpr int probe_value = 17;`n")
        & $taskCmake -S $taskProbe -B $taskProbeBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$taskNinja" -DCMAKE_CXX_COMPILER=cl
        if ($LASTEXITCODE -ne 0) { throw 'Probe configuration failed.' }
        foreach ($taskValue in @(17, 29, 41)) {
            if ($taskValue -ne 17) {
                [IO.File]::WriteAllText($taskHeader, "#pragma once`nconstexpr int probe_value = $taskValue;`n")
            }
            & $taskCmake --build $taskProbeBuild
            if ($LASTEXITCODE -ne 0) { throw 'Probe build failed.' }
            $taskObserved = & (Join-Path $taskProbeBuild 'probe.exe')
            if ($LASTEXITCODE -ne 0 -or $taskObserved -ne "$taskValue") {
                throw "Stale executable: expected $taskValue, observed $taskObserved"
            }
        }
        $taskDepsOutput = & $taskNinja -C $taskProbeBuild -t deps 'CMakeFiles/probe.dir/main.cpp.obj'
        if ($LASTEXITCODE -ne 0 -or -not ($taskDepsOutput -match 'value\.hpp')) {
            throw 'Ninja did not record the generated header dependency.'
        }
        Write-Output "PASS: original fixture output 17 -> 29 -> 41; header dependency recorded. Artifacts: $taskProbe"
        return
    }
    & $taskCmake -S $taskRepo -B $BuildDirectory -G Ninja "-DCMAKE_MAKE_PROGRAM=$taskNinja" `
        -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release `
        -DENABLE_QT_UI=OFF -DUSE_OPENGL=OFF -DUSE_VULKAN=OFF -DENABLE_TESTS=ON `
        "-DCMAKE_PREFIX_PATH=$taskDeps"
    if ($LASTEXITCODE -ne 0) { throw 'No-Qt CMake configuration failed.' }
    & $taskCmake --build $BuildDirectory --target $Target --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $Target" }
    if ($PackageDirectory) {
        $taskPackage = [IO.Path]::GetFullPath($PackageDirectory)
        if (Test-Path -LiteralPath $taskPackage) { throw 'Package directory must be new; nothing will be overwritten.' }
        & $taskCmake "-DEXECUTABLE=$BuildDirectory/caracu-ps2d/caracu-ps2d.exe" `
            "-DDEPENDENCIES=$taskDeps" "-DDESTINATION=$taskPackage" "-DSOURCE=$taskRepo" `
            -P (Join-Path $PSScriptRoot 'package-windows.cmake')
        if ($LASTEXITCODE -ne 0) { throw 'No-Qt package validation failed.' }
    }
} finally {
    [Console]::OutputEncoding = $taskSavedEncoding
    foreach ($taskName in $taskSavedEnv.Keys) {
        if ($null -eq $taskSavedEnv[$taskName]) {
            # PowerShell coerces $null to an empty string for the .NET setter.
            # Delete only this process environment entry to restore absence.
            Remove-Item -LiteralPath "Env:$taskName" -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable($taskName, $taskSavedEnv[$taskName], 'Process')
        }
    }
}
