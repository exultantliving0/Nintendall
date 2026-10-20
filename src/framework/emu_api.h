/**
 * Nintendall - Common C API
 * 
 * Every emulator core must implement this interface.
 * This allows the frontend to use any core through a common API.
 */

#ifndef EMU_API_H
#define EMU_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Core Types
// =============================================================================

typedef enum EmuConsoleType {
    EMU_CONSOLE_UNKNOWN = 0,
    EMU_CONSOLE_NES,
    EMU_CONSOLE_SNES,
    EMU_CONSOLE_GAMEBOY,
    EMU_CONSOLE_GAMEBOY_COLOR,
    EMU_CONSOLE_GAMEBOY_ADVANCE,
    EMU_CONSOLE_NINTENDO_DS,
    EMU_CONSOLE_NINTENDO_64,
    EMU_CONSOLE_GAMECUBE,
    EMU_CONSOLE_WII,
    EMU_CONSOLE_NINTENDO_3DS,
} EmuConsoleType;

typedef enum EmuRegion {
    EMU_REGION_NTSC = 0,
    EMU_REGION_PAL,
    EMU_REGION_MPAL,
} EmuRegion;

typedef enum EmuSaveStateFormat {
    EMU_SAVE_FORMAT_RAW = 0,      // Core-specific binary format
    EMU_SAVE_FORMAT_COMMON,       // Unified format (future)
} EmuSaveStateFormat;

// =============================================================================
// Video/Audio/Input Callbacks
// =============================================================================

// Video frame callback - called each frame with rendered framebuffer
// format: 32-bit RGBA, pitch = width * 4
typedef void (*EmuVideoCallback)(void* userdata, const uint32_t* pixels, 
                                  int width, int height, int pitch);

// Audio sample callback - called with stereo 16-bit samples
// samples = number of stereo frames (left+right pairs)
typedef void (*EmuAudioCallback)(void* userdata, const int16_t* samples, int count);

// Input polling callback - called before each frame to get input state
typedef void (*EmuInputPollCallback)(void* userdata);

// Input state query - returns current state of a button
typedef int (*EmuInputStateCallback)(void* userdata, unsigned port, unsigned device, 
                                      unsigned index, unsigned id);

// =============================================================================
// Core Configuration
// =============================================================================

typedef struct EmuConfig {
    // Video
    int video_width;
    int video_height;
    int fullscreen;
    int vsync;
    int scale_factor;           // Integer scaling (1x, 2x, 3x...)
    int aspect_correction;      // Maintain correct aspect ratio
    
    // Audio
    int audio_sample_rate;      // 44100, 48000, etc.
    int audio_buffer_size;      // Samples per buffer
    int audio_volume;           // 0-100
    
    // System
    EmuRegion region;           // Force region
    int fast_forward;           // Speed multiplier (1=normal, 2=2x, etc.)
    int rewind_enable;          // Enable rewind feature
    
    // Paths
    const char* save_dir;       // Save states, SRAM, etc.
    const char* shader_dir;     // Shader presets
    const char* bios_dir;       // Required BIOS files
    
    // Core-specific options (opaque pointer)
    void* core_options;
    size_t core_options_size;
} EmuConfig;

// Default configuration
void emu_config_default(EmuConfig* config);

// =============================================================================
// Core Instance
// =============================================================================

// Opaque handle to emulator core instance
typedef struct EmuCore* EmuCoreHandle;

// Core information
typedef struct EmuCoreInfo {
    const char* name;           // "Nestopia", "bsnes", etc.
    const char* version;        // Core version
    const char* author;         // Core authors
    EmuConsoleType console;     // Console this core emulates
    const char** extensions;    // Supported file extensions (NULL-terminated)
    int num_extensions;
    int has_save_states;        // Supports save/load state
    int has_rewind;             // Supports rewind
    int has_cheats;             // Supports cheat codes
    int needs_bios;             // Requires BIOS files
} EmuCoreInfo;

// Get core information
const EmuCoreInfo* emu_core_get_info(void);

// Create core instance
// Returns NULL on failure
EmuCoreHandle emu_core_create(const EmuConfig* config,
                               EmuVideoCallback video_cb,
                               EmuAudioCallback audio_cb,
                               EmuInputPollCallback input_poll_cb,
                               EmuInputStateCallback input_state_cb,
                               void* userdata);

// Destroy core instance
void emu_core_destroy(EmuCoreHandle core);

// =============================================================================
// ROM Loading
// =============================================================================

typedef enum EmuLoadResult {
    EMU_LOAD_OK = 0,
    EMU_LOAD_ERROR_INVALID_ROM,
    EMU_LOAD_ERROR_UNSUPPORTED_MAPPER,
    EMU_LOAD_ERROR_BIOS_MISSING,
    EMU_LOAD_ERROR_OUT_OF_MEMORY,
    EMU_LOAD_ERROR_INTERNAL,
} EmuLoadResult;

// Load ROM from memory buffer
EmuLoadResult emu_core_load_rom(EmuCoreHandle core, 
                                 const uint8_t* data, size_t size,
                                 const char* filename);

// Load ROM from file path
EmuLoadResult emu_core_load_rom_file(EmuCoreHandle core, const char* path);

// Unload current ROM
void emu_core_unload_rom(EmuCoreHandle core);

// Get detected console type for loaded ROM
EmuConsoleType emu_core_get_console_type(EmuCoreHandle core);

// Get ROM info (title, CRC32, mapper, etc.)
typedef struct EmuRomInfo {
    char title[64];
    uint32_t crc32;
    char mapper_name[64];
    EmuConsoleType console;
    EmuRegion region;
    int has_battery;            // Has save RAM
    int rom_size;               // ROM size in bytes
} EmuRomInfo;

void emu_core_get_rom_info(EmuCoreHandle core, EmuRomInfo* info);

// =============================================================================
// Execution Control
// =============================================================================

// Run one frame (returns false if emulation should stop)
int emu_core_run_frame(EmuCoreHandle core);

// Run for specified number of cycles (for synchronization)
void emu_core_run_cycles(EmuCoreHandle core, uint64_t cycles);

// Reset emulation (soft reset)
void emu_core_reset(EmuCoreHandle core);

// Hard reset (power cycle)
void emu_core_power_cycle(EmuCoreHandle core);

// Pause/unpause
void emu_core_set_paused(EmuCoreHandle core, int paused);
int emu_core_is_paused(EmuCoreHandle core);

// Fast forward
void emu_core_set_fast_forward(EmuCoreHandle core, int enabled, int speed);

// =============================================================================
// Save States
// =============================================================================

// Save state to memory buffer
// Returns size of state data, or 0 on failure
// If buffer is NULL, returns required buffer size
size_t emu_core_save_state(EmuCoreHandle core, void* buffer, size_t buffer_size);

// Load state from memory buffer
int emu_core_load_state(EmuCoreHandle core, const void* buffer, size_t size);

// Save state to file
int emu_core_save_state_file(EmuCoreHandle core, const char* path);

// Load state from file
int emu_core_load_state_file(EmuCoreHandle core, const char* path);

// Get save state metadata
typedef struct EmuSaveStateMeta {
    char description[128];
    uint32_t timestamp;         // Unix timestamp
    uint32_t frame_count;
    EmuConsoleType console;
    char rom_title[64];
    uint32_t rom_crc32;
} EmuSaveStateMeta;

int emu_core_get_save_state_meta(const void* buffer, size_t size, EmuSaveStateMeta* meta);

// =============================================================================
// Save RAM (SRAM/Flash/EEPROM)
// =============================================================================

// Save battery-backed RAM to file
int emu_core_save_sram(EmuCoreHandle core, const char* path);

// Load battery-backed RAM from file
int emu_core_load_sram(EmuCoreHandle core, const char* path);

// =============================================================================
// Cheats
// =============================================================================

typedef struct EmuCheat {
    uint32_t address;
    uint32_t value;
    uint32_t compare;           // For conditional cheats (0 = always)
    int enabled;
    char description[64];
} EmuCheat;

int emu_core_cheat_add(EmuCoreHandle core, const EmuCheat* cheat);
int emu_core_cheat_remove(EmuCoreHandle core, int index);
int emu_core_cheat_enable(EmuCoreHandle core, int index, int enabled);
int emu_core_cheat_get_count(EmuCoreHandle core);
int emu_core_cheat_get(EmuCoreHandle core, int index, EmuCheat* cheat);

// =============================================================================
// Input
// =============================================================================

// Standard input device types
typedef enum EmuDeviceType {
    EMU_DEVICE_NONE = 0,
    EMU_DEVICE_JOYPAD,          // Standard gamepad
    EMU_DEVICE_MOUSE,           // Mouse (for light gun, etc.)
    EMU_DEVICE_KEYBOARD,        // Keyboard
    EMU_DEVICE_LIGHTGUN,        // Light gun (NES Zapper, etc.)
    EMU_DEVICE_ANALOG,          // Analog stick
    EMU_DEVICE_POINTER,         // Wii Remote pointer, DS touch
    EMU_DEVICE_MOTION,          // Motion controls
} EmuDeviceType;

// Standard button/axis IDs (subset - cores can have more)
typedef enum EmuInputId {
    // Digital buttons
    EMU_ID_A = 0,
    EMU_ID_B,
    EMU_ID_X,
    EMU_ID_Y,
    EMU_ID_L,
    EMU_ID_R,
    EMU_ID_L2,
    EMU_ID_R2,
    EMU_ID_L3,
    EMU_ID_R3,
    EMU_ID_START,
    EMU_ID_SELECT,
    EMU_ID_HOME,
    EMU_ID_UP,
    EMU_ID_DOWN,
    EMU_ID_LEFT,
    EMU_ID_RIGHT,
       EMU_ID_C_UP,
       EMU_ID_C_DOWN,
       EMU_ID_C_LEFT,
       EMU_ID_C_RIGHT,
       EMU_ID_JOYSTICK_UP,
       EMU_ID_JOYSTICK_DOWN,
       EMU_ID_JOYSTICK_LEFT,
       EMU_ID_JOYSTICK_RIGHT,
    
    // Analog axes
    EMU_ID_ANALOG_LEFT_X = 100,
    EMU_ID_ANALOG_LEFT_Y,
    EMU_ID_ANALOG_RIGHT_X,
    EMU_ID_ANALOG_RIGHT_Y,
    EMU_ID_TRIGGER_LEFT,
    EMU_ID_TRIGGER_RIGHT,
    
    // Pointer/touch
    EMU_ID_POINTER_X = 200,
    EMU_ID_POINTER_Y,
    EMU_ID_POINTER_PRESSED,
    
    // Motion
    EMU_ID_ACCEL_X = 300,
    EMU_ID_ACCEL_Y,
    EMU_ID_ACCEL_Z,
    EMU_ID_GYRO_X,
    EMU_ID_GYRO_Y,
    EMU_ID_GYRO_Z,
} EmuInputId;

// Set input device for a port
void emu_core_set_input_device(EmuCoreHandle core, unsigned port, EmuDeviceType device);

// =============================================================================
// Video/Audio Control
// =============================================================================

// Set video callback (can be changed at runtime)
void emu_core_set_video_callback(EmuCoreHandle core, EmuVideoCallback cb, void* userdata);

// Set audio callback
void emu_core_set_audio_callback(EmuCoreHandle core, EmuAudioCallback cb, void* userdata);

// Get current video output size
void emu_core_get_video_size(EmuCoreHandle core, int* width, int* height);

// =============================================================================
// Core-Specific Options
// =============================================================================

// Get/set core-specific options (opaque key-value)
// Returns 0 on success, -1 if key not found
int emu_core_get_option(EmuCoreHandle core, const char* key, void* value, size_t* size);
int emu_core_set_option(EmuCoreHandle core, const char* key, const void* value, size_t size);

// =============================================================================
// Logging/Errors
// =============================================================================

typedef enum EmuLogLevel {
    EMU_LOG_DEBUG = 0,
    EMU_LOG_INFO,
    EMU_LOG_WARN,
    EMU_LOG_ERROR,
} EmuLogLevel;

typedef void (*EmuLogCallback)(void* userdata, EmuLogLevel level, const char* message);

void emu_core_set_log_callback(EmuCoreHandle core, EmuLogCallback cb, void* userdata);

const char* emu_load_result_string(EmuLoadResult result);

#ifdef __cplusplus
}
#endif

#endif // EMU_API_H
