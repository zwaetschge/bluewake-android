// The game in German, French, Spanish or Italian, built when the disc loads.
//
// The translated game module comes from the USA disc (GZLE01), whose text is
// English only. The European disc (GZLP01) holds the same game's text,
// title art, place names and GBA data in four languages. With
// BLUEWAKE_LANGUAGE=de|fr|es|it and the player's European disc at
// BLUEWAKE_LANGUAGE_DISC (default <game>/GZLP01.iso), this builds the USA
// disc's language files from it in memory (about 2 MB) when the USA disc
// opens, and serves them in place of the English ones. Neither disc is
// changed or copied.
//
// How: GXRuntime's DVD layer (dvd.c) resolves a file to its disc offset and
// length (dvd_entry_info) and copies disc bytes into guest RAM
// (dvd_read_to_guest); every caller (hle_dvd.c, the host's DI service) goes
// through those two. The Android link wraps them (-Wl,--wrap, see
// android/CMakeLists.txt): a replaced file is given an offset past the end
// of the USA image, and reads there are served from memory -- the same
// layout as appending the files to the image, without writing it.
//
// What is replaced (scripts-local's build_lang_iso.py did this to an image;
// the rules are the ones verified there):
//   * whole files: bmgres.arc (the messages), msgres.arc, TlogoE.arc (the
//     title logo), the GBA Tingle Tuner data, the place-name cards;
//   * inside USA archives, by name, keeping the USA entry tables (the code
//     takes some entries by index): itemres/saveres/itemicon's word images
//     and every action word in acticon.arc (PAL names them ba_*_N.bti and
//     has one more entry; swapping the archive whole drew the wrong one);
//   * the file select (Stage/Name/Stage.arc -> dat/file_select.arc, which the
//     European disc lays out differently): the USA layout with its button
//     words translated, Choose/Return from acticon, and drawn save-slot
//     labels (language_textures.h).
// nameres.arc is left English: the European name-entry layout has other
// panes and the USA code calls through a missing one (a crash on leaving
// the title screen). "New Game" on an empty slot is a string in main.dol.
//
// Anything wrong with the European disc (missing, another game, a file not
// where expected) leaves the game in English and says so in the log.
#include "language_overlay.h"
#include "language_textures.h"

#include "gxruntime/dvd.h"
#include "gxruntime/guest_memory_dirty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <zlib.h>

typedef struct {
    u8* data;
    u32 size;
} Buf;

static void buf_free(Buf* b) {
    free(b->data);
    b->data = NULL;
    b->size = 0;
}

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static u16 be16(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static void put32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static u32 align32(u32 v) { return (v + 31u) & ~31u; }

static char g_error[256];
static bool fail(const char* fmt, const char* what) {
    snprintf(g_error, sizeof g_error, fmt, what);
    return false;
}

// ---------------------------------------------------------------------------
// A GameCube disc image: its FST, and files read whole.
// ---------------------------------------------------------------------------
typedef struct {
    FILE* f;
    u8* fst;
    u32 fst_size;
    u32 count;
    const char* strings;
    u64 image_size;
} Disc;

static void disc_close(Disc* d) {
    if (d->f) fclose(d->f);
    free(d->fst);
    memset(d, 0, sizeof *d);
}

static bool disc_open(Disc* d, const char* path, const char* game_id) {
    memset(d, 0, sizeof *d);
    d->f = fopen(path, "rb");
    if (!d->f) return fail("cannot open %s", path);
    u8 head[0x440];
    if (fread(head, 1, sizeof head, d->f) != sizeof head || be32(head + 0x1C) != 0xC2339F3Du)
        return fail("%s is not a GameCube disc image", path);
    if (memcmp(head, game_id, 6) != 0)
        return fail("%s is not the expected disc", path);
    const u32 fst_off = be32(head + 0x424), fst_size = be32(head + 0x428);
    if (fst_size < 12u || fst_size > 0x400000u) return fail("%s: bad FST", path);
    d->fst = (u8*)malloc(fst_size);
    if (!d->fst || fseeko(d->f, (off_t)fst_off, SEEK_SET) != 0 ||
        fread(d->fst, 1, fst_size, d->f) != fst_size)
        return fail("%s: cannot read the FST", path);
    d->fst_size = fst_size;
    d->count = be32(d->fst + 8);
    if ((u64)d->count * 12u > fst_size) return fail("%s: bad FST", path);
    d->strings = (const char*)d->fst + (size_t)d->count * 12u;
    fseeko(d->f, 0, SEEK_END);
    d->image_size = (u64)ftello(d->f);
    return true;
}

static bool is_dir(const Disc* d, u32 i) { return d->fst[i * 12u] != 0; }
static u32 next_of(const Disc* d, u32 i) { return be32(d->fst + i * 12u + 8); }

// A path from the root, as DVDConvertPathToEntrynum resolves it (any case).
static s32 disc_find(const Disc* d, const char* path) {
    u32 dir = 0;
    const char* p = path[0] == '/' ? path + 1 : path;
    for (;;) {
        const char* slash = strchr(p, '/');
        const size_t len = slash ? (size_t)(slash - p) : strlen(p);
        const bool want_dir = slash != NULL;
        u32 i = dir + 1;
        bool found = false;
        while (i < next_of(d, dir) && i < d->count) {
            const u32 name_off = be32(d->fst + i * 12u) & 0x00FFFFFFu;
            const char* name = d->strings + name_off;
            if ((u8*)name < d->fst + d->fst_size && strlen(name) == len &&
                strncasecmp(name, p, len) == 0 && is_dir(d, i) == want_dir) {
                found = true;
                break;
            }
            i = is_dir(d, i) ? next_of(d, i) : i + 1;
        }
        if (!found) return -1;
        if (!want_dir) return (s32)i;
        dir = i;
        p = slash + 1;
    }
}

static bool disc_read(const Disc* d, const char* path, Buf* out) {
    const s32 i = disc_find(d, path);
    if (i < 0) return fail("the European disc has no %s", path);
    const u32 off = be32(d->fst + (u32)i * 12u + 4), size = be32(d->fst + (u32)i * 12u + 8);
    out->data = (u8*)malloc(size ? size : 1u);
    out->size = size;
    if (!out->data || fseeko(d->f, (off_t)off, SEEK_SET) != 0 || fread(out->data, 1, size, d->f) != size) {
        buf_free(out);
        return fail("cannot read %s", path);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Yaz0 and RARC
// ---------------------------------------------------------------------------
static bool yaz0_decode(const u8* in, u32 in_size, Buf* out) {
    if (in_size < 16u || memcmp(in, "Yaz0", 4) != 0) return false;
    const u32 total = be32(in + 4);
    out->data = (u8*)malloc(total ? total : 1u);
    out->size = total;
    if (!out->data) return false;
    u32 src = 16u, dst = 0u;
    while (dst < total) {
        if (src >= in_size) goto bad;
        u8 bits = in[src++];
        for (int k = 0; k < 8 && dst < total; ++k, bits <<= 1) {
            if (bits & 0x80u) {
                if (src >= in_size) goto bad;
                out->data[dst++] = in[src++];
                continue;
            }
            if (src + 1u >= in_size) goto bad;
            const u8 b0 = in[src], b1 = in[src + 1];
            src += 2u;
            const u32 back = (((u32)(b0 & 0x0Fu) << 8) | b1) + 1u;
            u32 count = b0 >> 4;
            if (count == 0u) {
                if (src >= in_size) goto bad;
                count = (u32)in[src++] + 0x12u;
            } else {
                count += 2u;
            }
            if (back > dst) goto bad;
            for (u32 c = 0; c < count && dst < total; ++c, ++dst)
                out->data[dst] = out->data[dst - back];
        }
    }
    return true;
bad:
    buf_free(out);
    return false;
}

typedef struct {
    char name[128];   // "<node>/<file>", the node's name dropped for ROOT
    u32 entry;        // index in the entry table
    u32 off, size;    // absolute offset in the archive, and length
} RarcFile;

typedef struct {
    const u8* data;
    u32 size;
    u32 data_start, entries_off;
    RarcFile* files;
    u32 count;
} Rarc;

static void rarc_free(Rarc* r) {
    free(r->files);
    memset(r, 0, sizeof *r);
}

static bool rarc_open(Rarc* r, const u8* data, u32 size) {
    memset(r, 0, sizeof *r);
    if (size < 0x40u || memcmp(data, "RARC", 4) != 0) return false;
    const u32 hs = be32(data + 8);
    r->data = data;
    r->size = size;
    r->data_start = hs + be32(data + 12);
    const u32 node_count = be32(data + 0x20), nodes_off = be32(data + 0x24) + hs;
    const u32 entry_count = be32(data + 0x28);
    r->entries_off = be32(data + 0x2C) + hs;
    const u32 str_size = be32(data + 0x30), str_off = be32(data + 0x34) + hs;
    if (nodes_off + node_count * 16u > size || r->entries_off + entry_count * 20u > size ||
        str_off + str_size > size || r->data_start > size)
        return false;
    const char* strings = (const char*)data + str_off;
    r->files = (RarcFile*)calloc(entry_count ? entry_count : 1u, sizeof(RarcFile));
    if (!r->files) return false;
    for (u32 n = 0; n < node_count; ++n) {
        const u8* node = data + nodes_off + n * 16u;
        const u32 node_name = be32(node + 4);
        const u32 count = be16(node + 10), first = be32(node + 12);
        if (node_name >= str_size || first + count > entry_count) return false;
        const char* nname = strings + node_name;
        for (u32 e = first; e < first + count; ++e) {
            const u8* en = data + r->entries_off + e * 20u;
            const u16 flag = be16(en + 4), name_off = be16(en + 6);
            if (name_off >= str_size) return false;
            const char* ename = strings + name_off;
            if ((flag & 0x0200u) || strcmp(ename, ".") == 0 || strcmp(ename, "..") == 0)
                continue;
            RarcFile* f = &r->files[r->count++];
            if (strcmp(nname, "ROOT") == 0)
                snprintf(f->name, sizeof f->name, "%s", ename);
            else
                snprintf(f->name, sizeof f->name, "%s/%s", nname, ename);
            f->entry = e;
            f->off = r->data_start + be32(en + 8);
            f->size = be32(en + 12);
            if ((u64)f->off + f->size > size) return false;
        }
    }
    return true;
}

static const RarcFile* rarc_get(const Rarc* r, const char* name) {
    for (u32 i = 0; i < r->count; ++i)
        if (strcmp(r->files[i].name, name) == 0) return &r->files[i];
    return NULL;
}

static int by_offset(const void* a, const void* b) {
    const u32 x = ((const RarcFile*)a)->off, y = ((const RarcFile*)b)->off;
    return x < y ? -1 : x > y;
}

typedef struct {
    const char* name;
    Buf content;
} Swap;

// The archive with the same tables and some files' contents replaced: every
// file starts on a 32-byte boundary, as in the originals (an unaligned
// archive runs but draws nothing).
static bool rarc_repack(const Buf* orig, const Swap* swaps, int n, Buf* out) {
    Rarc r;
    if (!rarc_open(&r, orig->data, orig->size)) {
        rarc_free(&r);
        return fail("%s", "an archive did not parse");
    }
    for (int s = 0; s < n; ++s)
        if (!rarc_get(&r, swaps[s].name)) {
            rarc_free(&r);
            return fail("an archive lacks %s", swaps[s].name);
        }
    RarcFile* order = (RarcFile*)malloc((r.count ? r.count : 1u) * sizeof(RarcFile));
    memcpy(order, r.files, r.count * sizeof(RarcFile));
    qsort(order, r.count, sizeof(RarcFile), by_offset);
    u64 total = r.data_start;
    for (u32 i = 0; i < r.count; ++i) {
        u32 size = order[i].size;
        for (int s = 0; s < n; ++s)
            if (strcmp(swaps[s].name, order[i].name) == 0) size = swaps[s].content.size;
        total = r.data_start + align32((u32)(total - r.data_start)) + size;
    }
    total = r.data_start + align32((u32)(total - r.data_start));
    out->data = (u8*)calloc((size_t)total, 1);
    out->size = (u32)total;
    memcpy(out->data, orig->data, r.data_start);
    u32 at = 0;
    for (u32 i = 0; i < r.count; ++i) {
        const u8* src = orig->data + order[i].off;
        u32 size = order[i].size;
        for (int s = 0; s < n; ++s)
            if (strcmp(swaps[s].name, order[i].name) == 0) {
                src = swaps[s].content.data;
                size = swaps[s].content.size;
            }
        at = align32(at);
        put32(out->data + r.entries_off + order[i].entry * 20u + 8u, at);
        put32(out->data + r.entries_off + order[i].entry * 20u + 12u, size);
        memcpy(out->data + r.data_start + at, src, size);
        at += size;
    }
    const u32 data_size = align32(at);
    put32(out->data + 4, out->size);
    put32(out->data + 0x10, data_size);
    put32(out->data + 0x14, data_size);
    free(order);
    rarc_free(&r);
    return true;
}

// A file of an archive (Yaz0-compressed archives and files opened).
static bool arc_file(const Buf* arc, const char* name, Buf* out) {
    Buf plain = {0};
    const Buf* a = arc;
    if (arc->size >= 4u && memcmp(arc->data, "Yaz0", 4) == 0) {
        if (!yaz0_decode(arc->data, arc->size, &plain)) return fail("%s", "a compressed archive");
        a = &plain;
    }
    Rarc r;
    const RarcFile* f = NULL;
    bool ok = rarc_open(&r, a->data, a->size) && (f = rarc_get(&r, name)) != NULL;
    if (ok) {
        out->data = (u8*)malloc(f->size ? f->size : 1u);
        out->size = f->size;
        memcpy(out->data, a->data + f->off, f->size);
    }
    rarc_free(&r);
    buf_free(&plain);
    return ok ? true : fail("an archive lacks %s", name);
}

static bool unyaz(Buf* b) {
    if (b->size < 4u || memcmp(b->data, "Yaz0", 4) != 0) return true;
    Buf plain;
    if (!yaz0_decode(b->data, b->size, &plain)) return false;
    buf_free(b);
    *b = plain;
    return true;
}

// ---------------------------------------------------------------------------
// The file select's layout: text panes' strings by pane tag
// ---------------------------------------------------------------------------
typedef struct {
    const char tag[4];
    const char* english;
} Pane;
static const Pane kPanes[6] = {
    {{'s', 't', 'a', 't'}, "Start"}, {{'c', 'o', 'p', 'y'}, "Copy"}, {{'d', 'l', 'l', 'e'}, "Erase"},
    {{'r', 'e', 't', 'u'}, "Return"}, {{0, 'y', 'e', 's'}, "Yes"}, {{0, 0, 'n', 'o'}, "No"}};
// Windows-1252, the font's code page (it draws 0x20-0xFF); each fits the
// 110-pixel button at the font's 24 pixels.
static const char* const kWords[4][6] = {
    {"Start", "Kopieren", "L\xF6" "schen", "Zur\xFC" "ck", "Ja", "Nein"},
    {"Jouer", "Copier", "Effacer", "Retour", "Oui", "Non"},
    {"Jugar", "Copiar", "Borrar", "Volver", "S\xED", "No"},
    {"Gioca", "Copia", "Cancella", "Indietro", "S\xEC", "No"},
};

static bool retext_blo(const Buf* blo, int lang, Buf* out) {
    // A TBX1 body ends: the string's u16 length, the string, 9 bytes of
    // extended colours, zero padding to 4. Block and file sizes follow.
    out->data = (u8*)malloc(blo->size + 256u);
    memcpy(out->data, blo->data, 0x20);
    u32 o = 0x20, p = 0x20;
    int done = 0;
    while (p + 8u <= blo->size) {
        const u32 size = be32(blo->data + p + 4);
        if (size == 0u || p + size > blo->size) break;
        const u8* block = blo->data + p;
        int pane = -1;
        if (memcmp(block, "TBX1", 4) == 0 && size >= 16u)
            for (int k = 0; k < 6; ++k)
                if (memcmp(block + 12, kPanes[k].tag, 4) == 0) pane = k;
        if (pane < 0) {
            memcpy(out->data + o, block, size);
            o += size;
        } else {
            const u32 elen = (u32)strlen(kPanes[pane].english);
            u32 at = 16u;
            while (at + 2u + elen <= size &&
                   !(be16(block + at) == elen && memcmp(block + at + 2, kPanes[pane].english, elen) == 0))
                ++at;
            if (at + 2u + elen + 9u > size) {
                buf_free(out);
                return fail("the file select layout's %s pane changed", kPanes[pane].english);
            }
            const char* word = kWords[lang][pane];
            const u32 wlen = (u32)strlen(word);
            u8* b = out->data + o;
            memcpy(b, block, at);
            b[at] = (u8)(wlen >> 8);
            b[at + 1] = (u8)wlen;
            memcpy(b + at + 2, word, wlen);
            memcpy(b + at + 2 + wlen, block + at + 2 + elen, 9);
            u32 nsize = at + 2u + wlen + 9u;
            while (nsize % 4u) b[nsize++] = 0;
            put32(b + 4, nsize);
            o += nsize;
            ++done;
        }
        p += size;
    }
    memcpy(out->data + o, blo->data + p, blo->size - p);
    o += blo->size - p;
    out->size = o;
    put32(out->data + 8, o);
    if (done != 6) {
        buf_free(out);
        return fail("%s", "the file select layout lacks a button");
    }
    return true;
}

// ---------------------------------------------------------------------------
// The overlay
// ---------------------------------------------------------------------------
typedef struct {
    s32 entry;      // in the USA disc's FST
    u32 start;      // the offset it is served at, past the USA image
    Buf data;
} Item;

static Item g_items[40];
static int g_count;
static u32 g_base;     // reads at or past this offset come from g_items
static bool g_active;
static char g_status[320];

static bool add_item(const Disc* usa, const char* path, Buf* data, u32* cursor) {
    const s32 e = disc_find(usa, path);
    if (e < 0 || g_count >= (int)(sizeof g_items / sizeof g_items[0]))
        return fail("the USA disc has no %s", path);
    g_items[g_count].entry = e;
    g_items[g_count].start = *cursor;
    g_items[g_count].data = *data;
    *cursor = align32(*cursor + data->size);
    data->data = NULL;
    ++g_count;
    return true;
}

static const char kLangCodes[4][3] = {"de", "fr", "es", "it"};
static const char* const kSaveSuffix[4] = {"gm", "fr", "sp", "it"};

static bool build(const char* usa_path, const char* pal_path, int lang) {
    const int L = lang + 1;   // the European disc numbers its languages 1-4
    char a[160], b[160];
    Disc usa = {0}, pal = {0};
    Buf buf = {0}, arc = {0}, src = {0};
    bool ok = pal_path != NULL && pal_path[0] != '\0' ? true : fail("%s", "no European disc set");
    ok = ok && disc_open(&usa, usa_path, "GZLE01") && disc_open(&pal, pal_path, "GZLP01");
    u32 cursor = 0;
    if (ok) {
        if (usa.image_size > 0xF0000000ull) ok = fail("%s", "the USA image is too large");
        g_base = align32((u32)usa.image_size) + 0x8000u;
        cursor = g_base;
    }

    // Whole files.
    struct { const char* usa; char pal[96]; } whole[6 + 18];
    int nw = 0;
    whole[nw].usa = "/res/Msg/bmgres.arc"; snprintf(whole[nw++].pal, 96, "/res/Msg/data%d/bmgres.arc", L);
    whole[nw].usa = "/res/Msg/msgres.arc"; snprintf(whole[nw++].pal, 96, "/res/Msg/msgres.arc");
    whole[nw].usa = "/res/Gba/client_u.bin"; snprintf(whole[nw++].pal, 96, "/res/Gba/client_%d.bin", L);
    whole[nw].usa = "/res/Gba/client_ud.bin"; snprintf(whole[nw++].pal, 96, "/res/Gba/client_%dd.bin", L);
    whole[nw].usa = "/res/Gba/msg_LZ.bin"; snprintf(whole[nw++].pal, 96, "/res/Gba/msg_LZ%d.bin", L);
    whole[nw].usa = "/res/Object/TlogoE.arc"; snprintf(whole[nw++].pal, 96, "/res/Object/TlogoE%d.arc", L);
    static char pn_usa[18][32];
    for (int xx = 2; xx < 20; ++xx) {
        snprintf(pn_usa[xx - 2], 32, "/res/placename/pn_%02d.bti", xx);
        whole[nw].usa = pn_usa[xx - 2];
        snprintf(whole[nw++].pal, 96, "/res/placename/PN%d/pn_%02d_%d.bti", L, xx, L);
    }
    for (int i = 0; ok && i < nw; ++i)
        ok = disc_read(&pal, whole[i].pal, &buf) && add_item(&usa, whole[i].usa, &buf, &cursor);

    // Word images inside the USA archives, by name.
    struct { const char* usa_arc; char pal_arc[64]; const char* names[2]; char pal_names[2][64]; int n; } in_arc[3];
    in_arc[0] = (typeof(in_arc[0])){"/res/Msg/itemres.arc", "/res/Msg/itemres.arc",
                                    {"timg/title_item.bti", "timg/word_save2.bti"}, {"", ""}, 2};
    snprintf(in_arc[0].pal_names[0], 64, "timg/title_item_%d.bti", L);
    snprintf(in_arc[0].pal_names[1], 64, "timg/word_save2_%d.bti", L);
    in_arc[1] = (typeof(in_arc[1])){"/res/Msg/saveres.arc", "/res/Msg/saveres.arc",
                                    {"timg/title_save.bti", NULL}, {"", ""}, 1};
    snprintf(in_arc[1].pal_names[0], 64, "timg/title_save_%s.bti", kSaveSuffix[lang]);
    in_arc[2] = (typeof(in_arc[2])){"/res/Msg/itemicon.arc", "/res/Msg/itemicon.arc",
                                    {"timg/cover_return.bti", NULL}, {"", ""}, 1};
    snprintf(in_arc[2].pal_names[0], 64, "timg/cover_return_%d.bti", L);
    for (int i = 0; ok && i < 3; ++i) {
        Swap swaps[2] = {{0}};
        ok = disc_read(&usa, in_arc[i].usa_arc, &arc) && disc_read(&pal, in_arc[i].pal_arc, &src);
        for (int k = 0; ok && k < in_arc[i].n; ++k) {
            swaps[k].name = in_arc[i].names[k];
            ok = arc_file(&src, in_arc[i].pal_names[k], &swaps[k].content);
        }
        ok = ok && rarc_repack(&arc, swaps, in_arc[i].n, &buf) && add_item(&usa, in_arc[i].usa_arc, &buf, &cursor);
        for (int k = 0; k < 2; ++k) buf_free(&swaps[k].content);
        buf_free(&arc);
        buf_free(&src);
    }

    // Every action word, under its USA name.
    Buf acticon = {0};
    if (ok) {
        snprintf(a, sizeof a, "/res/Msg/data%d/acticon.arc", L);
        ok = disc_read(&usa, "/res/Msg/acticon.arc", &arc) && disc_read(&pal, a, &src);
    }
    if (ok) {
        Rarc r;
        ok = rarc_open(&r, arc.data, arc.size);
        Swap* swaps = (Swap*)calloc(r.count ? r.count : 1u, sizeof(Swap));
        for (u32 k = 0; ok && k < r.count; ++k) {
            const size_t len = strlen(r.files[k].name);
            if (len < 4 || strcmp(r.files[k].name + len - 4, ".bti") != 0) continue;
            snprintf(b, sizeof b, "%.*s_%d.bti", (int)(len - 4), r.files[k].name, L);
            swaps[k].name = r.files[k].name;
            ok = arc_file(&src, b, &swaps[k].content);
        }
        int n = 0;
        for (u32 k = 0; k < r.count; ++k)
            if (swaps[k].name) swaps[n++] = swaps[k];
        ok = ok && rarc_repack(&arc, swaps, n, &acticon);
        for (int k = 0; k < n; ++k) buf_free(&swaps[k].content);
        free(swaps);
        rarc_free(&r);
        buf_free(&arc);
        buf_free(&src);
        if (ok) {
            buf.data = (u8*)malloc(acticon.size);
            buf.size = acticon.size;
            memcpy(buf.data, acticon.data, acticon.size);
            ok = add_item(&usa, "/res/Msg/acticon.arc", &buf, &cursor);
        }
    }

    // The file select: the USA layout, translated.
    if (ok) {
        Buf stage = {0}, fsel = {0}, blo = {0}, nblo = {0}, newfs = {0}, raw = {0};
        Swap swaps[6] = {{0}};
        ok = disc_read(&usa, "/res/Stage/Name/Stage.arc", &stage) && arc_file(&stage, "dat/file_select.arc", &fsel) &&
             arc_file(&fsel, "scrn/file_select.blo", &blo) && retext_blo(&blo, lang, &nblo);
        swaps[0].name = "scrn/file_select.blo";
        swaps[0].content = nblo;
        static const char* const kButtons[2] = {"timg/ba_kettei.bti", "timg/ba_modoru.bti"};
        for (int k = 0; ok && k < 2; ++k) {
            Buf usa_tex = {0};
            swaps[1 + k].name = kButtons[k];
            ok = arc_file(&acticon, kButtons[k], &swaps[1 + k].content) && unyaz(&swaps[1 + k].content) &&
                 arc_file(&fsel, kButtons[k], &usa_tex) && swaps[1 + k].content.size >= 0x20u &&
                 memcmp(usa_tex.data, swaps[1 + k].content.data, 0x20) == 0;
            if (!ok) fail("%s does not match the file select's", kButtons[k]);
            buf_free(&usa_tex);
        }
        if (ok) {
            raw.data = (u8*)malloc(BW_LANG_SLOT_TEX_RAW);
            raw.size = BW_LANG_SLOT_TEX_RAW;
            uLongf got = BW_LANG_SLOT_TEX_RAW;
            ok = uncompress(raw.data, &got, kBwLangSlotTextures, sizeof kBwLangSlotTextures) == Z_OK &&
                 got == BW_LANG_SLOT_TEX_RAW;
        }
        static const char* const kSlots[3] = {"timg/file_data_01.bti", "timg/file_data_02.bti",
                                              "timg/file_data_03.bti"};
        const u32 tex = BW_LANG_SLOT_TEX_W * BW_LANG_SLOT_TEX_H;
        for (int k = 0; ok && k < 3; ++k) {
            Buf usa_tex = {0};
            ok = arc_file(&fsel, kSlots[k], &usa_tex) && usa_tex.size == 0x20u + tex && usa_tex.data[0] == 2u &&
                 be32(usa_tex.data + 0x1C) == 0x20u;
            if (ok) {
                memcpy(usa_tex.data + 0x20, raw.data + ((u32)lang * 3u + (u32)k) * tex, tex);
                swaps[3 + k].name = kSlots[k];
                swaps[3 + k].content = usa_tex;
            } else {
                fail("%s is not the expected texture", kSlots[k]);
                buf_free(&usa_tex);
            }
        }
        ok = ok && rarc_repack(&fsel, swaps, 6, &newfs);
        Swap stage_swap = {"dat/file_select.arc", newfs};
        ok = ok && rarc_repack(&stage, &stage_swap, 1, &buf) && add_item(&usa, "/res/Stage/Name/Stage.arc", &buf, &cursor);
        for (int k = 0; k < 6; ++k) buf_free(&swaps[k].content);
        buf_free(&stage); buf_free(&fsel); buf_free(&blo); buf_free(&newfs); buf_free(&raw);
    }
    buf_free(&acticon);
    buf_free(&buf);
    disc_close(&usa);
    disc_close(&pal);
    return ok;
}

static void clear(void) {
    for (int i = 0; i < g_count; ++i) buf_free(&g_items[i].data);
    g_count = 0;
    g_active = false;
}

bool bw_lang_overlay_build(const char* usa_path, const char* pal_path, const char* language) {
    clear();
    g_error[0] = '\0';
    int lang = -1;
    for (int i = 0; i < 4 && language; ++i)
        if (strcasecmp(language, kLangCodes[i]) == 0) lang = i;
    if (lang < 0) {
        snprintf(g_status, sizeof g_status, "English");
        return false;
    }
    if (!build(usa_path, pal_path, lang)) {
        clear();
        snprintf(g_status, sizeof g_status, "English (%s)", g_error);
        fprintf(stderr, "[lang] %s: %s; the game stays in English\n", kLangCodes[lang], g_error);
        return false;
    }
    g_active = true;
    u32 bytes = 0;
    for (int i = 0; i < g_count; ++i) bytes += g_items[i].data.size;
    snprintf(g_status, sizeof g_status, "%s, from %s", kLangCodes[lang], pal_path);
    fprintf(stderr, "[lang] %s: %d files (%u bytes) from %s\n", kLangCodes[lang], g_count, bytes, pal_path);
    return true;
}

bool bw_lang_overlay_file(s32 entry, const u8** data, u32* size, u32* start) {
    for (int i = 0; g_active && i < g_count; ++i)
        if (g_items[i].entry == entry) {
            if (data) *data = g_items[i].data.data;
            if (size) *size = g_items[i].data.size;
            if (start) *start = g_items[i].start;
            return true;
        }
    return false;
}

const char* bw_lang_overlay_status(void) {
    return g_status[0] ? g_status : "English";
}

#ifndef BW_LANG_OVERLAY_NO_WRAP
// ---------------------------------------------------------------------------
// The DVD layer's three entry points, wrapped (-Wl,--wrap=...)
// ---------------------------------------------------------------------------
bool __real_dvd_open_image(const char* path);
bool __real_dvd_entry_info(s32 entrynum, u32* start, u32* length);
void __real_dvd_read_to_guest(CPUState* cpu, u32 guest_addr, u32 disc_off, u32 length);

bool __wrap_dvd_open_image(const char* path) {
    const bool was_ready = dvd_image_ready();
    const bool ok = __real_dvd_open_image(path);
    if (ok && !was_ready) {
        const char* language = getenv("BLUEWAKE_LANGUAGE");
        if (language != NULL && language[0] != '\0' && strcasecmp(language, "en") != 0)
            bw_lang_overlay_build(path, getenv("BLUEWAKE_LANGUAGE_DISC"), language);
    }
    return ok;
}

bool __wrap_dvd_entry_info(s32 entrynum, u32* start, u32* length) {
    if (!__real_dvd_entry_info(entrynum, start, length))
        return false;
    u32 size, at;
    if (bw_lang_overlay_file(entrynum, NULL, &size, &at)) {
        if (start) *start = at;
        if (length) *length = size;
    }
    return true;
}

void __wrap_dvd_read_to_guest(CPUState* cpu, u32 guest_addr, u32 disc_off, u32 length) {
    if (!g_active || disc_off < g_base || length == 0u) {
        __real_dvd_read_to_guest(cpu, guest_addr, disc_off, length);
        return;
    }
    const u8* src = NULL;
    u32 avail = 0;
    for (int i = 0; i < g_count; ++i) {
        const Item* it = &g_items[i];
        if (disc_off >= it->start && disc_off < it->start + it->data.size) {
            src = it->data.data + (disc_off - it->start);
            avail = it->start + it->data.size - disc_off;
            break;
        }
    }
    const u32 copy = src ? (avail < length ? avail : length) : 0u;
    // As dvd.c: straight into MEM1 when the whole target is there.
    u8* dst = NULL;
    if (guest_addr >= GC_RAM_BASE && (u64)guest_addr + length <= (u64)GC_RAM_BASE + cpu->ram_size)
        dst = cpu->ram + (guest_addr - GC_RAM_BASE);
    else if (guest_addr >= GC_RAM_UNCACHED && (u64)guest_addr + length <= (u64)GC_RAM_UNCACHED + cpu->ram_size)
        dst = cpu->ram + (guest_addr - GC_RAM_UNCACHED);
    if (dst) {
        if (copy) memcpy(dst, src, copy);
        if (copy < length) memset(dst + copy, 0, length - copy);
        dol_guest_memory_dirty_mark(guest_addr, length);
        return;
    }
    for (u32 k = 0; k < length; ++k)
        mem_write8(cpu, guest_addr + k, k < copy ? src[k] : 0u);
}
#endif
