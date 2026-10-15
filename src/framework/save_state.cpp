/**
 * Nintendall - Unified Save State Container implementation
 */

#include "save_state.h"
#include "emu_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Encoding / decoding
// =============================================================================

uint8_t* emu_state_wrap(const void* payload, size_t payload_size,
                        EmuConsoleType console, const char* core_name,
                        const char* rom_title, uint32_t rom_crc32,
                        uint64_t frame_count, uint32_t flags, size_t* out_size) {
    if (out_size) *out_size = 0;
    if (!payload && payload_size) return NULL;

    size_t total = EMU_STATE_HEADER_SIZE + payload_size + 8;  // +2 crc32
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return NULL;
    memset(buf, 0, EMU_STATE_HEADER_SIZE);

    EmuStateHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, EMU_STATE_MAGIC, 4);
    hdr.version = EMU_STATE_VERSION;
    hdr.console = (uint16_t)console;
    hdr.flags = flags;
    if (core_name) {
        strncpy(hdr.core_name, core_name, sizeof(hdr.core_name) - 1);
    }
    if (rom_title) {
        strncpy(hdr.rom_title, rom_title, sizeof(hdr.rom_title) - 1);
    }
    hdr.rom_crc32 = rom_crc32;
    hdr.timestamp = emu_timestamp();
    hdr.frame_count = frame_count;
    hdr.payload_size = payload_size;

    // Serialize header fields manually (packed layout, little-endian assumed).
    memcpy(buf + 0,   hdr.magic, 4);
    uint16_t v = hdr.version;      memcpy(buf + 4,  &v, 2);
    uint16_t c = hdr.console;      memcpy(buf + 6,  &c, 2);
    uint32_t f = hdr.flags;        memcpy(buf + 8,  &f, 4);
    // header_crc32 computed over [0,12)
    uint32_t hcrc = emu_crc32(buf, 12);
    memcpy(buf + 12, &hcrc, 4);
    memcpy(buf + 16, hdr.core_name, 32);
    memcpy(buf + 48, hdr.rom_title, 64);
    memcpy(buf + 112, &hdr.rom_crc32, 4);
    memcpy(buf + 116, &hdr.timestamp, 4);
    memcpy(buf + 120, &hdr.frame_count, 8);
    memcpy(buf + 128, &hdr.payload_size, 8);

    if (payload_size && payload) {
        memcpy(buf + EMU_STATE_HEADER_SIZE, payload, payload_size);
    }

    uint32_t pcrc = emu_crc32(buf + EMU_STATE_HEADER_SIZE, payload_size);
    memcpy(buf + EMU_STATE_HEADER_SIZE + payload_size, &pcrc, 4);

    uint32_t ccrc = emu_crc32(buf, EMU_STATE_HEADER_SIZE + payload_size + 4);
    memcpy(buf + EMU_STATE_HEADER_SIZE + payload_size + 4, &ccrc, 4);

    if (out_size) *out_size = total;
    return buf;
}

const uint8_t* emu_state_unwrap(const uint8_t* buffer, size_t size,
                                EmuStateHeader* header, size_t* payload_size) {
    if (payload_size) *payload_size = 0;
    if (!buffer || size < EMU_STATE_HEADER_SIZE + 8) return NULL;
    if (memcmp(buffer, EMU_STATE_MAGIC, 4) != 0) return NULL;

    EmuStateHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, buffer + 0, 4);
    memcpy(&hdr.version, buffer + 4, 2);
    memcpy(&hdr.console, buffer + 6, 2);
    memcpy(&hdr.flags, buffer + 8, 4);
    memcpy(&hdr.header_crc32, buffer + 12, 4);
    memcpy(hdr.core_name, buffer + 16, 32);
    memcpy(hdr.rom_title, buffer + 48, 64);
    memcpy(&hdr.rom_crc32, buffer + 112, 4);
    memcpy(&hdr.timestamp, buffer + 116, 4);
    memcpy(&hdr.frame_count, buffer + 120, 8);
    memcpy(&hdr.payload_size, buffer + 128, 8);

    if (hdr.version > EMU_STATE_VERSION) return NULL;
    if (hdr.header_crc32 != emu_crc32(buffer, 12)) return NULL;

    size_t total = EMU_STATE_HEADER_SIZE + (size_t)hdr.payload_size + 8;
    if (total > size) return NULL;

    uint32_t pcrc_stored, ccrc_stored;
    memcpy(&pcrc_stored, buffer + EMU_STATE_HEADER_SIZE + hdr.payload_size, 4);
    memcpy(&ccrc_stored, buffer + EMU_STATE_HEADER_SIZE + hdr.payload_size + 4, 4);

    uint32_t pcrc = emu_crc32(buffer + EMU_STATE_HEADER_SIZE, (size_t)hdr.payload_size);
    if (pcrc != pcrc_stored) return NULL;

    uint32_t ccrc = emu_crc32(buffer, EMU_STATE_HEADER_SIZE + (size_t)hdr.payload_size + 4);
    if (ccrc != ccrc_stored) return NULL;

    if (header) *header = hdr;
    if (payload_size) *payload_size = (size_t)hdr.payload_size;
    return buffer + EMU_STATE_HEADER_SIZE;
}

int emu_state_validate(const uint8_t* buffer, size_t size) {
    return emu_state_unwrap(buffer, size, NULL, NULL) ? 0 : -1;
}

// =============================================================================
// File helpers
// =============================================================================

int emu_state_write_file(const char* path, const void* payload, size_t payload_size,
                         EmuConsoleType console, const char* core_name,
                         const char* rom_title, uint32_t rom_crc32,
                         uint64_t frame_count, uint32_t flags) {
    size_t total = 0;
    uint8_t* buf = emu_state_wrap(payload, payload_size, console, core_name,
                                  rom_title, rom_crc32, frame_count, flags, &total);
    if (!buf) return -1;
    int rc = emu_write_file(path, buf, total);
    free(buf);
    return rc;
}

uint8_t* emu_state_read_file(const char* path, EmuStateHeader* header,
                             size_t* payload_size) {
    size_t size = 0;
    uint8_t* file = emu_read_file(path, &size, 0);
    if (!file) return NULL;

    const uint8_t* payload = emu_state_unwrap(file, size, header, payload_size);
    if (!payload) {
        free(file);
        return NULL;
    }

    uint8_t* out = (uint8_t*)malloc(*payload_size ? *payload_size : 1);
    if (!out) { free(file); return NULL; }
    if (*payload_size) memcpy(out, payload, *payload_size);
    free(file);
    return out;
}

// =============================================================================
// Save state manager
// =============================================================================

struct EmuStateManager {
    char base_dir[512];
    char game_dir[768];
    uint32_t rom_crc32;
    EmuConsoleType console;
};

EmuStateManager* emu_state_mgr_create(const char* base_dir) {
    EmuStateManager* mgr = (EmuStateManager*)calloc(1, sizeof(EmuStateManager));
    if (!mgr) return NULL;
    if (base_dir) {
        strncpy(mgr->base_dir, base_dir, sizeof(mgr->base_dir) - 1);
    }
    emu_mkdir_p(mgr->base_dir);
    return mgr;
}

void emu_state_mgr_destroy(EmuStateManager* mgr) {
    free(mgr);
}

void emu_state_mgr_set_game(EmuStateManager* mgr, const char* rom_path,
                            uint32_t rom_crc32, EmuConsoleType console) {
    if (!mgr) return;
    mgr->rom_crc32 = rom_crc32;
    mgr->console = console;

    char base[520];
    emu_get_basename(rom_path ? rom_path : "game", base, sizeof(base));
    emu_strip_extension(base, base, sizeof(base));

    // Sanitize basename for filesystem
    for (char* p = base; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            *p = '_';
        }
    }

    snprintf(mgr->game_dir, sizeof(mgr->game_dir), "%s/%s.%08X",
             mgr->base_dir, base, rom_crc32);
    emu_mkdir_p(mgr->game_dir);
}

int emu_state_mgr_slot_path(EmuStateManager* mgr, int slot, char* out, size_t out_size) {
    if (!mgr) return -1;
    int n = snprintf(out, out_size, "%s/slot%d.uestate", mgr->game_dir, slot);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

int emu_state_mgr_save(EmuStateManager* mgr, int slot,
                       const void* payload, size_t payload_size,
                       const char* core_name, const char* rom_title,
                       uint64_t frame_count, uint32_t flags) {
    if (!mgr) return -1;
    char path[1024];
    if (emu_state_mgr_slot_path(mgr, slot, path, sizeof(path)) != 0) return -1;
    return emu_state_write_file(path, payload, payload_size, mgr->console,
                                core_name, rom_title, mgr->rom_crc32,
                                frame_count, flags);
}

uint8_t* emu_state_mgr_load(EmuStateManager* mgr, int slot,
                            EmuStateHeader* header, size_t* payload_size) {
    if (!mgr) return NULL;
    char path[1024];
    if (emu_state_mgr_slot_path(mgr, slot, path, sizeof(path)) != 0) return NULL;

    EmuStateHeader hdr;
    uint8_t* payload = emu_state_read_file(path, &hdr, payload_size);
    if (!payload) return NULL;

    if (header) *header = hdr;
    return payload;
}

int emu_state_mgr_meta(EmuStateManager* mgr, int slot, EmuStateHeader* header) {
    if (!mgr || !header) return -1;
    char path[1024];
    if (emu_state_mgr_slot_path(mgr, slot, path, sizeof(path)) != 0) return -1;

    size_t size = 0;
    uint8_t* file = emu_read_file(path, &size, 0);
    if (!file) return -1;
    int rc = emu_state_unwrap(file, size, header, NULL) ? 0 : -1;
    free(file);
    return rc;
}

int emu_state_mgr_delete(EmuStateManager* mgr, int slot) {
    if (!mgr) return -1;
    char path[1024];
    if (emu_state_mgr_slot_path(mgr, slot, path, sizeof(path)) != 0) return -1;
    return remove(path);
}

// =============================================================================
// Rewind ring buffer
// =============================================================================

typedef struct RewindEntry {
    uint8_t* data;
    size_t size;
    uint64_t frame;
} RewindEntry;

struct EmuRewindBuffer {
    RewindEntry* entries;
    size_t capacity;
    size_t count;
    size_t head;              // next write index
    size_t total_bytes;
    size_t max_state_size;
    size_t total_budget;
};

EmuRewindBuffer* emu_rewind_create(size_t max_states, size_t max_state_size,
                                   size_t total_budget_bytes) {
    if (max_states == 0 || max_state_size == 0) return NULL;
    EmuRewindBuffer* rb = (EmuRewindBuffer*)calloc(1, sizeof(EmuRewindBuffer));
    if (!rb) return NULL;
    rb->entries = (RewindEntry*)calloc(max_states, sizeof(RewindEntry));
    if (!rb->entries) { free(rb); return NULL; }
    rb->capacity = max_states;
    rb->max_state_size = max_state_size;
    rb->total_budget = total_budget_bytes ? total_budget_bytes : (max_states * max_state_size);
    return rb;
}

void emu_rewind_destroy(EmuRewindBuffer* rb) {
    if (!rb) return;
    for (size_t i = 0; i < rb->capacity; i++) free(rb->entries[i].data);
    free(rb->entries);
    free(rb);
}

static void rewind_evict_entry(EmuRewindBuffer* rb, size_t idx) {
    if (rb->entries[idx].data) {
        rb->total_bytes -= rb->entries[idx].size;
        free(rb->entries[idx].data);
        rb->entries[idx].data = NULL;
        rb->entries[idx].size = 0;
    }
}

int emu_rewind_push(EmuRewindBuffer* rb, const void* data, size_t size,
                    uint64_t frame_count) {
    if (!rb || !data || size == 0 || size > rb->max_state_size) return -1;

    // Evict oldest entries until within budget
    while (rb->total_bytes + size > rb->total_budget && rb->count > 0) {
        // oldest is at (head - count + capacity) % capacity
        size_t oldest = (rb->head + rb->capacity - rb->count) % rb->capacity;
        rewind_evict_entry(rb, oldest);
        rb->count--;
    }

    size_t idx = rb->head;
    rewind_evict_entry(rb, idx);
    rb->entries[idx].data = (uint8_t*)malloc(size);
    if (!rb->entries[idx].data) return -1;
    memcpy(rb->entries[idx].data, data, size);
    rb->entries[idx].size = size;
    rb->entries[idx].frame = frame_count;
    rb->total_bytes += size;

    rb->head = (rb->head + 1) % rb->capacity;
    if (rb->count < rb->capacity) rb->count++;
    return 0;
}

size_t emu_rewind_pop(EmuRewindBuffer* rb, void* out, size_t out_size,
                      uint64_t* frame_count) {
    if (!rb || rb->count == 0) return 0;
    size_t idx = (rb->head + rb->capacity - 1) % rb->capacity;
    RewindEntry* e = &rb->entries[idx];
    if (!e->data || e->size > out_size) return 0;
    memcpy(out, e->data, e->size);
    if (frame_count) *frame_count = e->frame;
    size_t n = e->size;
    rewind_evict_entry(rb, idx);
    rb->count--;
    rb->head = idx;
    return n;
}

int emu_rewind_count(const EmuRewindBuffer* rb) {
    return rb ? (int)rb->count : 0;
}

void emu_rewind_clear(EmuRewindBuffer* rb) {
    if (!rb) return;
    for (size_t i = 0; i < rb->capacity; i++) rewind_evict_entry(rb, i);
    rb->count = 0;
    rb->head = 0;
    rb->total_bytes = 0;
}