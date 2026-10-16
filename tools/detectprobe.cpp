// Developer probe: call the detector directly on a file and print what it
// decides, so console detection can be tested without the GUI in the way.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "rom_detect.h"

static bool slurp(const char* path, std::vector<unsigned char>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: detectprobe <rom>\n"); return 2; }
    const char* path = argv[1];

    EmuDetectResult det;
    EmuConsoleType c = emu_detect_console_from_file(path, &det);

    printf("file        : %s\n", path);
    printf("console     : %d  (%s)\n", (int)c, emu_console_name(c));
    printf("confidence  : %d\n", det.confidence);
    printf("details     : %s\n", det.details);
    printf("crc32       : %08X\n\n", det.crc32);

    std::vector<unsigned char> data;
    if (!slurp(path, data)) { printf("could not read the file\n"); return 1; }

    printf("size        : %zu bytes\n", data.size());
    printf("first 8     : ");
    for (size_t i = 0; i < 8 && i < data.size(); i++) printf("%02X ", data[i]);
    printf("\n");

    // Feed the same bytes to the in-memory detector to compare.
    EmuDetectResult mem;
    EmuConsoleType mc = emu_detect_console_from_memory(data.data(), data.size(), &mem);
    printf("from_memory : %d  (%s)\n", (int)mc, emu_console_name(mc));

    printf("n64 direct  : %d\n", emu_detect_n64(data.data(), data.size(), &mem));
    printf("title 0x20  : ");
    for (size_t i = 0x20; i < 0x20 + 20 && i < data.size(); i++) {
        unsigned char ch = data[i];
        printf("%c", (ch >= 0x20 && ch < 0x7F) ? ch : '.');
    }
    printf("\n");
    return 0;
}