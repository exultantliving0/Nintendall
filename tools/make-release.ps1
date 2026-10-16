# Rebuilds the three files that get attached to a GitHub release, so they can
# never drift from the binaries the installer actually ships.
#
#   Nintendall-Setup.exe        the installer itself (built by make-setup.ps1)
#   Nintendall-Portable.zip     the same payload, unpacked, for people who
#                               would rather not run an installer
#   Nintendall-Setup.exe.sha256 hash of the installer, in sha256sum format
#
# Run tools\make-setup.ps1 first; this reads what it staged rather than
# rebuilding anything, so the two can never disagree.
[CmdletBinding()]
param(
    [string] $OutputDir = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo     = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = $repo }
$stageDir = Join-Path $repo 'build\stage'
$setupExe = Join-Path $OutputDir 'Nintendall-Setup.exe'

if (-not (Test-Path $stageDir)) { throw "No staged payload at $stageDir - run tools\make-setup.ps1 first." }
if (-not (Test-Path $setupExe)) { throw "No installer at $setupExe - run tools\make-setup.ps1 first." }

# --- portable zip ----------------------------------------------------------
# Rebuilt from scratch so a leftover entry from a previous run cannot survive.
$zipPath = Join-Path $OutputDir 'Nintendall-Portable.zip'
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, 'Create')
try {
    foreach ($f in Get-ChildItem $stageDir -Recurse -File) {
        $rel = $f.FullName.Substring($stageDir.Length + 1)
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $f.FullName, $rel, [System.IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $zip.Dispose()
}
Write-Host "==> Nintendall-Portable.zip" -ForegroundColor Cyan

# --- installer hash --------------------------------------------------------
# Written in the "<hash>  <filename>" form sha256sum produces, so it can be
# checked with `certutil -hashfile` or any standard tool without editing.
$hash = (Get-FileHash $setupExe -Algorithm SHA256).Hash.ToLower()
$hashFile = Join-Path $OutputDir 'Nintendall-Setup.exe.sha256'
"$hash  Nintendall-Setup.exe" | Set-Content -Path $hashFile -Encoding ascii -NoNewline
Write-Host "==> Nintendall-Setup.exe.sha256" -ForegroundColor Cyan

Write-Host ''
foreach ($n in @('Nintendall-Setup.exe', 'Nintendall-Portable.zip')) {
    $i = Get-Item (Join-Path $OutputDir $n)
    '{0,-26} {1,7:N2} MB' -f $n, ($i.Length / 1MB)
}
$h = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLower()
"Nintendall-Portable.zip sha256 $h"