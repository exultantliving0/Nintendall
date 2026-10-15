<#
.SYNOPSIS
    Signs the Nintendall binaries so Smart App Control will run them.

.DESCRIPTION
    Unsign executables are refused by Smart App Control on most Windows 11
    machines, which stops the app and the installer from launching at all. A
    valid Authenticode signature fixes that, and is also what lets other
    people's PCs install the software cleanly.

    Uses the development certificate created by tools\new-signing-cert.ps1,
    which is trusted in LocalMachine\Root and LocalMachine\TrustedPublisher.

    Signing order matters: a signature covers the bytes of the file, so
    nintendall.exe must be signed before it is packaged into the installer, and
    the installer must be signed only after its payload has been appended.

.PARAMETER Path
    Files to sign.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\sign.ps1 -Path .\build\bin\nintendall.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]] $Path,

    # Skip the RFC 3161 timestamp service (offline builds).
    [switch] $NoTimestamp
)

$ErrorActionPreference = 'Stop'

$thumbFile = 'C:\Users\Exult\AppData\Local\Temp\opencode\cert-thumb.txt'
if (-not (Test-Path $thumbFile)) {
    throw 'No signing certificate found. Run tools\new-signing-cert.ps1 first.'
}
$thumb = (Get-Content $thumbFile -Raw).Trim()

$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter signtool.exe |
            Where-Object { $_.DirectoryName -match '\\x64$' } |
            Sort-Object FullName | Select-Object -Last 1
if (-not $signtool) { throw 'signtool.exe not found (Windows SDK missing).' }
$signtool = $signtool.FullName

# Signing fails if the cert chain is not trusted, so check before we start.
$cert = Get-Item "Cert:\CurrentUser\My\$thumb" -ErrorAction SilentlyContinue
if (-not $cert) { throw "Certificate $thumb is not in CurrentUser\My." }

$tsUrl = 'http://timestamp.digicert.com'

foreach ($f in $Path) {
    if (-not (Test-Path $f)) { Write-Warning "skipping missing file: $f"; continue }

    # Options must precede the filename: signtool treats anything after the
    # file as another file to sign, which is why /tr could not just be appended.
    $base = @('sign', '/a', '/sha1', $thumb, '/fd', 'SHA256', '/td', 'SHA256')

    Write-Host "signing $(Split-Path -Leaf $f)"

    if (-not $NoTimestamp) {
        $ok = & $signtool @base '/tr' $tsUrl $f 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host '    signed and timestamped'
            continue
        }
        Write-Warning "    timestamping unavailable, signing without it ($($ok -join ' '))"
    }

    & $signtool @base $f
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $f (exit $LASTEXITCODE)" }
    Write-Host '    signed'
}

# Verify everything we just signed.
$failed = $false
foreach ($f in $Path) {
    if (-not (Test-Path $f)) { continue }
    $out = & $signtool verify /pa $f 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "verified: $(Split-Path -Leaf $f)"
    } else {
        Write-Host "VERIFY FAILED: $(Split-Path -Leaf $f)" -ForegroundColor Red
        $out | ForEach-Object { Write-Host "    $_" }
        $failed = $true
    }
}

if ($failed) { exit 1 }
Write-Host ''
Write-Host 'All binaries signed and verified.' -ForegroundColor Green