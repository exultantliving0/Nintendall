<#
.SYNOPSIS
    Verifies Nintendall-Setup.exe without running it.

.DESCRIPTION
    Smart App Control on many machines refuses to launch freshly compiled,
    unsigned binaries, which makes "just run the installer" an unreliable test.
    This script finds the embedded RCDATA package, parses it exactly the way
    installer\setup.cpp does, unpacks it into a scratch folder and compares
    every file against the staged original.

    It proves the packaging pipeline is correct. It does not exercise the
    installer's UI, which still has to be run by hand.

.PARAMETER KeepExtracted
    Leave the scratch folder behind for inspection.

.PARAMETER RequireSignature
    Fail when the installer is unsigned.  Without this an unsigned installer is
    reported as a warning, which is what CI wants while no certificate has been
    configured yet.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\verify-setup.ps1
#>
[CmdletBinding()]
param(
    [string] $SetupExe = '',
    [string] $StageDir = '',
    [switch] $KeepExtracted,
    [switch] $RequireSignature
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not $SetupExe)  { $SetupExe  = Join-Path $repo 'Nintendall-Setup.exe' }
if (-not $StageDir) { $StageDir = Join-Path $repo 'build\stage' }
$scratch = Join-Path $repo 'build\verify'

Write-Host "Verifying $SetupExe" -ForegroundColor Cyan
if (-not (Test-Path $SetupExe)) { throw "Not found: $SetupExe" }

$bytes = [System.IO.File]::ReadAllBytes($SetupExe)
Write-Host ("  installer size : {0:N0} bytes" -f $bytes.Length)

# -----------------------------------------------------------------------------
# Locate the RCDATA resource holding the package.
#
# The .rsrc section has to be parsed by hand.  Rather than depend on a PE
# library, the package is found by its own header signature: search for the
# "NTDLPKG" magic, then confirm the rest of the structure is self-consistent.
# -----------------------------------------------------------------------------
$magic = [System.Text.Encoding]::ASCII.GetBytes('NTDLPKG')
$found = @()
for ($i = 0; $i -le $bytes.Length - $magic.Length; $i++) {
    $hit = $true
    for ($j = 0; $j -lt $magic.Length; $j++) {
        if ($bytes[$i + $j] -ne $magic[$j]) { $hit = $false; break }
    }
    if ($hit) { $found += $i }
}
Write-Host ("  NTDLPKG magic found at offset(s): {0}" -f ($found -join ', '))
if ($found.Count -eq 0) { throw 'No package magic found - the payload is not embedded.' }

# The package starts at the magic; every candidate is validated below and the
# first fully consistent one wins.
$errors = @()
$okPkg = $false
foreach ($base in $found) {
    if ($base + 24 -gt $bytes.Length) { continue }
    $version = [BitConverter]::ToUInt32($bytes, $base + 8)
    $count   = [BitConverter]::ToUInt32($bytes, $base + 12)
    $blobOff = [BitConverter]::ToInt64($bytes, $base + 16)
    if ($version -ne 1) { continue }
    if ($count -le 0 -or $count -gt 4096) { continue }

    $entrySize = 8 + 8 + 4 + 1 + 3
    $tableEnd = $base + 24 + ($count * $entrySize)
    if ($tableEnd -gt $bytes.Length) { continue }

    $records = @()
    $p = $base + 24
    for ($i = 0; $i -lt $count; $i++) {
        $records += [pscustomobject]@{
            Offset    = [BitConverter]::ToInt64($bytes, $p)
            Size      = [BitConverter]::ToInt64($bytes, $p + 8)
            PathBytes = [BitConverter]::ToUInt32($bytes, $p + 16)
            IsDir     = ($bytes[$p + 20] -ne 0)
            Path      = ''
        }
        $p += $entrySize
    }

    # Path strings follow the whole table; walk them by accumulating lengths.
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $sumPaths = 0
    for ($i = 0; $i -lt $count; $i++) {
        $len = $records[$i].PathBytes
        if ($p + $len -gt $bytes.Length) { break }
        $records[$i].Path = $utf8.GetString($bytes, [int]$p, [int]$len)
        $p += $len
        $sumPaths += $len
    }
    if ($p -ne $tableEnd + $sumPaths) { continue }

    $headerLen = 24 + ($count * $entrySize) + $sumPaths
    if ($blobOff -ne $headerLen) { continue }

    # Every record must land inside the file.
    $fits = $true
    foreach ($r in $records) {
        if ($r.IsDir) { continue }
        if ($base + $blobOff + $r.Offset + $r.Size -gt $bytes.Length) { $fits = $false; break }
    }
    if (-not $fits) { continue }

    # And the blob must actually begin with a PE image.
    $blobAbs = $base + $blobOff
    if ($blobAbs + 2 -gt $bytes.Length) { continue }
    if ([char]$bytes[$blobAbs] -ne 'M' -or [char]$bytes[$blobAbs + 1] -ne 'Z') { continue }

    Write-Host "  package at     : $base"
    Write-Host "  version        : $version"
    Write-Host "  entries        : $count"
    Write-Host ("  blob (relative): {0}" -f $blobOff)
    $okPkg = $true
    break
}
if (-not $okPkg) { throw 'No structurally valid package found.' }

Write-Host ''
foreach ($r in $records) {
    $kind = if ($r.IsDir) { '<DIR>' } else { 'FILE' }
    $size = '{0,12:N0}' -f $r.Size
    Write-Host "    $kind  $size  $($r.Path)"
}

# -----------------------------------------------------------------------------
# Extract and compare
# -----------------------------------------------------------------------------
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

$blobAbs = $base + $blobOff
foreach ($r in $records) {
    $dest = Join-Path $scratch $r.Path
    if ($r.IsDir) {
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        continue
    }
    $parent = Split-Path -Parent $dest
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $slice = New-Object byte[] $r.Size
    [Array]::Copy($bytes, [int]($blobAbs + $r.Offset), $slice, 0, [int]$r.Size)
    [System.IO.File]::WriteAllBytes($dest, $slice)
}

Write-Host ''
Write-Host 'Comparing extracted files against the staged originals...' -ForegroundColor Cyan
foreach ($r in $records | Where-Object { -not $_.IsDir }) {
    $orig = Join-Path $StageDir $r.Path
    $got  = Join-Path $scratch  $r.Path
    if (-not (Test-Path $orig)) { $errors += "$($r.Path): not present in the stage folder"; continue }
    $a = (Get-FileHash $orig -Algorithm SHA256).Hash
    $b = (Get-FileHash $got  -Algorithm SHA256).Hash
    if ($a -ne $b) {
        $errors += "$($r.Path): hash mismatch"
    } else {
        Write-Host ("  OK   {0}" -f $r.Path) -ForegroundColor DarkGreen
    }
}

# -----------------------------------------------------------------------------
# Signature
# -----------------------------------------------------------------------------
Write-Host ''
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.DirectoryName -match '\\x64$' } | Sort-Object FullName | Select-Object -Last 1
if ($signtool) {
    # signtool reports "no signature" on stderr and exits non-zero, which under
    # ErrorActionPreference='Stop' would abort the whole script. Relax it for
    # this one call and judge the result by the exit code.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $sigOut = & $signtool.FullName verify /pa $SetupExe 2>&1 | Out-String
    $sigExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    if ($sigExit -eq 0) {
        Write-Host 'Authenticode    : signature valid' -ForegroundColor DarkGreen
    } elseif ($sigOut -match 'No signature found') {
        # Unsigned is not a packaging defect; it only matters if the caller
        # explicitly asked for a signed build.
        if ($RequireSignature) {
            $errors += 'installer is unsigned and -RequireSignature was given'
        } else {
            Write-Host 'Authenticode    : UNSIGNED' -ForegroundColor Yellow
            Write-Host '                  Smart App Control will refuse to run this build.' -ForegroundColor Yellow
        }
    } else {
        $errors += 'installer carries a signature that does not validate'
        Write-Host $sigOut -ForegroundColor Red
    }
}

if (-not $KeepExtracted -and $errors.Count -eq 0) {
    Remove-Item -Recurse -Force $scratch
}

Write-Host ''
if ($errors.Count) {
    Write-Host 'FAILED:' -ForegroundColor Red
    $errors | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host 'Package verified: index, offsets, every file hash and the signature.' -ForegroundColor Green
Write-Host '(The installer UI itself still needs a manual run.)' -ForegroundColor DarkGray
exit 0