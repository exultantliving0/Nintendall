<#
.SYNOPSIS
    Vendor the upstream emulator source trees into this project.

.DESCRIPTION
    The Nintendall emulator links each console's emulator as a static library.
    This script copies the upstream sources it needs into

        cores/<core>/upstream/

    which is the location the per-core CMakeLists.txt looks in first. Running
    it makes the project self-contained: after vendoring you can build with no
    external checkout at all.

    If you already have the sources next to this project (the default), the
    build also works without vendoring: each core falls back to
    <repo>/consoles/<system>/<emulator>. Vendoring is what you do when you want
    to copy the tree somewhere else (for example, to publish a standalone
    source archive).

.PARAMETER SourceRoot
    Folder that contains the per-system emulator checkouts. Defaults to
    "..\..\consoles" relative to this script (i.e. <repo>\consoles).

.PARAMETER Core
    Optional list of cores to vendor. Valid names: nestopia, bsnes, mgba,
    desmume, project64, dolphin, azahar. Defaults to all of them.

.PARAMETER Force
    Overwrite an existing upstream folder even if it is already populated.

.EXAMPLE
    .\vendor-emulators.ps1
    Copies every available emulator source tree into cores/<core>/upstream.

.EXAMPLE
    .\vendor-emulators.ps1 -Core bsnes,nestopia -Force
    Re-vendors only the two cores that build by default.
#>
[CmdletBinding()]
param(
    [string]   $SourceRoot = (Join-Path $PSScriptRoot '..\..\consoles'),
    [string[]] $Core,
    [switch]   $Force
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SourceRoot  = (Resolve-Path $SourceRoot).Path

# core name -> upstream path (relative to SourceRoot), destination (relative to
# the project) and a marker file used to confirm a valid source tree.
$Cores = [ordered]@{
    nestopia  = @{ Src = 'nes\nestopia-master';                  Dst = 'cores\nestopia\upstream';  Marker = 'source\core\NstCore.hpp' }
    bsnes     = @{ Src = 'snes\bsnes-master';                    Dst = 'cores\bsnes\upstream';     Marker = 'bsnes\emulator\emulator.cpp' }
    mgba      = @{ Src = 'gameboyadvanced and gbc\mgba-master';  Dst = 'cores\mgba\upstream';      Marker = 'include\mgba\core\core.h' }
    desmume   = @{ Src = 'Ds\desmume-master\desmume';            Dst = 'cores\desmume\upstream';   Marker = 'src\NDSSystem.cpp' }
    project64 = @{ Src = 'n64\project64-develop';                Dst = 'cores\project64\upstream'; Marker = 'Source\Project64' }
    dolphin   = @{ Src = 'gamecube and wii\dolphin-master';      Dst = 'cores\dolphin\upstream';   Marker = 'Source\Core\Core\Core.h' }
    azahar    = @{ Src = '3ds\azahar-master';                    Dst = 'cores\azahar\upstream';    Marker = 'src\core\core.h' }
}

if (-not $Core -or $Core.Count -eq 0) { $Core = $Cores.Keys }

Write-Host "Project    : $ProjectRoot"
Write-Host "Source root: $SourceRoot"
Write-Host ""

$vendored = 0
$skipped  = 0
$missing  = 0

foreach ($name in $Core) {
    if (-not $Cores.Contains($name)) {
        Write-Warning "Unknown core '$name' (valid: $($Cores.Keys -join ', '))"
        continue
    }

    $info = $Cores[$name]
    $src  = Join-Path $SourceRoot $info.Src
    $dst  = Join-Path $ProjectRoot $info.Dst
    $mark = Join-Path $src $info.Marker

    if (-not (Test-Path $mark)) {
        Write-Warning "[$name] source not found (looked for '$mark') - skipping"
        $missing++
        continue
    }

    if ((Test-Path (Join-Path $dst $info.Marker)) -and -not $Force) {
        Write-Host "[$name] already vendored at $dst (use -Force to overwrite)"
        $skipped++
        continue
    }

    Write-Host "[$name] copying $src"
    Write-Host "        -> $dst"
    if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
    New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null
    Copy-Item -Recurse -Force $src $dst
    $vendored++
}

Write-Host ""
Write-Host "Done. vendored=$vendored skipped=$skipped missing=$missing"
if ($vendored -gt 0) {
    Write-Host "Re-run CMake so the build picks up the new paths."
}
