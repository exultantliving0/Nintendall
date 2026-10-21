<#
.SYNOPSIS
    Builds Nintendall-Setup.exe: one self-contained, signed installer.

.DESCRIPTION
    Compiles the installer stub and the uninstaller from installer\setup.cpp,
    collects the files that make up the product, embeds them in the stub as an
    RCDATA resource, and signs the result.

    No NSIS / Inno Setup / 7-Zip required.

    Why a resource and not an appended blob: Authenticode signing treats
    everything after the last PE section as an overlay and relocates it to sit
    after the certificate table. A signed installer with an appended payload
    therefore comes out with its payload moved and its offsets pointing at
    nothing. Keeping the payload inside the image means signing leaves it
    exactly where it is.

.PARAMETER OutputDir
    Where to drop the installer.  Defaults to the repository root.

.PARAMETER SkipBuild
    Reuse the existing nintendall.exe instead of rebuilding it.

.PARAMETER NoSign
    Build without signing, even if a certificate is available.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\make-setup.ps1
#>
[CmdletBinding()]
param(
    [string] $OutputDir = '',
    [switch] $SkipBuild,
    [switch] $NoSign
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo      = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = $repo }
$buildDir  = Join-Path $repo 'build'
$stageDir  = Join-Path $buildDir 'stage'
$payload   = Join-Path $buildDir 'package.bin'
$rcFile    = Join-Path $buildDir 'payload.rc'
$rcObj     = Join-Path $buildDir 'payload_res.o'
$iconRc    = Join-Path $buildDir 'icon.rc'
$iconObj   = Join-Path $buildDir 'icon_res.o'
$iconPath  = Join-Path $repo 'assets\nintendall.ico'
$stubExe   = Join-Path $buildDir 'setup_stub.exe'
$uninstExe = Join-Path $buildDir 'uninstall_stub.exe'
$appExe    = Join-Path $buildDir 'bin\nintendall.exe'
$setupSrc  = Join-Path $repo 'installer\setup.cpp'

function Write-Step($msg) {
    Write-Host "==> $msg" -ForegroundColor Cyan
}

# -----------------------------------------------------------------------------
# Signing
#
# Unsigned executables are refused by Smart App Control, so signing is what
# makes the app and the installer launchable.  Skipped when no certificate has
# been created, so a fresh checkout still builds.
# -----------------------------------------------------------------------------
$signScript = Join-Path $PSScriptRoot 'sign.ps1'
$thumbFile  = 'C:\Users\Exult\AppData\Local\Temp\opencode\cert-thumb.txt'
$haveCert   = (-not $NoSign) -and (Test-Path $signScript) -and (Test-Path $thumbFile)

function Sign-IfPossible($file) {
    if (-not $haveCert) { return }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $signScript -Path $file
    if ($LASTEXITCODE -ne 0) { throw "Signing failed for $file" }
}

# -----------------------------------------------------------------------------
# 1. The application itself
# -----------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Step 'Building nintendall.exe'
    cmake --build $buildDir --target nintendall
    if ($LASTEXITCODE -ne 0) { throw 'Building nintendall.exe failed.' }
}
if (-not (Test-Path $appExe)) { throw "Missing $appExe - build it first." }

# Signed before staging, so the copy that ships is the signed one.
Sign-IfPossible $appExe

# -----------------------------------------------------------------------------
# 2. Icon resource (shared by the installer and the uninstaller)
# -----------------------------------------------------------------------------
if (-not (Test-Path $iconPath)) { throw "Missing $iconPath" }
Write-Step 'Compiling icon resource'
# The .rc lives in build\, so this path is resolved relative to the repo root.
# Forward slashes avoid the RC string parser treating backslashes as escapes.
Set-Content -Path $iconRc -Value '1 ICON "../assets/nintendall.ico"' -Encoding ascii
& windres $iconRc '-O' 'coff' '-o' $iconObj
if ($LASTEXITCODE -ne 0) { throw 'windres failed to compile the icon resource.' }

# -----------------------------------------------------------------------------
# 3. Uninstaller stub
# -----------------------------------------------------------------------------
# Libraries must follow the source file on the command line, or the linker
# drops them before it has seen the objects that need them.
# -static matters: without it the installer imports libstdc++-6.dll and
# libgcc_s_seh-1.dll, and then fails to start on any machine that does not
# have MinGW installed ("libstdc++-6.dll is missing").
$gxxFlags = @('-O2', '-std=c++17', '-municode', '-static', '-s')
$gxxLibs  = @(
    '-luser32', '-lgdi32', '-lshell32', '-lshlwapi',
    '-lole32', '-lcomctl32', '-ladvapi32', '-luuid'
)

Write-Step 'Compiling uninstaller'
& g++ @gxxFlags '-DUNINSTALLER' '-o' $uninstExe $setupSrc $iconObj @gxxLibs
if ($LASTEXITCODE -ne 0) { throw 'Compiling the uninstaller stub failed.' }
Sign-IfPossible $uninstExe

# -----------------------------------------------------------------------------
# 4. Collect the payload
# -----------------------------------------------------------------------------
Write-Step 'Collecting payload'
if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
if (Test-Path $payload)  { Remove-Item -Force $payload }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

# SDL3.dll must be copied from build output (dynamic linking)
$sdl3Dll = Join-Path $buildDir 'bin\SDL3.dll'

$files = @(
    @{ From = $appExe;                            To = 'nintendall.exe' }
    @{ From = $sdl3Dll;                            To = 'SDL3.dll' }
    @{ From = $uninstExe;                          To = 'Uninstall.exe' }
    @{ From = (Join-Path $repo 'README.md');       To = 'README.md' }
    @{ From = (Join-Path $repo 'assets\config.ini');   To = 'assets\config.ini' }
    @{ From = (Join-Path $repo 'docs\ARCHITECTURE.md'); To = 'docs\ARCHITECTURE.md' }
)
foreach ($f in $files) {
    if (Test-Path $f.From) {
        $dest = Join-Path $stageDir $f.To
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
        Copy-Item $f.From $dest -Force
    }
}

# -----------------------------------------------------------------------------
# 5. Build the package: header + records + path table + raw file bytes
# -----------------------------------------------------------------------------
Write-Step 'Building package'
$utf8 = New-Object System.Text.UTF8Encoding($false)

$entries = @()
Get-ChildItem $stageDir -Recurse -File | ForEach-Object {
    $entries += [pscustomobject]@{
        Path = $_.FullName.Substring($stageDir.Length + 1)
        Bytes = $_.FullName
        Size  = $_.Length
        Dir   = $false
    }
}
Get-ChildItem $stageDir -Recurse -Directory | ForEach-Object {
    $entries += [pscustomobject]@{
        Path = $_.FullName.Substring($stageDir.Length + 1)
        Bytes = $null
        Size  = 0
        Dir   = $true
    }
}
# Shallowest first, so a directory is always created before what is inside it.
$entries = @($entries | Sort-Object { $_.Path.Split('\').Count }, Path)

$blob = New-Object System.IO.MemoryStream
$bwBlob = New-Object System.IO.BinaryWriter($blob)
foreach ($e in $entries) {
    $e | Add-Member -NotePropertyName Offset -NotePropertyValue ([int64]$blob.Position)
    if ($e.Dir) { continue }
    $data = [System.IO.File]::ReadAllBytes($e.Bytes)
    $bwBlob.Write($data, 0, $data.Length)
}
$bwBlob.Flush()
$blobBytes = $blob.ToArray()

$magic = [byte[]](0x4E,0x54,0x44,0x4C,0x50,0x4B,0x47,0x00)   # "NTDLPKG\0"

$fs = [System.IO.File]::Open($payload, [System.IO.FileMode]::Create)
try {
    $bw = New-Object System.IO.BinaryWriter($fs)

    # footer: magic, version, count, blob offset (relative to this package)
    $bw.Write($magic)
    $bw.Write([uint32]1)
    $bw.Write([uint32]$entries.Count)
    $bw.Write([int64]0)            # patched below once the header size is known

    # All fixed-size records first...
    foreach ($e in $entries) {
        $pw = $utf8.GetBytes($e.Path)
        $flag = if ($e.Dir) { 1 } else { 0 }
        $bw.Write([int64]$e.Offset)
        $bw.Write([int64]$e.Size)
        $bw.Write([uint32]$pw.Length)
        $bw.Write([byte]$flag)                                      # is_dir
        $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([byte]0)  # pad
    }
    # ...then every path string, in the same order.
    foreach ($e in $entries) {
        $pw = $utf8.GetBytes($e.Path)
        $bw.Write($pw)
    }
    $bw.Flush()

    $headerLen = $fs.Position
    $fs.Position = $magic.Length + 4 + 4
    $bw.Write([int64]$headerLen)
    $bw.Flush()

    $fs.Position = $headerLen
    $fs.Write($blobBytes, 0, $blobBytes.Length)
    $fs.Flush()
} finally {
    $fs.Dispose()
}

$count = $entries.Count
$bytes = '{0:N0}' -f $blobBytes.Length
$total = '{0:N0}' -f (Get-Item $payload).Length
Write-Host "    $count files, $bytes bytes of content, $total bytes packaged"

# -----------------------------------------------------------------------------
# 6. Embed the package and link the installer
# -----------------------------------------------------------------------------
Write-Step 'Embedding payload and linking installer'

# Resource id 1 matches PKG_RESOURCE_ID in installer\setup.cpp.  windres
# resolves the filename relative to the .rc file, so both live in build\.
Set-Content -Path $rcFile -Value "1 RCDATA `"package.bin`"" -Encoding ascii

& windres $rcFile '-O' 'coff' '-o' $rcObj
if ($LASTEXITCODE -ne 0) { throw 'windres failed to embed the payload.' }

& g++ @gxxFlags '-o' $stubExe $setupSrc $rcObj $iconObj @gxxLibs
if ($LASTEXITCODE -ne 0) { throw 'Linking the installer stub failed.' }

$outExe = Join-Path $OutputDir 'Nintendall-Setup.exe'
Copy-Item $stubExe $outExe -Force

# Signing is the very last step, now that the bytes are final.
Sign-IfPossible $outExe

if (-not $haveCert) {
    Write-Host ''
    Write-Warning 'No signing certificate available - the installer is UNSIGNED.'
    Write-Warning 'Smart App Control will refuse to run it. Create one with'
    Write-Warning '  tools\new-signing-cert.ps1'
}

$len = (Get-Item $outExe).Length
$mb  = '{0:N1} MB' -f ($len / 1MB)
Write-Host ''
Write-Host "Built $outExe  ($mb)" -ForegroundColor Green
Write-Host 'Ship that single file to anyone; double-clicking it installs everything.'