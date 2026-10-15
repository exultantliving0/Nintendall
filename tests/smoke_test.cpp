/**
 * Nintendall - core smoke test
 *
 * Drives the compiled-in cores end to end through the same API the frontend
 * uses: console detection -> registry lookup -> create -> load ROM -> run
 * frames -> save/load state. It does not open a window; it just counts the
 * video/audio callbacks and prints what happened.
 *
 * Enable with -DBUILD_SMOKE_TEST=ON, then:
 *
 *     build/bin/nintendall_test <rom-file>
 *
 * Exit code 0 means a frame was rendered with visible pixels, audio samples
 * were produced, and a save state round-tripped. (A deliberately blank
 * homebrew ROM will report FAIL on the pixel check even though the core is
 * fine; read the printed counters in that case.)
 */

#include <cstdio>
#include <cstring>
#include <vector>

#include "emu_api.h"
#include "emu_util.h"
#include "rom_detect.h"
#include "core_registry.h"
#include "zip.h"

#include <cstdlib>

namespace {

int g_video_frames = 0;
int g_video_w = 0, g_video_h = 0;
// 64-bit: a busy core can emit well over INT32_MAX samples/pixels across the
// whole run, and `long` is 32-bit on Windows/MinGW (it would wrap negative and
// fail the > 0 checks below even when everything is working).
long long g_nonblack = 0;
int g_audio_calls = 0;
long long g_audio_samples = 0;
double g_audio_energy = 0.0;

void video_cb(void*, const uint32_t* px, int w, int h, int pitch) {
    g_video_frames++;
    g_video_w = w; g_video_h = h;
    if (!px) return;
    const unsigned char* base = (const unsigned char*)px;
    for (int y = 0; y < h; y++) {
        const uint32_t* row = (const uint32_t*)(base + (size_t)y * pitch);
        for (int x = 0; x < w; x++)
            if ((row[x] & 0x00FFFFFFu) != 0) g_nonblack++;
    }
}

void audio_cb(void*, const int16_t* s, int count) {
    g_audio_calls++;
    for (int i = 0; i < count * 2; i++) {
        g_audio_samples++;
        g_audio_energy += (s[i] < 0 ? -s[i] : s[i]);
    }
}

int input_state(void*, unsigned, unsigned, unsigned, unsigned) { return 0; }

size_t file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n > 0 ? (size_t)n : 0;
}

bool files_identical(const char* a, const char* b) {
    size_t na = 0, nb = 0;
    unsigned char* da = emu_read_file(a, &na, 0);
    unsigned char* db = emu_read_file(b, &nb, 0);
    bool same = da && db && na == nb && memcmp(da, db, na) == 0;
    free(da);
    free(db);
    return same;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== Nintendall core smoke test ==\n\n");

    if (argc < 2) {
        printf("usage: nintendall_test <rom-file>\n");
        return 2;
    }

    printf("== registry ==\n");
    printf("cores compiled in: %d\n", emu_registry_count());
    for (int i = 0; i < emu_registry_count(); i++) {
        const EmuCoreModule* m = emu_registry_get(i);
        const EmuCoreInfo* in = m->get_info();
        printf("  - %-10s (id=%-10s) -> %s\n", in->name, m->id,
               emu_console_name(in->console));
    }

    printf("\n== detection ==\n");

    // Unpack archives (.zip) the same way the frontend does, so a zipped ROM
    // is detected and loaded from the ROM inside the archive.
    char cache_base[600];
    const char* tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = ".";
    snprintf(cache_base, sizeof(cache_base), "%s/nintendall_test_cache", tmp);

    char display[520] = "";
    char* romfile = emu_prepare_rom(argv[1], cache_base, display, sizeof(display));
    if (!romfile) { printf("FAIL: could not prepare %s\n", argv[1]); return 2; }
    printf("input    : %s\n", argv[1]);
    printf("rom file : %s\n", romfile);
    if (display[0]) printf("game     : %s\n", display);

    // Name the game the same way the frontend does, so battery saves written
    // here land next to (and are read back from) the ones a real session made.
    char stem[256];
    {
        const char* name_src = display[0] ? display : argv[1];
        emu_get_basename(name_src, stem, sizeof(stem));
        emu_strip_extension(stem, stem, sizeof(stem));
    }

    EmuDetectResult det;
    memset(&det, 0, sizeof(det));
    EmuConsoleType console = emu_detect_console_from_file(romfile, &det);
    printf("console  : %s (confidence %d, crc32 %08X)\n",
           emu_console_name(console), det.confidence, det.crc32);
    printf("details  : %s\n", det.details);

    // The detection API is also archive-aware: detecting the raw input should
    // give the same answer (this is what a caller that skips emu_prepare_rom
    // would see).
    {
        EmuDetectResult zdet;
        memset(&zdet, 0, sizeof(zdet));
        EmuConsoleType zc = emu_detect_console_from_file(argv[1], &zdet);
        printf("archive  : %s (crc32 %08X) via direct detection\n",
               emu_console_name(zc), zdet.crc32);
    }

    const EmuCoreModule* mod = emu_registry_find_for_console(console);
    if (!mod) { printf("FAIL: no compiled-in core for %s\n", emu_console_name(console)); free(romfile); return 1; }
    printf("core     : %s\n", mod->id);

    printf("\n== run ==\n");
    EmuConfig cfg;
    emu_config_default(&cfg);

    // Where battery saves live.  Defaults to the working directory; point it at
    // the frontend's saves/ folder to test against saves a real session made.
    char save_dir[1024];
    {
        const char* sd = getenv("NINTENDALL_SMOKE_SAVEDIR");
        snprintf(save_dir, sizeof(save_dir), "%s", (sd && *sd) ? sd : ".");
    }
    cfg.save_dir = save_dir;

    EmuCoreHandle core = mod->create(&cfg, video_cb, audio_cb, nullptr, input_state, nullptr);
    if (!core) { printf("FAIL: create\n"); free(romfile); return 1; }

    EmuLoadResult lr = mod->load_rom_file(core, romfile);
    printf("load     : %d (%s)\n", (int)lr, emu_load_result_string(lr));
    if (lr != EMU_LOAD_OK) { mod->destroy(core); free(romfile); return 1; }

    int frames = 120;
    {
        const char* fe = getenv("NINTENDALL_SMOKE_FRAMES");
        if (fe && atoi(fe) > 0) frames = atoi(fe);
    }
    for (int i = 0; i < frames; i++) mod->run_frame(core);
    printf("video    : %d frames, %dx%d, %lld non-black pixels\n",
           g_video_frames, g_video_w, g_video_h, g_nonblack);
    printf("audio    : %d calls, %lld samples, energy %.0f\n",
           g_audio_calls, g_audio_samples, g_audio_energy);

    printf("\n== save state ==\n");
    int state_ok = 0;
    size_t need = mod->save_state(core, nullptr, 0);
    printf("size     : %zu bytes\n", need);
    if (need) {
        std::vector<unsigned char> buf(need);
        size_t written = mod->save_state(core, buf.data(), buf.size());
        for (int i = 0; i < 30; i++) mod->run_frame(core);
        int ok = mod->load_state(core, buf.data(), written);
        for (int i = 0; i < 30; i++) mod->run_frame(core);
        printf("roundtrip: written=%zu loaded=%d\n", written, ok);
        state_ok = (written == need) && ok;
    }

    mod->destroy(core);

    // ---- battery save (SRAM / EEPROM / flash) ------------------------------
    //
    // The frontend hands a game's .sav back to the core by hand, so this is the
    // one place where "does the game still have its data after a restart" can
    // be checked without a human: write the save out, start a brand new core on
    // the same ROM, feed it that .sav, and check the same bytes come back.
    printf("\n== battery save ==\n");

    char sav_seed[1400], sav_a[1400], sav_b[1400];
    snprintf(sav_seed, sizeof(sav_seed), "%s/%s.sav", save_dir, stem);
    snprintf(sav_a, sizeof(sav_a), "%s/%s.smoke1.sav", save_dir, stem);
    snprintf(sav_b, sizeof(sav_b), "%s/%s.smoke2.sav", save_dir, stem);
    remove(sav_a);
    remove(sav_b);

    int save_ok = 1;
    EmuCoreHandle c1 = mod->create(&cfg, video_cb, audio_cb, nullptr, input_state, nullptr);
    if (!c1 || mod->load_rom_file(c1, romfile) != EMU_LOAD_OK) {
        printf("FAIL: could not start a session to test the save\n");
        if (c1) mod->destroy(c1);
        save_ok = 0;
    } else {
        // A save left behind by an earlier session is far more interesting to
        // round-trip than empty memory, so restore it first if there is one.
        int seed_rc = mod->load_sram ? mod->load_sram(c1, sav_seed) : -1;
        printf("seed     : rc=%d size=%zu\n", seed_rc, file_size(sav_seed));

        for (int i = 0; i < frames; i++) mod->run_frame(c1);
        int write_rc = mod->save_sram(c1, sav_a);
        size_t size_a = file_size(sav_a);
        printf("write    : rc=%d size=%zu\n", write_rc, size_a);
        mod->destroy(c1);

        if (size_a == 0) {
            printf("note     : this core keeps no frontend-managed save\n");
        } else {
            EmuCoreHandle c2 = mod->create(&cfg, video_cb, audio_cb, nullptr, input_state, nullptr);
            if (!c2 || mod->load_rom_file(c2, romfile) != EMU_LOAD_OK) {
                printf("FAIL: could not start the second session\n");
                if (c2) mod->destroy(c2);
                save_ok = 0;
            } else {
                int load_rc = mod->load_sram(c2, sav_a);
                for (int i = 0; i < frames; i++) mod->run_frame(c2);
                int write_rc2 = mod->save_sram(c2, sav_b);
                size_t size_b = file_size(sav_b);
                printf("reload   : rc=%d\n", load_rc);
                printf("rewrite  : rc=%d size=%zu\n", write_rc2, size_b);
                save_ok = (load_rc == 0) && (size_b == size_a) &&
                          files_identical(sav_a, sav_b);
                printf("roundtrip: %s\n", save_ok ? "identical" : "DIFFERENT");
                mod->destroy(c2);
            }
        }
    }

    free(romfile);
    remove(sav_a);
    remove(sav_b);

    int pass = (g_video_frames >= 100) && (g_nonblack > 0) &&
               (g_audio_samples > 0) && state_ok && save_ok;
    printf("\n== result ==\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
