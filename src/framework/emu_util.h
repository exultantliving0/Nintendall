/**
 * Nintendall - Shared internal utilities
 */

#ifndef EMU_UTIL_H
#define EMU_UTIL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// CRC32 (IEEE 802.3, poly 0xEDB88320) - used for ROM identification
uint32_t emu_crc32(const uint8_t* data, size_t len);

// Current Unix timestamp
uint32_t emu_timestamp(void);

// Case-insensitive string compare (portable)
int emu_strcasecmp(const char* a, const char* b);

// Case-insensitive extension check: does path end with ".ext"?
int emu_has_extension(const char* path, const char* ext);

// Get lowercase file extension (without dot) into out buffer
void emu_get_extension(const char* path, char* out, size_t out_size);

// Get base filename (without directory) into out buffer
void emu_get_basename(const char* path, char* out, size_t out_size);

// Strip extension from filename into out buffer
void emu_strip_extension(const char* path, char* out, size_t out_size);

// Create directory (and parents) if it does not exist. Returns 0 on success.
int emu_mkdir_p(const char* path);

// Read entire file into a newly allocated buffer. Caller frees with free().
// max_size = 0 means no limit.
uint8_t* emu_read_file(const char* path, size_t* out_size, size_t max_size);

// Write buffer to file. Returns 0 on success.
int emu_write_file(const char* path, const void* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // EMU_UTIL_H