# Nintendall

Many Nintendo systems in one executable. Load a ROM, the console is detected
from the file itself, and the matching core runs it — no separate emulator, no
per-game setup.

## For players

**Download `Nintendall-Setup.exe` and run it.** That single file is the whole
program. It installs to

```
%LOCALAPPDATA%\Programs\Nintendall
```

which needs no administrator rights, and puts a shortcut on the desktop and in
the Start menu. To remove it, use *Settings → Apps → Installed apps → Nintendall*.

### Running it

| Action | Key |
| --- | --- |
| Load a ROM | `Ctrl+O` or *File → Load ROM…* |
| Fullscreen (no title bar, no borders) | `F11` (again, or `Esc`, to leave) |
| Pause | `Space` |
| Reset | `F2` |
| Save / load state | `F5` / `F8`, or `1`–`9` for a slot |

`.zip` ROMs are opened directly. Battery saves and save states are written to a
`saves` folder beside the executable, in a subfolder per console
(`saves\n64`, `saves\gba`, …), so they survive closing and reopening the app.

## For developers

```powershell
cmake --preset all-cores
cmake --build --preset all-cores
```

Cores: NES (Nestopia), SNES (bsnes), GB/GBC/GBA (mGBA), NDS, N64, GameCube/Wii,
3DS. The `release` preset drops the optional ones and builds faster.

### Building the installer

```powershell
powershell -ExecutionPolicy Bypass -File tools\make-setup.ps1
```

This compiles `installer/setup.cpp` twice — once as the installer, once as the
uninstaller — collects the payload, and appends it to the installer stub. The
result is a single self-contained `Nintendall-Setup.exe`.

No NSIS, Inno Setup or 7-Zip is needed: the stub reads the payload back out of
its own executable at runtime.

Layout of the generated file:

```
[ stub executable        ]
[ footer + index + paths ]   <- 8-byte pointer in the last 8 bytes of the file
[ raw file bytes         ]
```

To check the packaging without running it (useful on machines where Smart App
Control refuses to launch fresh unsigned binaries):

```powershell
powershell -ExecutionPolicy Bypass -File tools\verify-setup.ps1
```

It parses the package exactly as the installer does, unpacks it to a scratch
folder and compares every file against the staged original.

### Layout

```
src/framework/   shared core API, ROM detection, save states
src/app/         the Win32 frontend
cores/<name>/    one adapter per console plus its vendored upstream tree
installer/       setup / uninstall stub
tools/           build and packaging scripts
```

## Notes

Smart App Control blocks freshly compiled, unsigned binaries. If Windows
refuses to start `nintendall.exe` or the installer, either turn Smart App
Control off (*Windows Security → App & browser control*), or sign the binaries.