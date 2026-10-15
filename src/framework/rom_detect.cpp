/**
 * Nintendall - ROM Detection Implementation
 *
 * Detects console type by examining ROM headers and structure.
 * This powers the "Load ROM -> auto-detect console" feature.
 */

#include "rom_detect.h"
#include "emu_util.h"
#include "zip.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// =============================================================================
// Byte readers
// =============================================================================

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint32_t read_le32(const uint8_t* p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8)  | (uint32_t)p[0];
}

// =============================================================================
// NES / Famicom Disk System / NSF
// =============================================================================

int emu_detect_nes(const uint8_t* data, size_t size, EmuDetectResult* result) {
    if (size < 16) return 0;

    if (data[0] == 'N' && data[1] == 'E' && data[2] == 'S' && data[3] == 0x1A) {
        int nes2 = ((data[7] >> 2) & 0x03) == 0x02;
        int mapper = (data[6] >> 4) | (data[7] & 0xF0);
        if (nes2) mapper |= ((data[8] & 0x0F) << 8);

        result->console = EMU_CONSOLE_NES;
        result->confidence = 100;
        snprintf(result->details, sizeof(result->details),
                 "%s | mapper %d | %d PRG banks | %d CHR banks",
                 nes2 ? "NES 2.0" : "iNES", mapper, data[4], data[5]);
        return 1;
    }

    if (memcmp(data, "UNIF", 4) == 0) {
        result->console = EMU_CONSOLE_NES;
        result->confidence = 95;
        snprintf(result->details, sizeof(result->details), "UNIF format NES ROM");
        return 1;
    }

    if (data[0] == 'F' && data[1] == 'D' && data[2] == 'S' && data[3] == 0x1A) {
        result->console = EMU_CONSOLE_NES;
        result->confidence = 95;
        snprintf(result->details, sizeof(result->details),
                 "Famicom Disk System | %d sides", data[4]);
        return 1;
    }

    if (memcmp(data, "NESM", 4) == 0) {
        result->console = EMU_CONSOLE_NES;
        result->confidence = 90;
        snprintf(result->details, sizeof(result->details), "NES Sound File (NSF)");
        return 1;
    }

    return 0;
}

// =============================================================================
// SNES / Super Famicom
// =============================================================================

static int snes_valid_header(const uint8_t* d, size_t size, size_t offset,
                             EmuDetectResult* result, const char* map) {
    if (size < offset + 64) return 0;

    int all_zero = 1, all_ff = 1;
    for (int i = 0; i < 32; i++) {
        if (d[offset + i] != 0x00) all_zero = 0;
        if (d[offset + i] != 0xFF) all_ff = 0;
    }
    if (all_zero || all_ff) return 0;

    uint8_t rom_type = d[offset + 0x16];
    uint8_t rom_size = d[offset + 0x17];
    if (rom_size > 0x0D) return 0;

    const char* coprocessor = "none";
    switch (rom_type) {
        case 0x00: coprocessor = "none"; break;
        case 0x02: coprocessor = "DSP1"; break;
        case 0x03: coprocessor = "DSP1 + SRAM"; break;
        case 0x04: coprocessor = "DSP2/3"; break;
        case 0x05: coprocessor = "DSP2/3 + SRAM"; break;
        case 0x13: coprocessor = "SuperFX"; break;
        case 0x14: coprocessor = "SuperFX + SRAM"; break;
        case 0x15: coprocessor = "SuperFX 2"; break;
        case 0x1A: coprocessor = "S-DD1 + SRAM"; break;
        case 0x22: coprocessor = "SA-1"; break;
        case 0x23: coprocessor = "SA-1 + SRAM"; break;
        case 0x25: coprocessor = "OBC-1"; break;
        case 0x32: coprocessor = "S-DD1"; break;
        case 0x34: coprocessor = "Cx4"; break;
        case 0x35: coprocessor = "Cx4 + SRAM"; break;
        case 0x43: coprocessor = "ST010"; break;
        case 0x45: coprocessor = "ST010"; break;
        default: coprocessor = "unknown"; break;
    }

    const char* region = "unknown";
    switch (d[offset + 0x1A]) {
        case 0x00: region = "Japan"; break;
        case 0x01: region = "USA"; break;
        case 0x02: region = "Europe"; break;
        case 0x03: region = "Sweden"; break;
        case 0x06: region = "France"; break;
        case 0x07: region = "Denmark"; break;
        case 0x09: region = "Germany"; break;
        case 0x0B: region = "Italy"; break;
        case 0x0C: region = "China"; break;
        case 0x0E: region = "Korea"; break;
        case 0xFF: region = "International"; break;
        default: break;
    }

    char title[22];
    memcpy(title, d + offset, 21);
    title[21] = '\0';
    for (int i = 0; i < 21; i++)
        if (title[i] < 0x20 || title[i] > 0x7E) title[i] = ' ';

    result->console = EMU_CONSOLE_SNES;
    result->confidence = 90;
    snprintf(result->details, sizeof(result->details),
             "%s | %s | %s | coprocessor: %s | %dKB",
             map, title, region, coprocessor, 1 << rom_size);
    return 1;
}

int emu_detect_snes(const uint8_t* data, size_t size, EmuDetectResult* result) {
    const uint8_t* d = data;

    // 512-byte SMC copier header
    if ((size % 1024) == 512 && size > 512) {
        d = data + 512;
        size -= 512;
    }

    if (snes_valid_header(d, size, 0x7FC0, result, "LoROM")) return 1;
    if (snes_valid_header(d, size, 0xFFC0, result, "HiROM")) return 1;
    if (size > 0x40FFC0 && snes_valid_header(d, size, 0x40FFC0, result, "ExHiROM")) return 1;
    if (size > 0x80FFC0 && snes_valid_header(d, size, 0x80FFC0, result, "ExHiROM")) return 1;

    return 0;
}

// =============================================================================
// Game Boy / Game Boy Color
// =============================================================================

static const uint8_t nintendo_logo[48] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
    0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
    0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
};

int emu_detect_gb_gbc(const uint8_t* data, size_t size, EmuDetectResult* result) {
    if (size < 0x150) return 0;
    if (memcmp(data + 0x0104, nintendo_logo, 48) != 0) return 0;

    uint8_t cgb_flag = data[0x0143];
    uint8_t type = data[0x0147];

    const char* mbc = "unknown";
    switch (type) {
        case 0x00: mbc = "ROM ONLY"; break;
        case 0x01: case 0x02: case 0x03: mbc = "MBC1"; break;
        case 0x05: case 0x06: mbc = "MBC2"; break;
        case 0x08: case 0x09: mbc = "ROM+RAM"; break;
        case 0x0B: case 0x0C: case 0x0D: mbc = "MMM01"; break;
        case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: mbc = "MBC3"; break;
        case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: mbc = "MBC5"; break;
        case 0x20: mbc = "MBC6"; break;
        case 0x22: mbc = "MBC7"; break;
        case 0xFC: mbc = "POCKET CAMERA"; break;
        case 0xFD: mbc = "BANDAI TAMA5"; break;
        case 0xFE: mbc = "HuC3"; break;
        case 0xFF: mbc = "HuC1"; break;
        default: break;
    }

    char title[17];
    memcpy(title, data + 0x0134, 16);
    title[16] = '\0';
    for (int i = 0; i < 16; i++)
        if (title[i] < 0x20 || title[i] > 0x7E) title[i] = ' ';

    if (cgb_flag == 0x80 || cgb_flag == 0xC0) {
        result->console = EMU_CONSOLE_GAMEBOY_COLOR;
        result->confidence = 100;
        snprintf(result->details, sizeof(result->details),
                 "Game Boy Color | %s | MBC: %s", title, mbc);
    } else {
        result->console = EMU_CONSOLE_GAMEBOY;
        result->confidence = 100;
        snprintf(result->details, sizeof(result->details),
                 "Game Boy | %s | MBC: %s", title, mbc);
    }
    return 1;
}

// =============================================================================
// Game Boy Advance
// =============================================================================

int emu_detect_gba(const uint8_t* data, size_t size, EmuDetectResult* result) {
    if (size < 0xC0) return 0;
    if (data[0xB2] != 0x96) return 0;

    char title[13];
    memcpy(title, data + 0xA0, 12);
    title[12] = '\0';
    for (int i = 0; i < 12; i++)
        if (title[i] < 0x20 || title[i] > 0x7E) title[i] = ' ';

    char gamecode[5];
    memcpy(gamecode, data + 0xAC, 4);
    gamecode[4] = '\0';

    char makercode[3];
    memcpy(makercode, data + 0xB0, 2);
    makercode[2] = '\0';

    result->console = EMU_CONSOLE_GAMEBOY_ADVANCE;
    result->confidence = 100;
    snprintf(result->details, sizeof(result->details),
             "GBA | %s [%s] by %s", title, gamecode, makercode);
    return 1;
}

// =============================================================================
// Nintendo DS
// =============================================================================

int emu_detect_nds(const uint8_t* data, size_t size, EmuDetectResult* result) {
    if (size < 0x200) return 0;

    char gamecode[5];
    memcpy(gamecode, data + 0x0C, 4);
    gamecode[4] = '\0';

    char makercode[3];
    memcpy(makercode, data + 0x10, 2);
    makercode[2] = '\0';

    char title[13];
    for (int i = 0; i < 12; i++) {
        uint8_t c = data[i * 2];
        title[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
    }
    title[12] = '\0';

    for (int i = 0; i < 4; i++)
        if (!isalnum((unsigned char)gamecode[i])) return 0;
    for (int i = 0; i < 2; i++)
        if (!isalnum((unsigned char)makercode[i])) return 0;

    uint32_t rom_size = read_le32(data + 0x80);

    result->console = EMU_CONSOLE_NINTENDO_DS;
    result->confidence = 90;
    snprintf(result->details, sizeof(result->details),
             "Nintendo DS | %s [%s] by %s | %.1fMB",
             title, gamecode, makercode, rom_size / (1024.0 * 1024.0));
    return 1;
}

// =============================================================================
// Nintendo 64
// =============================================================================

int emu_detect_n64(const uint8_t* data, size_t size, EmuDetectResult* result) {
    if (size < 0x40) return 0;

    const char* endian = NULL;
    if (data[0] == 0x80 && data[1] == 0x37 && data[2] == 0x12 && data[3] == 0x40)
        endian = "big-endian (z64)";
    else if (data[0] == 0x37 && data[1] == 0x80 && data[2] == 0x40 && data[3] == 0x12)
        endian = "byte-swapped (v64)";
    else if (data[0] == 0x40 && data[1] == 0x12 && data[2] == 0x37 && data[3] == 0x80)
        endian = "little-endian (n64)";
    if (!endian) return 0;

    char title[21];
    if (data[0] == 0x80) {
        memcpy(title, data + 0x20, 20);
    } else {
        for (int i = 0; i < 20; i += 2) {
            title[i]     = data[0x20 + i + 1];
            title[i + 1] = data[0x20 + i];
        }
    }
    title[20] = '\0';
    for (int i = 0; i < 20; i++)
        if (title[i] < 0x20 || title[i] > 0x7E) title[i] = ' ';

    result->console = EMU_CONSOLE_NINTENDO_64;
    result->confidence = 100;
    snprintf(result->details, sizeof(result->details),
             "Nintendo 64 | %s | %s", title, endian);
    return 1;
}

// =============================================================================
// GameCube / Wii
// =============================================================================

int emu_detect_gc_wii(const uint8_t* data, size_t size, EmuDetectResult* result) {
    if (size < 0x100) return 0;

    uint32_t magic = read_be32(data + 0x1C);
    int is_gc  = (magic == 0xC2339F3D);
    int is_wii = (magic == 0x5D1C9EA3);
    if (!is_gc && !is_wii) return 0;

    char name[7];
    memcpy(name, data, 6);
    name[6] = '\0';

    char maker[3];
    memcpy(maker, data + 6, 2);
    maker[2] = '\0';

    const char* region_str = "Unknown";
    switch (data[3]) {
        case 'E': region_str = "USA"; break;
        case 'J': region_str = "Japan"; break;
        case 'P': region_str = "Europe"; break;
        case 'D': region_str = "Germany"; break;
        case 'F': region_str = "France"; break;
        case 'I': region_str = "Italy"; break;
        case 'S': region_str = "Spain"; break;
        case 'K': region_str = "Korea"; break;
        case 'W': region_str = "Taiwan"; break;
        case 'L': region_str = "World"; break;
        default: break;
    }

    if (is_wii) {
        result->console = EMU_CONSOLE_WII;
        result->confidence = 100;
        snprintf(result->details, sizeof(result->details),
                 "Wii | %s | %s | %s", name, maker, region_str);
    } else {
        result->console = EMU_CONSOLE_GAMECUBE;
        result->confidence = 100;
        snprintf(result->details, sizeof(result->details),
                 "GameCube | %s | %s | %s", name, maker, region_str);
    }
    return 1;
}

// =============================================================================
// Nintendo 3DS
// =============================================================================

int emu_detect_3ds(const uint8_t* data, size_t size, EmuDetectResult* result) {
    if (size < 0x200) return 0;

    if (memcmp(data + 0x100, "NCSD", 4) == 0) {
        uint32_t media_size = read_le32(data + 0x104);
        result->console = EMU_CONSOLE_NINTENDO_3DS;
        result->confidence = 100;
        snprintf(result->details, sizeof(result->details),
                 "Nintendo 3DS NCSD image | %.1fMB",
                 (media_size * 512) / (1024.0 * 1024.0));
        return 1;
    }

    if (memcmp(data + 0x100, "NCCH", 4) == 0) {
        result->console = EMU_CONSOLE_NINTENDO_3DS;
        result->confidence = 95;
        snprintf(result->details, sizeof(result->details),
                 "Nintendo 3DS NCCH executable");
        return 1;
    }

    if (memcmp(data, "CIA", 3) == 0) {
        result->console = EMU_CONSOLE_NINTENDO_3DS;
        result->confidence = 70;
        snprintf(result->details, sizeof(result->details),
                 "Nintendo 3DS CIA (may be encrypted)");
        return 1;
    }

    return 0;
}

// =============================================================================
// Main entry points
// =============================================================================

EmuConsoleType emu_detect_console_from_memory(const uint8_t* data, size_t size,
                                               EmuDetectResult* result) {
    EmuDetectResult local;
    if (!result) result = &local;
    memset(result, 0, sizeof(EmuDetectResult));
    result->console = EMU_CONSOLE_UNKNOWN;

    if (!data || size < 16) return EMU_CONSOLE_UNKNOWN;

    result->crc32 = emu_crc32(data, size);

    // Most specific formats first to avoid false positives.
    if (emu_detect_3ds(data, size, result))     return result->console;
    if (emu_detect_gc_wii(data, size, result))  return result->console;
    if (emu_detect_n64(data, size, result))     return result->console;
    if (emu_detect_gba(data, size, result))     return result->console;
    if (emu_detect_gb_gbc(data, size, result))  return result->console;
    if (emu_detect_nes(data, size, result))     return result->console;
    if (emu_detect_nds(data, size, result))     return result->console;
    if (emu_detect_snes(data, size, result))    return result->console;

    return EMU_CONSOLE_UNKNOWN;
}

EmuConsoleType emu_detect_console_from_file(const char* path, EmuDetectResult* result) {
    EmuDetectResult local;
    if (!result) result = &local;
    memset(result, 0, sizeof(EmuDetectResult));
    result->console = EMU_CONSOLE_UNKNOWN;

    // Cartridge ROMs can be several megabytes (a SNES cart is ~2-8 MB, GBA up
    // to 32 MB, NDS up to 512 MB). A small cap here silently failed on larger
    // ROMs, leaving crc32 at 0 and falling back to extension-only detection, so
    // the save-folder name and confidence were both wrong. Cover every
    // supported cartridge; anything bigger still falls back to its extension.
    const size_t kDetectMaxBytes = (size_t)512 * 1024 * 1024;

    size_t size = 0;
    uint8_t* data = emu_read_file(path, &size, kDetectMaxBytes);

    EmuConsoleType type = EMU_CONSOLE_UNKNOWN;
    if (data && emu_zip_is_zip(data, size)) {
        // ROMs are commonly distributed as .zip archives; look inside.
        free(data);
        data = NULL;

        size_t zsize = 0;
        uint8_t* zdata = emu_read_file(path, &zsize, kDetectMaxBytes);
        if (zdata) {
            uint8_t* rom = NULL;
            size_t   romsize = 0;
            char     name[520];
            if (emu_zip_extract_rom(zdata, zsize, &rom, &romsize, name, sizeof(name)) && rom) {
                type = emu_detect_console_from_memory(rom, romsize, result);
                free(rom);
            }
            free(zdata);
        }
    } else if (data) {
        type = emu_detect_console_from_memory(data, size, result);
        free(data);
    }

    // Extension fallback when header detection is inconclusive.
    if (type == EMU_CONSOLE_UNKNOWN) {
        struct { const char* ext; EmuConsoleType c; } map[] = {
            {"nes", EMU_CONSOLE_NES}, {"nsf", EMU_CONSOLE_NES},
            {"fds", EMU_CONSOLE_NES}, {"unf", EMU_CONSOLE_NES},
            {"unif", EMU_CONSOLE_NES},
            {"sfc", EMU_CONSOLE_SNES}, {"smc", EMU_CONSOLE_SNES},
            {"fig", EMU_CONSOLE_SNES}, {"swc", EMU_CONSOLE_SNES},
            {"gb",  EMU_CONSOLE_GAMEBOY},
            {"gbc", EMU_CONSOLE_GAMEBOY_COLOR},
            {"gba", EMU_CONSOLE_GAMEBOY_ADVANCE},
            {"nds", EMU_CONSOLE_NINTENDO_DS}, {"dsi", EMU_CONSOLE_NINTENDO_DS},
            {"srl", EMU_CONSOLE_NINTENDO_DS},
            {"z64", EMU_CONSOLE_NINTENDO_64}, {"n64", EMU_CONSOLE_NINTENDO_64},
            {"v64", EMU_CONSOLE_NINTENDO_64},
            {"gcm", EMU_CONSOLE_GAMECUBE}, {"gcz", EMU_CONSOLE_GAMECUBE},
            {"wbfs", EMU_CONSOLE_WII}, {"rvz", EMU_CONSOLE_WII},
            {"wia", EMU_CONSOLE_WII}, {"iso", EMU_CONSOLE_GAMECUBE},
            {"ndd", EMU_CONSOLE_NINTENDO_64},
            {"3ds", EMU_CONSOLE_NINTENDO_3DS}, {"cia", EMU_CONSOLE_NINTENDO_3DS},
            {"cci", EMU_CONSOLE_NINTENDO_3DS}, {"cxi", EMU_CONSOLE_NINTENDO_3DS},
            {"app", EMU_CONSOLE_NINTENDO_3DS}, {"3dsx", EMU_CONSOLE_NINTENDO_3DS},
            {NULL, EMU_CONSOLE_UNKNOWN}
        };
        for (int i = 0; map[i].ext; i++) {
            if (emu_has_extension(path, map[i].ext)) {
                result->console = map[i].c;
                result->confidence = 40;
                snprintf(result->details, sizeof(result->details),
                         "Detected by file extension (.%s)", map[i].ext);
                return map[i].c;
            }
        }
    }

    return type;
}

// =============================================================================
// Extension tables
// =============================================================================

static const char* nes_exts[]  = {"nes", "nsf", "fds", "unf", "unif", "zip", NULL};
static const char* snes_exts[] = {"sfc", "smc", "fig", "swc", "bs", "st", "zip", NULL};
static const char* gb_exts[]   = {"gb", "zip", NULL};
static const char* gbc_exts[]  = {"gbc", "zip", NULL};
static const char* gba_exts[]  = {"gba", "zip", NULL};
static const char* nds_exts[]  = {"nds", "dsi", "ids", "srl", "zip", NULL};
static const char* n64_exts[]  = {"z64", "n64", "v64", "rom", "ndd", "zip", NULL};
static const char* gc_exts[]   = {"iso", "gcm", "gcz", "rvz", "wia", "dol", "elf", NULL};
static const char* wii_exts[]  = {"iso", "wbfs", "rvz", "wia", "dol", "elf", NULL};
static const char* ds3_exts[]  = {"3ds", "cia", "cci", "cxi", "app", "3dsx", NULL};

const char** emu_get_extensions_for_console(EmuConsoleType console, int* count) {
    if (count) *count = 0;
    switch (console) {
        case EMU_CONSOLE_NES:             if (count) *count = 6; return nes_exts;
        case EMU_CONSOLE_SNES:            if (count) *count = 7; return snes_exts;
        case EMU_CONSOLE_GAMEBOY:         if (count) *count = 2; return gb_exts;
        case EMU_CONSOLE_GAMEBOY_COLOR:   if (count) *count = 2; return gbc_exts;
        case EMU_CONSOLE_GAMEBOY_ADVANCE: if (count) *count = 2; return gba_exts;
        case EMU_CONSOLE_NINTENDO_DS:     if (count) *count = 5; return nds_exts;
        case EMU_CONSOLE_NINTENDO_64:     if (count) *count = 6; return n64_exts;
        case EMU_CONSOLE_GAMECUBE:        if (count) *count = 7; return gc_exts;
        case EMU_CONSOLE_WII:             if (count) *count = 6; return wii_exts;
        case EMU_CONSOLE_NINTENDO_3DS:    if (count) *count = 6; return ds3_exts;
        default: return NULL;
    }
}

int emu_is_supported_extension(const char* extension) {
    if (!extension) return 0;
    static const char* all_exts[] = {
        "nes", "nsf", "fds", "unf", "unif",
        "sfc", "smc", "fig", "swc", "bs", "st",
        "gb", "gbc", "gba",
        "nds", "dsi", "ids", "srl",
        "z64", "n64", "v64", "rom", "ndd",
        "iso", "gcm", "gcz", "rvz", "wia", "dol", "elf", "wbfs",
        "3ds", "cia", "cci", "cxi", "app", "3dsx",
        "zip", NULL
    };
    for (int i = 0; all_exts[i]; i++)
        if (emu_strcasecmp(extension, all_exts[i]) == 0) return 1;
    return 0;
}

const char* emu_console_name(EmuConsoleType console) {
    switch (console) {
        case EMU_CONSOLE_NES:             return "Nintendo Entertainment System";
        case EMU_CONSOLE_SNES:            return "Super Nintendo";
        case EMU_CONSOLE_GAMEBOY:         return "Game Boy";
        case EMU_CONSOLE_GAMEBOY_COLOR:   return "Game Boy Color";
        case EMU_CONSOLE_GAMEBOY_ADVANCE: return "Game Boy Advance";
        case EMU_CONSOLE_NINTENDO_DS:     return "Nintendo DS";
        case EMU_CONSOLE_NINTENDO_64:     return "Nintendo 64";
        case EMU_CONSOLE_GAMECUBE:        return "Nintendo GameCube";
        case EMU_CONSOLE_WII:             return "Nintendo Wii";
        case EMU_CONSOLE_NINTENDO_3DS:    return "Nintendo 3DS";
        default:                          return "Unknown";
    }
}

const char* emu_console_folder_name(EmuConsoleType console) {
    switch (console) {
        case EMU_CONSOLE_NES:             return "nes";
        case EMU_CONSOLE_SNES:            return "snes";
        case EMU_CONSOLE_GAMEBOY:         return "gb";
        case EMU_CONSOLE_GAMEBOY_COLOR:   return "gbc";
        case EMU_CONSOLE_GAMEBOY_ADVANCE: return "gba";
        case EMU_CONSOLE_NINTENDO_DS:     return "nds";
        case EMU_CONSOLE_NINTENDO_64:     return "n64";
        case EMU_CONSOLE_GAMECUBE:        return "gc";
        case EMU_CONSOLE_WII:             return "wii";
        case EMU_CONSOLE_NINTENDO_3DS:    return "3ds";
        default:                          return "other";
    }
}