/**
 * Nintendall - ZIP archive support
 *
 * ROMs are very often distributed as .zip archives. This module lets the
 * framework (and therefore the frontend, detection and the smoke test) treat a
 * .zip exactly like a ROM: the archive is inspected, the entry most likely to
 * be a ROM is extracted, and a normal file path is handed back.
 *
 * It is fully self-contained: the DEFLATE decoder is built in, so there is no
 * zlib or miniz dependency and it works in every build configuration
 * (including a NES-only build with no other cores present).
 */

#ifndef EMU_ZIP_H
#define EMU_ZIP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Low-level archive access
// -----------------------------------------------------------------------------

// Does this buffer start with a ZIP signature?
int emu_zip_is_zip(const uint8_t* data, size_t size);

typedef struct EmuZip EmuZip;

// Parse an archive from memory. The caller must keep `data` alive for the
// lifetime of the handle. Returns NULL if it is not a usable archive.
EmuZip* emu_zip_open_memory(const uint8_t* data, size_t size);
void    emu_zip_close(EmuZip* zip);

int         emu_zip_count(const EmuZip* zip);
const char* emu_zip_name(const EmuZip* zip, int index);
uint32_t    emu_zip_size(const EmuZip* zip, int index);   // uncompressed size
int         emu_zip_is_dir(const EmuZip* zip, int index);

// Extract one entry. Returns a newly allocated buffer (free with free()),
// or NULL on error. *out_size receives the uncompressed size.
uint8_t* emu_zip_read(const EmuZip* zip, int index, size_t* out_size);

// Index of the entry most likely to be a ROM, or -1 if the archive is empty.
int emu_zip_find_rom(const EmuZip* zip);

// -----------------------------------------------------------------------------
// High-level helpers
// -----------------------------------------------------------------------------

// Pick the best ROM entry inside `zipdata`, extract it, and (if that entry is
// itself an archive) descend into it a couple of levels.
// On success returns 1 with *out_data (malloc'd), *out_size and *out_name
// (the entry's file name) filled in. Returns 0 on failure.
int emu_zip_extract_rom(const uint8_t* zipdata, size_t zipsize,
                        uint8_t** out_data, size_t* out_size,
                        char* out_name, size_t out_name_size);

// Prepare a user-selected path for loading:
//   * if `path` is a ZIP archive, the best ROM entry is extracted into
//     `cache_dir` (as "<crc32>_<entryname>") and that path is returned;
//   * otherwise `path` is returned unchanged.
// `out_display_name` receives the inner entry's name for archives, or the
// file's base name otherwise. Returns a malloc'd string (free with free()),
// or NULL on invalid input.
char* emu_prepare_rom(const char* path, const char* cache_dir,
                      char* out_display_name, size_t out_display_name_size);

#ifdef __cplusplus
}
#endif

#endif // EMU_ZIP_H
