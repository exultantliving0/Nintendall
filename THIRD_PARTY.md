# Third-party components

Nintendall links emulator cores that are separate projects with their own
copyright holders and licences. This file records what is actually included, so
that anyone receiving a binary can find the corresponding source — which the
copyleft licences in play here require us to offer.

Nintendall's own code is GPL-2.0-only; see [`LICENSE`](LICENSE).

## Console core status

Only the cores marked **working** emulate anything yet. The others are adapter
scaffolds: they compile and register with the console registry, so their console
is recognised by ROM detection, but they contain no emulation code and will not
run a game.

| Console | Project | Upstream revision | Licence | Status |
| --- | --- | --- | --- | --- |
| NES | Nestopia | *not pinned* | LGPL | **working** |
| SNES | bsnes | *not pinned* | GPL-2.0 | **working** |
| GB / GBC / GBA | mGBA | 0.11.0 | MPL-2.0 | **working** |
| N64 | Mupen64Plus-Core | 2.6.0 (July 2024) | GPL-2.0 | **working** |
| N64 | Mupen64Plus-RSP-CXD4 | `00906a9` (2026-06-23) | CC0 | **working** |
| N64 | Angrylion | *not pinned* | zlib | **working** (software RDP) |
| N64 | GLideN64 | *not pinned* | GPL-2.0 | **working** (fast OpenGL RDP) |
| N64 | Mupen64Plus-RSP-HLE | 2.6.0 (July 2024) | GPL-2.0 | **working** (fast RSP) |
| NDS | DeSmuME | — | GPL-2.0 | **stub, not implemented** |
| GameCube / Wii | Dolphin | — | GPL-2.0 | **stub, not implemented** |
| 3DS | Azahar | — | GPL-2.0 | **stub, not implemented** |

Bundled zlib (used by the N64 core) is version 1.3.1, zlib licence.

### What "stub" means in the code

`cores/desmume/nds_core.cpp`, `cores/dolphin/gcwii_core.cpp` and
`cores/azahar/n3ds_core.cpp` are ~118 lines each. Their `run_frame` returns 0,
their `save_state` returns 0, and each carries a `TODO` where the real core call
belongs. They build cleanly, so `BUILD_NDS_CORE=ON` is not evidence that DS
games run.

### Upstream sources

| Project | Repository |
| --- | --- |
| Nestopia | <https://github.com/0ldsk00l/nestopia> |
| bsnes | <https://github.com/higan/bsnes> |
| mGBA | <https://github.com/mGBA-emu/mgba> |
| Mupen64Plus-Core | <https://github.com/mupen64plus/mupen64plus-core> |
| Mupen64Plus-RSP-CXD4 | <https://github.com/mupen64plus/mupen64plus-rsp-cxd4> |
| Angrylion | <https://github.com/grpc/angrylion> (archived upstream; vendored tree in use) |
| GLideN64 | <https://github.com/gonetz/GLideN64> |
| Mupen64Plus-RSP-HLE | <https://github.com/mupen64plus/mupen64plus-rsp-hle> |
| DeSmuME | <https://github.com/nurpax/desmume> |
| Dolphin | <https://github.com/dolphin-emu/dolphin> |
| Azahar | <https://github.com/azahar-emu/azahar> |

### Source availability

The complete corresponding source for the working cores is in this repository
under `cores/<name>/upstream/`. To satisfy GPL §2(b) for binaries we publish, we
must also publish the build scripts that produce them; those are the
`CMakeLists.txt` files in this repository.

## Licences, verbatim

The full text of each licence is shipped in the vendored tree:

| Project | File |
| --- | --- |
| Nestopia | `cores/nestopia/upstream/license.txt` |
| bsnes | `cores/bsnes/upstream/License.txt` |
| mGBA | `cores/mgba/upstream/LICENSE` |
| Mupen64Plus-Core | `cores/n64/upstream/mupen64plus-core/doc/gpl-license` |
| Mupen64Plus-RSP-CXD4 | `cores/n64/upstream/mupen64plus-rsp-cxd4/COPYING` (CC0) |
| GLideN64 | `cores/n64/upstream/GLideN64/LICENSE` |
| Mupen64Plus-RSP-HLE | `cores/n64/upstream/mupen64plus-rsp-hle/LICENSES` |
| zlib | `cores/n64/upstream/zlib/LICENSE` |

## Build-time source lookups

`cores/desmume`, `cores/dolphin` and `cores/azahar` contain CMake code that
falls back to looking for their upstream trees in `../consoles/…` next to this
repository, e.g. `../../consoles/3ds/azahar-master`. That is a **build-time**
convenience for the development machine only. On a fresh clone, or in CI, those
directories do not exist and the corresponding core builds as a stub.

## Not included

- **No ROMs.** The user supplies their own game files.
- **No BIOS files.** No Nintendo firmware, key blobs or boot ROMs are
  redistributed. Systems that need one (N64, and partially DS) will not boot
  until the user supplies their own dump.