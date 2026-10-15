/**
 * Nintendall - ZIP archive support (implementation)
 *
 * Container: standard ZIP (end-of-central-directory + central directory).
 * Compression: "stored" (0) and "deflate" (8). The DEFLATE decoder is a
 * compact, self-contained implementation (canonical Huffman, RFC 1951);
 * no external zlib/miniz is required.
 *
 * ZIP64, encryption and other compression methods are not supported; ROM
 * archives do not use them.
 */

#include "zip.h"
#include "emu_util.h"
#include "rom_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Little-endian readers
// =============================================================================

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char* dup_str(const char* s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char* out = (char*)malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

// =============================================================================
// DEFLATE (RFC 1951) decoder
// =============================================================================

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const uint8_t* in;  size_t inlen;  size_t incnt;
    uint8_t*       out; size_t outlen; size_t outcnt;
    int bitbuf;
    int bitcnt;
} InflateState;

static int bits(InflateState* s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) return -1;
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

typedef struct {
    short* count;    // number of symbols of each length
    short* symbol;   // canonically ordered symbols
} Huffman;

static int huffman_decode(InflateState* s, const Huffman* h) {
    int len, code = 0, first = 0, index = 0;
    for (len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

// Returns 0 on success, negative on over-subscribed, positive (left) if the
// code is incomplete.
static int huffman_construct(Huffman* h, const short* length, int n) {
    int len, left;
    short offs[MAXBITS + 1];

    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;   // all lengths zero -> no code at all

    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;    // over-subscribed
    }

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);

    for (int symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = (short)symbol;

    return left;
}

static const short lens[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const short lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const short dists[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const short dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

// Returns 0 for end-of-block, 1 if the output filled up, negative on error.
static int inflate_codes(InflateState* s, const Huffman* lencode,
                         const Huffman* distcode) {
    for (;;) {
        int symbol = huffman_decode(s, lencode);
        if (symbol < 0) return symbol;

        if (symbol < 256) {
            if (s->outcnt == s->outlen) return 1;
            s->out[s->outcnt++] = (uint8_t)symbol;
        } else if (symbol == 256) {
            return 0;
        } else {
            symbol -= 257;
            if (symbol >= 29) return -1;

            int extra = bits(s, lext[symbol]);
            if (extra < 0) return -1;
            int len = lens[symbol] + extra;

            int dsym = huffman_decode(s, distcode);
            if (dsym < 0) return dsym;
            if (dsym >= 30) return -1;

            int dextra = bits(s, dext[dsym]);
            if (dextra < 0) return -1;
            int dist = dists[dsym] + dextra;

            if ((size_t)dist > s->outcnt) return -1;
            while (len--) {
                if (s->outcnt == s->outlen) return 1;
                s->out[s->outcnt] = s->out[s->outcnt - dist];
                s->outcnt++;
            }
        }
    }
}

static int inflate_stored(InflateState* s) {
    s->bitbuf = 0;
    s->bitcnt = 0;

    if (s->incnt + 4 > s->inlen) return -1;
    unsigned len  = s->in[s->incnt++];
    len |= ((unsigned)s->in[s->incnt++]) << 8;
    unsigned nlen = s->in[s->incnt++];
    nlen |= ((unsigned)s->in[s->incnt++]) << 8;
    if (len != ((~nlen) & 0xffff)) return -1;

    if (s->incnt + len > s->inlen) return -1;
    if (s->outcnt + len > s->outlen) return 1;
    while (len--) s->out[s->outcnt++] = s->in[s->incnt++];
    return 0;
}

static int inflate_fixed(InflateState* s) {
    short lencnt[MAXBITS + 1], lensym[FIXLCODES];
    short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    Huffman lencode = { lencnt, lensym };
    Huffman distcode = { distcnt, distsym };
    short lengths[FIXLCODES];
    int symbol;

    for (symbol = 0;   symbol < 144; symbol++) lengths[symbol] = 8;
    for (;              symbol < 256; symbol++) lengths[symbol] = 9;
    for (;              symbol < 280; symbol++) lengths[symbol] = 7;
    for (;              symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
    huffman_construct(&lencode, lengths, FIXLCODES);

    for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
    huffman_construct(&distcode, lengths, MAXDCODES);

    return inflate_codes(s, &lencode, &distcode);
}

static int inflate_dynamic(InflateState* s) {
    static const short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    short lencnt[MAXBITS + 1], lensym[MAXLCODES];
    short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    short lencnt2[MAXBITS + 1], lensym2[19];
    Huffman lencode  = { lencnt, lensym };
    Huffman distcode = { distcnt, distsym };
    Huffman lenlen   = { lencnt2, lensym2 };
    short lengths[320];
    int nlen, ndist, ncode, err;

    int b = bits(s, 5); if (b < 0) return -1; nlen = b + 257;
    b = bits(s, 5);     if (b < 0) return -1; ndist = b + 1;
    b = bits(s, 4);     if (b < 0) return -1; ncode = b + 4;

    if (nlen > MAXLCODES || ndist > MAXDCODES) return -1;

    for (int i = 0; i < ncode; i++) {
        b = bits(s, 3); if (b < 0) return -1;
        lengths[order[i]] = (short)b;
    }
    for (int i = ncode; i < 19; i++) lengths[order[i]] = 0;

    err = huffman_construct(&lenlen, lengths, 19);
    if (err != 0) return -1;   // must be a complete code

    int i = 0;
    while (i < nlen + ndist) {
        int symbol = huffman_decode(s, &lenlen);
        if (symbol < 0) return symbol;

        if (symbol < 16) {
            lengths[i++] = (short)symbol;
        } else {
            int len = 0, count;
            if (symbol == 16) {
                if (i == 0) return -1;
                len = lengths[i - 1];
                b = bits(s, 2); if (b < 0) return -1; count = 3 + b;
            } else if (symbol == 17) {
                b = bits(s, 3); if (b < 0) return -1; count = 3 + b;
            } else {
                b = bits(s, 7); if (b < 0) return -1; count = 11 + b;
            }
            if (i + count > nlen + ndist) return -1;
            while (count--) lengths[i++] = (short)len;
        }
    }

    if (lengths[256] == 0) return -1;   // no end-of-block code

    err = huffman_construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
        return -1;

    err = huffman_construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
        return -1;

    return inflate_codes(s, &lencode, &distcode);
}

static int inflate_blocks(InflateState* s) {
    int last = 0;
    while (!last) {
        int b = bits(s, 1); if (b < 0) return -1; last = b;
        b = bits(s, 2);     if (b < 0) return -1; int type = b;

        int err;
        if (type == 0)      err = inflate_stored(s);
        else if (type == 1) err = inflate_fixed(s);
        else if (type == 2) err = inflate_dynamic(s);
        else                return -1;

        if (err) return err;   // 1 (out full) is treated as done by caller
    }
    return 0;
}

// Returns 0 on success, non-zero on failure. `outlen` bytes are produced
// (the caller supplied the exact uncompressed size).
static int inflate_raw(const uint8_t* in, size_t inlen,
                       uint8_t* out, size_t outlen) {
    InflateState s;
    memset(&s, 0, sizeof(s));
    s.in = in; s.inlen = inlen;
    s.out = out; s.outlen = outlen;

    int err = inflate_blocks(&s);
    if (err == 1) err = 0;   // output buffer filled exactly: acceptable
    if (err) return -1;
    if (s.outcnt != outlen) return -1;
    return 0;
}

// =============================================================================
// ZIP container
// =============================================================================

typedef struct ZipEntry {
    char*    name;
    uint16_t flags;
    uint16_t method;
    uint32_t crc;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_off;
} ZipEntry;

struct EmuZip {
    const uint8_t* data;
    size_t         size;
    ZipEntry*      entries;
    int            count;
};

int emu_zip_is_zip(const uint8_t* data, size_t size) {
    if (!data || size < 4) return 0;
    if (data[0] != 'P' || data[1] != 'K') return 0;
    return (data[2] == 3 && data[3] == 4) ||   // local file header
           (data[2] == 5 && data[3] == 6) ||   // end of central directory
           (data[2] == 7 && data[3] == 8);     // spanned
}

EmuZip* emu_zip_open_memory(const uint8_t* data, size_t size) {
    if (!emu_zip_is_zip(data, size) || size < 22) return NULL;

    // Locate the end-of-central-directory record (search from the tail).
    size_t max_back = 22 + 65535;
    size_t start = size > max_back ? size - max_back : 0;
    size_t eocd = 0;
    int found = 0;
    for (size_t i = size - 22 + 1; i-- > start; ) {
        if (data[i] == 'P' && data[i + 1] == 'K' &&
            data[i + 2] == 5 && data[i + 3] == 6) {
            eocd = i; found = 1; break;
        }
    }
    if (!found) return NULL;

    const uint8_t* e = data + eocd;
    uint32_t cd_size = rd32(e + 12);
    uint32_t cd_off  = rd32(e + 16);
    if (cd_off > size || cd_size > size || (size_t)cd_off + cd_size > size)
        return NULL;

    EmuZip* z = (EmuZip*)calloc(1, sizeof(EmuZip));
    if (!z) return NULL;
    z->data = data;
    z->size = size;

    // Count entries first so we can allocate exactly.
    int total = 0;
    {
        size_t p = cd_off;
        while (p + 46 <= cd_off + cd_size) {
            const uint8_t* c = data + p;
            if (rd32(c) != 0x02014b50) break;
            uint16_t nlen = rd16(c + 28);
            uint16_t elen = rd16(c + 30);
            uint16_t clen = rd16(c + 32);
            total++;
            p += 46 + nlen + elen + clen;
        }
    }
    if (total <= 0) { free(z); return NULL; }

    z->entries = (ZipEntry*)calloc((size_t)total, sizeof(ZipEntry));
    if (!z->entries) { free(z); return NULL; }

    size_t p = cd_off;
    int n = 0;
    while (p + 46 <= cd_off + cd_size && n < total) {
        const uint8_t* c = data + p;
        if (rd32(c) != 0x02014b50) break;

        uint16_t nlen = rd16(c + 28);
        uint16_t elen = rd16(c + 30);
        uint16_t clen = rd16(c + 32);
        if (p + 46 + nlen + elen + clen > size) break;

        char* nm = (char*)malloc((size_t)nlen + 1);
        if (!nm) break;
        memcpy(nm, c + 46, nlen);
        nm[nlen] = 0;

        z->entries[n].name        = nm;
        z->entries[n].flags       = rd16(c + 8);
        z->entries[n].method      = rd16(c + 10);
        z->entries[n].crc         = rd32(c + 16);
        z->entries[n].comp_size   = rd32(c + 20);
        z->entries[n].uncomp_size = rd32(c + 24);
        z->entries[n].local_off   = rd32(c + 42);
        n++;
        p += 46 + nlen + elen + clen;
    }
    z->count = n;

    if (n == 0) { emu_zip_close(z); return NULL; }
    return z;
}

void emu_zip_close(EmuZip* z) {
    if (!z) return;
    if (z->entries) {
        for (int i = 0; i < z->count; i++) free(z->entries[i].name);
        free(z->entries);
    }
    free(z);
}

int emu_zip_count(const EmuZip* z) { return z ? z->count : 0; }

const char* emu_zip_name(const EmuZip* z, int index) {
    if (!z || index < 0 || index >= z->count) return NULL;
    return z->entries[index].name;
}

uint32_t emu_zip_size(const EmuZip* z, int index) {
    if (!z || index < 0 || index >= z->count) return 0;
    return z->entries[index].uncomp_size;
}

int emu_zip_is_dir(const EmuZip* z, int index) {
    if (!z || index < 0 || index >= z->count) return 0;
    const char* n = z->entries[index].name;
    size_t len = strlen(n);
    return len > 0 && (n[len - 1] == '/' || n[len - 1] == '\\');
}

uint8_t* emu_zip_read(const EmuZip* z, int index, size_t* out_size) {
    if (out_size) *out_size = 0;
    if (!z || index < 0 || index >= z->count) return NULL;

    const ZipEntry* e = &z->entries[index];
    if (e->local_off + 30 > z->size) return NULL;

    const uint8_t* lh = z->data + e->local_off;
    if (rd32(lh) != 0x04034b50) return NULL;

    uint16_t nlen = rd16(lh + 26);
    uint16_t elen = rd16(lh + 28);
    size_t data_off = (size_t)e->local_off + 30 + nlen + elen;
    if (data_off > z->size) return NULL;

    // ZIP64 / unsupported sizes.
    if (e->comp_size == 0xFFFFFFFFu || e->uncomp_size == 0xFFFFFFFFu) return NULL;

    uint8_t* out = NULL;
    size_t   produced = 0;

    if (e->method == 0) {                       // stored
        if (data_off + e->uncomp_size > z->size) return NULL;
        out = (uint8_t*)malloc(e->uncomp_size ? e->uncomp_size : 1);
        if (!out) return NULL;
        memcpy(out, z->data + data_off, e->uncomp_size);
        produced = e->uncomp_size;
    } else if (e->method == 8) {                // deflate
        if (data_off + e->comp_size > z->size) return NULL;
        out = (uint8_t*)malloc(e->uncomp_size ? e->uncomp_size : 1);
        if (!out) return NULL;
        if (inflate_raw(z->data + data_off, e->comp_size, out, e->uncomp_size) != 0) {
            free(out);
            return NULL;
        }
        produced = e->uncomp_size;
    } else {
        return NULL;                            // unsupported method
    }

    // Verify integrity against the central-directory CRC.
    if (emu_crc32(out, produced) != e->crc) {
        free(out);
        return NULL;
    }

    if (out_size) *out_size = produced;
    return out;
}

// -----------------------------------------------------------------------------
// Picking the ROM entry
// -----------------------------------------------------------------------------

static const char* just_basename(const char* p) {
    const char* base = p;
    for (const char* s = p; *s; s++)
        if (*s == '/' || *s == '\\') base = s + 1;
    return base;
}

static int contains_ci(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               ((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]) ==
               ((needle[i] >= 'A' && needle[i] <= 'Z') ? needle[i] + 32 : needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

static int is_ignorable_ext(const char* ext) {
    static const char* junk[] = {
        "txt", "nfo", "diz", "md", "json", "xml", "html", "htm",
        "jpg", "jpeg", "png", "gif", "bmp", "url", "ini", "cfg",
        "log", "sfv", "db", "pdf", "doc", "rtf", NULL
    };
    for (int i = 0; junk[i]; i++)
        if (emu_strcasecmp(ext, junk[i]) == 0) return 1;
    return 0;
}

int emu_zip_find_rom(const EmuZip* z) {
    if (!z || z->count <= 0) return -1;

    int best_rom = -1;  uint32_t best_rom_size = 0;
    int best_any = -1;  uint32_t best_any_size = 0;

    for (int i = 0; i < z->count; i++) {
        const char* name = z->entries[i].name;
        if (!name || !name[0]) continue;
        if (emu_zip_is_dir(z, i)) continue;

        const char* base = just_basename(name);
        if (!base[0] || base[0] == '.') continue;          // hidden / "."
        if (contains_ci(name, "__MACOSX")) continue;
        if (strncmp(base, "._", 2) == 0) continue;         // AppleDouble

        char ext[24];
        emu_get_extension(base, ext, sizeof(ext));

        int is_zip_entry = (ext[0] && emu_strcasecmp(ext, "zip") == 0);
        int is_supported = (ext[0] && emu_is_supported_extension(ext) && !is_zip_entry);

        if (is_supported || is_zip_entry) {
            if (z->entries[i].uncomp_size > best_rom_size) {
                best_rom = i;
                best_rom_size = z->entries[i].uncomp_size;
            }
            continue;
        }

        if (ext[0] && is_ignorable_ext(ext)) continue;

        // Unknown extension: keep as a last-resort candidate.
        if (z->entries[i].uncomp_size > best_any_size) {
            best_any = i;
            best_any_size = z->entries[i].uncomp_size;
        }
    }

    return best_rom >= 0 ? best_rom : best_any;
}

// -----------------------------------------------------------------------------
// Extract with limited nesting
// -----------------------------------------------------------------------------

#define EMU_ZIP_MAX_DEPTH 2

static int extract_rom_depth(const uint8_t* zipdata, size_t zipsize,
                             uint8_t** out_data, size_t* out_size,
                             char* out_name, size_t out_name_size,
                             int depth) {
    if (out_name && out_name_size) out_name[0] = 0;
    if (!emu_zip_is_zip(zipdata, zipsize)) return 0;

    EmuZip* z = emu_zip_open_memory(zipdata, zipsize);
    if (!z) return 0;

    int idx = emu_zip_find_rom(z);
    if (idx < 0) { emu_zip_close(z); return 0; }

    char name[520];
    const char* nm = emu_zip_name(z, idx);
    snprintf(name, sizeof(name), "%s", nm ? nm : "");
    const char* base = just_basename(name);

    size_t esize = 0;
    uint8_t* edata = emu_zip_read(z, idx, &esize);
    emu_zip_close(z);
    if (!edata) return 0;
    strncpy(name, base, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;

    // Descend into a nested archive.
    if (depth < EMU_ZIP_MAX_DEPTH && emu_zip_is_zip(edata, esize)) {
        uint8_t* nd = NULL; size_t ns = 0; char nn[520] = "";
        if (extract_rom_depth(edata, esize, &nd, &ns, nn, sizeof(nn), depth + 1)) {
            free(edata);
            edata = nd; esize = ns;
            if (nn[0]) { strncpy(name, nn, sizeof(name) - 1); name[sizeof(name) - 1] = 0; }
        }
    }

    *out_data = edata;
    *out_size = esize;
    if (out_name && out_name_size) {
        strncpy(out_name, name, out_name_size - 1);
        out_name[out_name_size - 1] = 0;
    }
    return 1;
}

int emu_zip_extract_rom(const uint8_t* zipdata, size_t zipsize,
                        uint8_t** out_data, size_t* out_size,
                        char* out_name, size_t out_name_size) {
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    return extract_rom_depth(zipdata, zipsize, out_data, out_size,
                             out_name, out_name_size, 0);
}

// -----------------------------------------------------------------------------
// Prepare a path for loading
// -----------------------------------------------------------------------------

static void sanitize(char* s) {
    for (char* p = s; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
            *p = '_';
    }
}

char* emu_prepare_rom(const char* path, const char* cache_dir,
                      char* out_display_name, size_t out_display_name_size) {
    if (!path) return NULL;

    char base[520];
    emu_get_basename(path, base, sizeof(base));
    if (out_display_name && out_display_name_size) {
        strncpy(out_display_name, base, out_display_name_size - 1);
        out_display_name[out_display_name_size - 1] = 0;
    }

    size_t fsize = 0;
    uint8_t* file = emu_read_file(path, &fsize, (size_t)512 * 1024 * 1024);
    if (!file) return dup_str(path);

    if (!emu_zip_is_zip(file, fsize)) {
        free(file);
        return dup_str(path);
    }

    uint8_t* rom = NULL;
    size_t   romsize = 0;
    char     inner[520] = "";
    int ok = emu_zip_extract_rom(file, fsize, &rom, &romsize, inner, sizeof(inner));
    free(file);

    if (!ok || !rom) return dup_str(path);   // let the caller try the raw file

    if (out_display_name && out_display_name_size && inner[0]) {
        strncpy(out_display_name, inner, out_display_name_size - 1);
        out_display_name[out_display_name_size - 1] = 0;
    }

    char safe[520];
    emu_get_basename(inner[0] ? inner : base, safe, sizeof(safe));
    sanitize(safe);
    if (!safe[0]) snprintf(safe, sizeof(safe), "rom.bin");

    uint32_t crc = emu_crc32(rom, romsize);

    char out[1400];
    snprintf(out, sizeof(out), "%s/%08X_%s",
             cache_dir ? cache_dir : ".", crc, safe);

    if (cache_dir) emu_mkdir_p(cache_dir);

    if (emu_write_file(out, rom, romsize) != 0) {
        free(rom);
        return dup_str(path);
    }
    free(rom);
    return dup_str(out);
}
