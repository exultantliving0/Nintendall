/**
 * Nintendall - Unified Save State Container
 *
 * Every core stores its save state as an opaque payload. This module wraps that
 * payload in a common container so the frontend can:
 *   - list/manage save states uniformly for every console
 *   - verify the state belongs to the currently loaded ROM
 *   - keep slot history / thumbnails / timestamps
 *
 * Container layout (little-endian):
 *
 *   offset  size  field
 *   ------  ----  --------------------------------------------------
 *   0       4     magic "UESV"
 *   4       2     format version (1)
 *   6       2     console type
 *   8       4     flags
 *   12      4     header crc32 (of bytes [0,12))
 *   16      32    core name (null-terminated)
 *   48      64    rom title (null-terminated)
 *   112     4     rom crc32
 *   116     4     unix timestamp
 *   120     8     frame count
 *   128     8     payload size
 *   136     N     payload (core-specific)
 *   136+N   4     payload crc32
 *   140+N   4     container crc32 (of bytes [0,140+N))
 */

#ifndef SAVE_STATE_H
#define SAVE_STATE_H

#include "emu_api.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU_STATE_MAGIC       "UESV"
#define EMU_STATE_VERSION     1
#define EMU_STATE_HEADER_SIZE 136

#pragma pack(push, 1)
typedef struct EmuStateHeader {
    char     magic[4];        // "UESV"
    uint16_t version;         // EMU_STATE_VERSION
    uint16_t console;         // EmuConsoleType
    uint32_t flags;           // reserved
    uint32_t header_crc32;    // crc of bytes [0,12)
    char     core_name[32];   // producing core
    char     rom_title[64];   // game title
    uint32_t rom_crc32;       // rom identity
    uint32_t timestamp;       // unix time
    uint64_t frame_count;     // frame at which state was captured
    uint64_t payload_size;    // size of core-specific payload
} EmuStateHeader;
#pragma pack(pop)

typedef enum EmuStateFlags {
    EMU_STATE_FLAG_NONE     = 0,
    EMU_STATE_FLAG_AUTO     = 1 << 0,  // auto-save / rewind buffer
    EMU_STATE_FLAG_QUICKSAVE = 1 << 1,
} EmuStateFlags;

// =============================================================================
// Encode / decode
// =============================================================================

// Wrap a raw core payload into a UESV container.
// Returns a newly allocated buffer (free with free()), or NULL on error.
uint8_t* emu_state_wrap(const void* payload, size_t payload_size,
                        EmuConsoleType console, const char* core_name,
                        const char* rom_title, uint32_t rom_crc32,
                        uint64_t frame_count, uint32_t flags, size_t* out_size);

// Parse a UESV container. On success fills *header (if non-NULL) and returns a
// pointer to the payload *inside* the input buffer, setting *payload_size.
// Returns NULL if the container is invalid.
const uint8_t* emu_state_unwrap(const uint8_t* buffer, size_t size,
                                EmuStateHeader* header, size_t* payload_size);

// Validate a container (magic, CRCs, version). Returns 0 if valid.
int emu_state_validate(const uint8_t* buffer, size_t size);

// =============================================================================
// File helpers
// =============================================================================

int emu_state_write_file(const char* path, const void* payload, size_t payload_size,
                         EmuConsoleType console, const char* core_name,
                         const char* rom_title, uint32_t rom_crc32,
                         uint64_t frame_count, uint32_t flags);

// Read a container file and return the raw payload (newly allocated).
// Fills *header if non-NULL. Caller frees with free().
uint8_t* emu_state_read_file(const char* path, EmuStateHeader* header,
                             size_t* payload_size);

// =============================================================================
// Save state manager (slot based)
// =============================================================================

typedef struct EmuStateManager EmuStateManager;

// State file path layout: <dir>/<rom_basename>.<console>/slot<N>.uestate
EmuStateManager* emu_state_mgr_create(const char* base_dir);
void emu_state_mgr_destroy(EmuStateManager* mgr);

// Configure the current game (basename + crc used for identity checks)
void emu_state_mgr_set_game(EmuStateManager* mgr, const char* rom_path,
                            uint32_t rom_crc32, EmuConsoleType console);

// Build the path for a slot. Returns 0 on success.
int emu_state_mgr_slot_path(EmuStateManager* mgr, int slot, char* out, size_t out_size);

// Save/load a slot (payload is the raw core state; wrapped automatically).
int emu_state_mgr_save(EmuStateManager* mgr, int slot,
                       const void* payload, size_t payload_size,
                       const char* core_name, const char* rom_title,
                       uint64_t frame_count, uint32_t flags);

// Returns newly allocated raw payload for the slot, or NULL if missing.
uint8_t* emu_state_mgr_load(EmuStateManager* mgr, int slot,
                            EmuStateHeader* header, size_t* payload_size);

// Query a slot's metadata without loading the payload.
// Returns 0 on success, -1 if the slot does not exist.
int emu_state_mgr_meta(EmuStateManager* mgr, int slot, EmuStateHeader* header);

// Delete a slot file. Returns 0 on success.
int emu_state_mgr_delete(EmuStateManager* mgr, int slot);

// =============================================================================
// Serialized-state black box for rewind
// =============================================================================

typedef struct EmuRewindBuffer EmuRewindBuffer;

// Create a ring buffer that keeps up to `max_states` serialized states, each
// at most `max_state_size` bytes. `total_budget_bytes` caps memory usage.
EmuRewindBuffer* emu_rewind_create(size_t max_states, size_t max_state_size,
                                   size_t total_budget_bytes);
void emu_rewind_destroy(EmuRewindBuffer* rb);

// Push a serialized state snapshot.
int emu_rewind_push(EmuRewindBuffer* rb, const void* data, size_t size,
                    uint64_t frame_count);

// Pop the most recent snapshot into `out` (returns bytes copied, or 0 if empty).
size_t emu_rewind_pop(EmuRewindBuffer* rb, void* out, size_t out_size,
                      uint64_t* frame_count);

int emu_rewind_count(const EmuRewindBuffer* rb);
void emu_rewind_clear(EmuRewindBuffer* rb);

#ifdef __cplusplus
}
#endif

#endif // SAVE_STATE_H