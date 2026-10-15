/**
 * Nintendall - ROM Detection
 * 
 * Detects console type from ROM header/data.
 * Used by frontend to auto-select correct core.
 */

#ifndef ROM_DETECT_H
#define ROM_DETECT_H

#include "emu_api.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Detection Result
// =============================================================================

typedef struct EmuDetectResult {
    EmuConsoleType console;
    int confidence;             // 0-100
    char details[256];          // Human-readable details
    uint32_t crc32;             // ROM CRC32
} EmuDetectResult;

// Detect console type from ROM file
// Returns EMU_CONSOLE_UNKNOWN if unable to detect
EmuConsoleType emu_detect_console_from_file(const char* path, EmuDetectResult* result);

// Detect console type from memory buffer
EmuConsoleType emu_detect_console_from_memory(const uint8_t* data, size_t size, EmuDetectResult* result);

// Get supported extensions for a console type
const char** emu_get_extensions_for_console(EmuConsoleType console, int* count);

// Check if extension is supported by any core
int emu_is_supported_extension(const char* extension);

// Human-readable console name (e.g. "Nintendo Entertainment System")
const char* emu_console_name(EmuConsoleType console);

// =============================================================================
// Console-Specific Detection Functions
// =============================================================================

// Get a short, filesystem-friendly folder name for a console's saves/states.
const char* emu_console_folder_name(EmuConsoleType console);

// NES: iNES, NES 2.0, UNIF headers
int emu_detect_nes(const uint8_t* data, size_t size, EmuDetectResult* result);

// SNES: SMC/SFC headers, LoROM/HiROM/ExHiROM
int emu_detect_snes(const uint8_t* data, size_t size, EmuDetectResult* result);

// GB/GBC: Nintendo header at 0x100-0x14F
int emu_detect_gb_gbc(const uint8_t* data, size_t size, EmuDetectResult* result);

// GBA: Nintendo header at 0x000-0x1FF
int emu_detect_gba(const uint8_t* data, size_t size, EmuDetectResult* result);

// NDS: Nintendo DS header at 0x000-0x1FF
int emu_detect_nds(const uint8_t* data, size_t size, EmuDetectResult* result);

// N64: Big/little endian detection, header at 0x000
int emu_detect_n64(const uint8_t* data, size_t size, EmuDetectResult* result);

// GC/Wii: GCM/ISO/WIA headers
int emu_detect_gc_wii(const uint8_t* data, size_t size, EmuDetectResult* result);

// 3DS: CCI/CXI/NCCH formats
int emu_detect_3ds(const uint8_t* data, size_t size, EmuDetectResult* result);

#ifdef __cplusplus
}
#endif

#endif // ROM_DETECT_H