<#
.SYNOPSIS
    Configure and build Nintendall with the local toolchain.

.DESCRIPTION
    Thin wrapper around the CMake presets in CMakePresets.json. It makes sure a
    MinGW-w64 toolchain is on PATH (searching the usual install locations,
    including the winget WinLibs package), then runs

        cmake --preset <preset>
        cmake --build --preset <preset>

.PARAMETER Preset
    Which preset to build. One of: release, debug, tests, all-cores.
    "release" is the default; "tests" also builds nintendall_test.exe.

.PARAMETER Fresh
    Delete the build directory first for a clean configure.

.EXAMPLE
    .\tools\build.ps1
    Release build of the default NES + SNES cores.

.EXAMPLE
    .\tools\build.ps1 -Preset tests
    Same, plus the headless smoke-test binary.
#>
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug', 'tests', 'all-cores')]
    [string] $Preset = 'release',
    [switch] $Fresh
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# --- make sure a C/C++ toolchain (gcc/g++) is reachable ----------------------
if (-not (Get-Command gcc.exe -ErrorAction SilentlyContinue)) {
    $candidates = @()

    # winget WinLibs / other MinGW packages
    $winget = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path $winget) {
        $candidates += Get-ChildItem $winget -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'WinLibs|mingw|LLVM' } |
            ForEach-Object { Join-Path $_.FullName 'mingw64\bin' }
    }

    # common manual installs
    $candidates += @(
        'C:\mingw64\bin'
        'C:\msys64\mingw64\bin'
        'C:\ProgramData\mingw64\mingw64\bin'
        (Join-Path $env:ProgramFiles 'mingw64\bin')
    )

    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'gcc.exe'))) {
            $env:PATH = "$c;$env:PATH"
            Write-Host "Using toolchain: $c"
            break
        }
    }

    if (-not (Get-Command gcc.exe -ErrorAction SilentlyContinue)) {
        throw "No gcc.exe found. Install MinGW-w64 (e.g. winget install BrechtSanders.WinLibs.POSIX.UCRT) or add it to PATH."
    }
}

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw "cmake.exe not found on PATH. Install CMake 3.21+ or add it to PATH."
}

Push-Location $ProjectRoot
try {
    if ($Fresh) {
        Write-Host "Removing build directory..."
        Remove-Item (Join-Path $ProjectRoot 'build') -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host "Configuring (preset: $Preset)..."
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }

    Write-Host "Building..."
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "build failed" }

    Write-Host ""
    Write-Host "Done. Executables are in build\bin\"
} finally {
    Pop-Location
}
