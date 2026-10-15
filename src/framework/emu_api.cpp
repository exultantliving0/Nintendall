/**
 * Nintendall - Common API Implementation
 */

#include "emu_api.h"
#include "emu_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#define EMU_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define EMU_MKDIR(p) mkdir((p), 0755)
#endif

// =============================================================================
// Default Configuration
// =============================================================================

extern "C" void emu_config_default(EmuConfig* config) {
    if (!config) return;
    memset(config, 0, sizeof(EmuConfig));

    config->video_width = 1280;
    config->video_height = 720;
    config->fullscreen = 0;
    config->vsync = 1;
    config->scale_factor = 0;        // 0 = auto
    config->aspect_correction = 1;

    config->audio_sample_rate = 48000;
    config->audio_buffer_size = 2048;
    config->audio_volume = 100;

    config->region = EMU_REGION_NTSC;
    config->fast_forward = 1;
    config->rewind_enable = 1;

    config->save_dir = "./saves";
    config->shader_dir = "./shaders";
    config->bios_dir = "./bios";
}

// =============================================================================
// Load Result Strings
// =============================================================================

extern "C" const char* emu_load_result_string(EmuLoadResult result) {
    switch (result) {
        case EMU_LOAD_OK: return "Success";
        case EMU_LOAD_ERROR_INVALID_ROM: return "Invalid or corrupted ROM";
        case EMU_LOAD_ERROR_UNSUPPORTED_MAPPER: return "Unsupported mapper/chip";
        case EMU_LOAD_ERROR_BIOS_MISSING: return "Required BIOS file missing";
        case EMU_LOAD_ERROR_OUT_OF_MEMORY: return "Out of memory";
        case EMU_LOAD_ERROR_INTERNAL: return "Internal error";
        default: return "Unknown error";
    }
}

// =============================================================================
// CRC32
// =============================================================================

extern "C" uint32_t emu_crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// =============================================================================
// Timestamp
// =============================================================================

extern "C" uint32_t emu_timestamp(void) {
    return (uint32_t)time(NULL);
}

// =============================================================================
// String helpers
// =============================================================================

extern "C" int emu_strcasecmp(const char* a, const char* b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

extern "C" int emu_has_extension(const char* path, const char* ext) {
    if (!path || !ext) return 0;
    const char* dot = strrchr(path, '.');
    if (!dot) return 0;
    return emu_strcasecmp(dot + 1, ext) == 0;
}

extern "C" void emu_get_extension(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    out[0] = '\0';
    const char* dot = strrchr(path, '.');
    if (!dot) return;
    const char* p = dot + 1;
    size_t i = 0;
    while (p[i] && i < out_size - 1) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        out[i] = c;
        i++;
    }
    out[i] = '\0';
}

extern "C" void emu_get_basename(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    const char* slash1 = strrchr(path, '/');
    const char* slash2 = strrchr(path, '\\');
    const char* base = path;
    if (slash1) base = slash1 + 1;
    if (slash2 && slash2 + 1 > base) base = slash2 + 1;
    size_t i = 0;
    while (base[i] && i < out_size - 1) { out[i] = base[i]; i++; }
    out[i] = '\0';
}

extern "C" void emu_strip_extension(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    size_t i = 0;
    while (path[i] && i < out_size - 1) { out[i] = path[i]; i++; }
    out[i] = '\0';
    char* dot = strrchr(out, '.');
    char* slash1 = strrchr(out, '/');
    char* slash2 = strrchr(out, '\\');
    if (dot && (!slash1 || dot > slash1) && (!slash2 || dot > slash2)) {
        *dot = '\0';
    }
}

// =============================================================================
// Filesystem helpers
// =============================================================================

extern "C" int emu_mkdir_p(const char* path) {
    if (!path || !*path) return -1;
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
            EMU_MKDIR(tmp);
            *p = save;
        }
    }
    return EMU_MKDIR(tmp);
}

extern "C" uint8_t* emu_read_file(const char* path, size_t* out_size, size_t max_size) {
    if (out_size) *out_size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || (max_size > 0 && (size_t)size > max_size)) {
        fclose(f);
        return NULL;
    }

    uint8_t* data = (uint8_t*)malloc((size_t)size);
    if (!data) { fclose(f); return NULL; }

    size_t rd = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (rd != (size_t)size) { free(data); return NULL; }

    if (out_size) *out_size = (size_t)size;
    return data;
}

extern "C" int emu_write_file(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t wr = fwrite(data, 1, size, f);
    fclose(f);
    return (wr == size) ? 0 : -1;
}