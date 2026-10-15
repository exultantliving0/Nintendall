# Architecture

How the Nintendall emulator is put together, and why.

## Design goal

Run many Nintendo consoles from **one executable**, without the user installing
or downloading a separate emulator for each system, and without rewriting any
emulator from scratch.

The chosen shape is a **shared-library host**:

* each emulator is built once, as a **static library**;
* a thin **adapter** per core implements a common **C API**;
* a single **frontend** links every core in and drives whichever one matches
  the loaded ROM.

This keeps the emulators untouched (we compile upstream sources as-is and adapt
at the edges) while presenting one program to the user.

## Layers

```
src/app/               the Win32 GUI: one window, one ~60 Hz emulation thread
      │  ▲             State menu (slots 1–9), back-buffer video, waveOut audio
      │  └──────────── talks to cores only through the module vtable and
      ▼                receives video / audio / input through the callbacks
src/framework/         the common C API and building-block library
      │                  emu_api        callbacks, EmuConfig, results
      │                  core_registry  console → EmuCoreModule lookup
      │                  rom_detect     header sniffing → EmuConsoleType
      │                  save_state     .uestate container + slot manager
      │                  zip            built-in .zip reader + DEFLATE
      │                  emu_util       file / string / CRC helpers
      ▼
cores/<name>/          one adapter per console (nes_core.cpp, snes_core.cpp, …)
      │                  drives the upstream emulator's own API
      ▼
cores/<name>/upstream/ the unmodified upstream emulator sources
```

Three rules keep it clean:

1. **The frontend knows nothing about any specific emulator.** It only ever
   talks to the framework and to an `EmuCoreModule`.
2. **An adapter knows nothing about the frontend.** It only sees the callbacks
   in `emu_api.h` and the upstream emulator's own API.
3. **The framework knows nothing about either.** It defines types and helpers.

## The common C API (`emu_api.h`)

Everything crossing a boundary is plain C, so it does not matter whether a core
is C, C++, or a mix, or which compiler flags it uses.

The model is a small "core instance":

* `EmuConfig` — video/audio/region/path settings handed in at creation.
* `EmuCoreHandle` — opaque handle to one running console.
* Callbacks the core calls back into the host:
  * `EmuVideoCallback(userdata, pixels, w, h, pitch)` — 32-bit RGBA, top-down.
  * `EmuAudioCallback(userdata, samples, count)` — stereo int16, `count` frames.
  * `EmuInputPollCallback` / `EmuInputStateCallback` — the core asks the host
    what a button is doing, using the `EMU_ID_*` id space.
* Operations: create / destroy / load ROM / run one frame / reset / power
  cycle / pause / save+load state / save+load SRAM / set input device.

The functions in `emu_api.h` (`emu_core_create`, …) are the *conceptual* API.
The frontend actually calls them **through the module table** (below) so that
several cores can coexist in one process without symbol collisions.

## Core registry and the module vtable

Each core exports exactly one symbol: an `EmuCoreModule`
(`src/framework/core_registry.h`). It is a struct of function pointers — a
vtable — whose order must match the struct exactly:

```
id, get_info, create, destroy,
load_rom, load_rom_file, unload_rom,
run_frame, reset, power_cycle,
set_paused, is_paused, set_fast_forward,
save_state, load_state,
save_sram, load_sram,
set_input_device
```

`core_registry.cpp` contains a table of the cores that were compiled in,
gated by `EMU_ENABLE_<CORE>` macros that the build defines. The frontend asks
the registry for the right core:

```c
EmuConsoleType console = emu_detect_console_from_file(path, &details);
const EmuCoreModule* mod = emu_registry_find_for_console(console);
EmuCoreHandle core = mod->create(&cfg, video_cb, audio_cb, NULL, input_cb, NULL);
```

Family matching is built in: `EMU_CONSOLE_GAMEBOY`, `_COLOR` and `_ADVANCE`
all resolve to the GB-family core; `EMU_CONSOLE_GAMECUBE` and `_WII` resolve to
the GC/Wii core.

## Frontend flow

```
WM_COMMAND(Open ROM)
        │  GetOpenFileName
        ▼
emu_detect_console_from_file(path, &detect)      →  console + crc + details
        │
emu_registry_find_for_console(console)           →  module
        │
mod->create(config, video, audio, poll, state, this)
        │
mod->load_rom_file(core, path)
        │
   spawn emulation thread ─────────────────────────────────────┐
        │                                                      │
        │  each ~1/60 s:                                       │
        │     input_poll(); mod->run_frame(core);              │
        │        core → video_cb(...) → copy frame → InvalidateRect
        │        core → audio_cb(...) → push into the audio ring
        ▼                                                      │
   WM_PAINT → compose into the offscreen back buffer → BitBlt  │
   AudioPump (once per frame) → hand finished buffers to waveOut
        └──────────────────────────────────────────────────────┘
   File → Exit / next Open:  mod->destroy(core)
```

* **Video:** the core renders into a 32-bit buffer; the frontend composes that
  frame into an offscreen memory DC and blits it to the window in a single
  `BitBlt` (stretched, letterboxed to preserve aspect). Compositing offscreen
  and blitting once is what keeps the picture from flickering.
* **Audio:** the core pushes int16 stereo into a ring buffer. Once per emulated
  frame the emulation thread (`AudioPump`) moves whole buffers to `waveOut` and
  never pads with silence, so there are no injected clicks. Pacing is derived
  from the number of samples the core actually emits per frame, which keeps
  every console (NES 801, SNES ~798) at its true speed.
* **Input:** the core asks through `EmuInputStateCallback`, which maps
  `EMU_ID_*` to `GetAsyncKeyState` on the fixed keyboard layout.

## Save states: the UESV container

Each emulator serializes its own state however it likes. The framework wraps
that opaque blob in a common header so the frontend can manage states
uniformly. Layout (little-endian), `src/framework/save_state.h`:

| off | size | field |
|-----|------|-------|
| 0   | 4    | magic `"UESV"` |
| 4   | 2    | format version (1) |
| 6   | 2    | console type |
| 8   | 4    | flags |
| 12  | 4    | header CRC32 (bytes `[0,12)`) |
| 16  | 32   | core name |
| 48  | 64   | ROM title |
| 112 | 4    | ROM CRC32 |
| 116 | 4    | unix timestamp |
| 120 | 8    | frame count |
| 128 | 8    | payload size |
| 136 | N    | **payload** (core-specific) |
| 136+N | 4  | payload CRC32 |
| 140+N | 4  | container CRC32 (bytes `[0,140+N)`) |

Three CRCs let the loader reject corrupt or mismatched files. The
`EmuStateManager` turns this into numbered slots at

```
<saves>/<rom_basename>.<ROM_CRC32>/slot<N>.uestate
```

so states for different games never collide. `EmuRewindBuffer` (a bounded ring
of snapshots) is also provided, but the current frontend does not expose
rewind.

## ROM detection

`rom_detect.cpp` does header inspection per console rather than trusting the
file extension alone:

* **NES** — `iNES`/`NES\x1A` header, mapper, PRG/CHR bank counts, battery flag.
* **SNES** — LoROM/HiROM/ExHiROM header scoring, title, region, size, and a
  coprocessor guess.
* **GB/GBC** — Nintendo logo bytes and the CGB flag.
* **GBA** — fixed entry point and logo, save type.
* **N64** — the byte-order magic of `.z64`/`.n64`/`.v64` images plus the ROM
  header (name, CIC, CRC) behind it.
* … and analogous checks for NDS / GC-Wii / 3DS.

Wrong or ambiguous extensions fall back to heuristics and a confidence score;
the detected CRC32 is reused to identify the save-state folder.

## Archives (`.zip`)

Most ROMs circulate as `.zip` files, so the framework treats an archive like a
ROM:

* `zip.cpp` reads the end-of-central-directory + central directory, lists
  entries, picks the one most likely to be a ROM (known extension first, junk
  like `*.txt`, `__MACOSX/` and hidden entries ignored, largest otherwise) and
  extracts it. Both **stored** and **deflate** entries are supported by a
  built-in DEFLATE decoder — no zlib/miniz dependency, so it works in every
  build (including NES-only). The extracted bytes are checked against the
  entry's stored CRC32.
* `emu_prepare_rom()` turns a user selection into a loadable path: for an
  archive it unpacks the ROM into `saves/rom-cache/<CRC32>_<name>.<ext>` and
  returns that; for a plain ROM it returns the path unchanged. The frontend
  calls this before detecting/loading, and uses the *inner* ROM name for the
  save-state folder and `.sav` name.
* Detection is archive-aware too: `emu_detect_console_from_file()` peeks inside
  a `.zip` so a console can be identified without unpacking first.

## Per-core adapters

### Nestopia (NES) — `cores/nestopia/nes_core.cpp`

* Uses Nestopia's public API (`NstApi*.hpp`).
* Video comes out of `Video::Output{pixels, pitch}`; audio from
  `Sound::Output` (mono int16, duplicated to stereo).
* Input maps into `Input::Controllers::Pad` bits
  (A=0x01, B=0x02, SELECT=0x04, START=0x08, UP=0x10, DOWN=0x20, LEFT=0x40,
  RIGHT=0x80).
* Built with `NST_NO_ZLIB` so no external zlib is needed. The optional scaler
  filters (`2xSaI`, `HqX`, `xBR`) are compiled out; `NstVideoFilterNone` plus
  the ScaleX/NTSC TUs stay in because the renderer references them.
* Save states go through Nestopia's state writer into a memory stream; the
  adapter hands the raw bytes to the framework.

### bsnes (SNES) — `cores/bsnes/snes_core.cpp`

* Implements bsnes's `Emulator::Platform` interface. bsnes calls back through
  it for file IO (`open`), cartridge load (`load`), video, audio and input.
* ROM handling: the adapter reads the ROM, strips a 512-byte copier header if
  present, and runs bsnes's own `Heuristics::SuperFamicom` to get title,
  region, manifest and the program/data/expansion/firmware ROM splits. Those
  are then served back to bsnes as in-memory `vfs::file`s keyed by name
  (`manifest.bml`, `program.rom`, `data.rom`, `expansion.rom`), plus the
  embedded `boards.bml` and `ipl.rom`.
* Video: bsnes delivers 15-bit palette indices, which the adapter converts to
  RGBA through a precomputed palette (`build_palette`).
* Audio: doubles → int16 stereo.
* Save states use `serialize()` / `unserialize(serializer)`.
* The filter library and the `target-bsnes`/`target-libretro` front-ends are
  excluded; only the emulator core and the SameBoy GB sub-core are compiled.
  The heuristic sources are `#include`d by the adapter, not compiled
  separately. `processor/sm83` is deliberately not built (bsnes's SNES build
  doesn't use it).

### mupen64plus (Nintendo 64) — `cores/n64/n64_core.cpp`

* Three upstream projects are vendored and linked statically behind one
  `n64_core` target: `mupen64plus-core` (the console), `mupen64plus-rsp-cxd4`
  (an LLE RSP interpreter) and `angrylion` (a software RDP).
* **Why an LLE RSP:** angrylion is an RDP-only renderer — it consumes a raw RDP
  command list. The graphics microcode (F3DEX) is what turns a display list into
  that command list, and it has to actually *run*. mupen64plus's bundled
  `mupen64plus-rsp-hle` is HLE: it pattern-matches known tasks and forwards the
  display list straight to the graphics plugin, so with angrylion nothing ever
  reaches the RDP and the screen stays blank. cxd4 executes the microcode, writes
  `DPC_START`/`DPC_END` and calls back into the video plugin's `ProcessRdpList()`
  — exactly the path angrylion implements.
* `n64_glue.c` owns every mupen64plus detail: the video/audio/input/RSP plugin
  tables, `CoreStartup`/`CoreShutdown`, ROM open/close, save states
  (`M64CMD_STATE_SAVE` / `_LOAD` run on the emulation thread), and the frame
  boundary handshake. The `EmuCore` adapter (`n64_core.cpp`) is a thin shim.
* The graphics plugin table wires `processDList` to a no-op and `processRdpList`
  to `n64video_process_list()`, matching the reference angrylion plugin.
* `screen_write()` copies angrylion's `0x00RRGGBB` output into the glue's
  capture buffer; `gfx_UpdateScreen()` publishes it. Until the VI is configured
  the glue synthesizes a test pattern so the frontend always gets a frame.
* cxd4 is a plugin: its `module.c` normally resolves the frontend's
  `Config*`/`CoreDoCommand` API through a dynamic-library handle at
  `PluginStartup()`. `n64_rsp.c` never calls `PluginStartup()`; it installs two
  minimal stubs and calls `InitiateRSP()`/`DoRspCycles()` directly. The pointer
  names are `-D`-renamed at compile time (`cores/n64/CMakeLists.txt`) so they do
  not clash with mupen64plus-core's public API of the same name.
* Built with the **cached interpreter** (`R4300Emulator = 1`), no dynarec, to
  keep the build free of nasm/`.obj`-in-static-lib issues. Classic dynarec can
  be re-enabled later for speed.

### The remaining cores

The remaining cores: `desmume/dolphin/azahar` are adapter stubs that
register a module and report `EMU_LOAD_ERROR_INTERNAL` from `load_rom`. They
compile only when their `BUILD_*_CORE` option is enabled and are templates for
finishing each console. `cores/mgba/` and `cores/n64/` are full adapters as
described above.

## Build graph

`emu_framework` is defined **before** any core, so each core can link it. Each
core's `CMakeLists.txt` compiles the upstream sources into a library and its
adapter into the module target, then appends its target to `EMU_CORE_TARGETS`
(`PARENT_SCOPE`). The frontend links `emu_framework` + `${EMU_CORE_TARGETS}` +
the platform libs.

```
emu_framework ──┐
               ├──► nintendall.exe
nes_core ──────┤
snes_core ─────┤
mgba_core ─────┤
n64_core ──────┘
```

Core discovery: each core's CMake prefers a vendored tree at
`cores/<core>/upstream/` and otherwise falls back to `<repo>/consoles/...`.
`tools/vendor-emulators.*` populates the vendored path.

The optional smoke test (`tests/`, `-DBUILD_SMOKE_TEST=ON`) links the exact
same libraries — `emu_framework` + `${EMU_CORE_TARGETS}` — but provides a
console `main` instead of a window, which is how the cores are exercised
end to end without a display.

## Adding a core — the short version

1. New `cores/<name>/` with an adapter `.cpp` and `CMakeLists.txt`
   (copy the Nestopia one).
2. Define `EmuCoreModule <name>_core_module` with the vtable filled in;
   `set_input_device` may be `nullptr`.
3. Add it to the table in `core_registry.cpp` under `EMU_ENABLE_<CORE>`.
4. Add a `BUILD_<X>_CORE` option and an `add_subdirectory` in the top-level
   `CMakeLists.txt`.

No frontend changes are required.
