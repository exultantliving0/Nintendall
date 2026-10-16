# Measures every core the same way: launches it with a ROM, lets it settle, then
# reports the per-frame cost from video-modes.log.  Used to tell an
# emulator-bound core (CPU time near the 16.7 ms budget) from a frontend-bound
# one (CPU time small, wall time dominated by waiting), and to check a change
# actually moved the number instead of guessing.
param(
    [int]$Seconds = 25,
    [string]$OutDir = "build"
)

$roms = @(
    @{ Name = 'NES';  File = 'Mega Man (USA).zip' },
    @{ Name = 'SNES'; File = 'Super Metroid (Japan, USA) (En,Ja).zip' },
    @{ Name = 'GB';   File = "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (SGB Enhanced) (GB Compatible).zip" },
    @{ Name = 'GBA';  File = 'Legend of Zelda, The - The Minish Cap (USA).zip' },
    @{ Name = 'N64';  File = 'Super Mario 64 (USA).zip' }
)

$exe = "$PWD\build\bin\nintendall.exe"
$log = "$PWD\build\bin\video-modes.log"
$romDir = 'C:\Users\Exult\Documents\emulators\games-used-to-test'

"{0,-5} {1,10} {2,11} {3,10} {4,7} {5,10}" -f `
    'core','wall_ms','wall_max','cores_busy','fps','underruns'
'-' * 58

foreach ($r in $roms) {
    Get-Process nintendall -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 350
    Remove-Item $log -ErrorAction SilentlyContinue

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    # Built through ProcessStartInfo, not Start-Process: PowerShell's argument
    # quoting mangles paths containing parentheses and spaces.
    $psi.Arguments = '"' + (Join-Path $romDir $r.File) + '"'
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)

    # Let it settle past start-up, then sample the real cost.
    Start-Sleep -Seconds ([Math]::Max(5, $Seconds - 8))
    if ($proc.HasExited) {
        "{0,-5} {1}" -f $r.Name, "DIED (0x$('{0:X8}' -f $proc.ExitCode))"
        continue
    }
    $live = Get-Process -Id $proc.Id
    $cpu0 = $live.CPU
    Start-Sleep -Seconds 8
    $live = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
    if (-not $live) { "{0,-5} {1}" -f $r.Name, 'DIED during sample'; continue }
    # CPU seconds burned per second of wall time, i.e. how many cores it keeps
    # busy.  1.0 means one core saturated; 5.0 means five.
    $coresBusy = ($live.CPU - $cpu0) / 8.0
    Stop-Process -Id $proc.Id -Force
    Start-Sleep -Milliseconds 350

    # Average over the second half, so start-up does not skew it.
    $lines = Get-Content $log -ErrorAction SilentlyContinue |
             Where-Object { $_ -like '*frame wall=*' }
    if (-not $lines) { "{0,-5} {1}" -f $r.Name, 'no data'; continue }

    $tail = $lines | Select-Object -Last ([Math]::Min(5, $lines.Count))
    $wall = 0.0; $wmax = 0.0; $fps = 0; $un = 0
    foreach ($l in $tail) {
        if ($l -match 'fps=(\d+)')          { $fps = [int]$matches[1] }
        if ($l -match 'underruns=(\d+)')     { $un  = [int]$matches[1] }
        if ($l -match 'wall=([\d.]+) ms')   { $wall += [double]$matches[1] }
        if ($l -match 'wallmax=([\d.]+)')    { $wmax += [double]$matches[1] }
    }
    $n = [Math]::Max(1, $tail.Count)
    "{0,-5} {1,10:N2} {2,11:N1} {3,10:N2} {4,7} {5,10}" -f `
        $r.Name, ($wall/$n), ($wmax/$n), $coresBusy, $fps, $un
}