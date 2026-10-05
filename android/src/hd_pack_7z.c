// Unpacks a Dolphin texture pack's GZL folder from a 7z archive, streaming:
// Hypatia's pack is one solid LZMA2 block of 531 MB, which the LZMA SDK's
// own extractor (SzArEx_Extract) would hold in memory whole; decoded here in
// 1 MB pieces, it needs the dictionary (128 MB) and little else. Every
// file's CRC is checked. Only folders whose single coder is LZMA2 or LZMA
// are read (what 7-Zip writes by default); anything else is refused.
//
// The LZMA SDK (C/, public domain) is fetched by android/CMakeLists.txt.
#include "hd_pack_7z.h"

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "Lzma2Dec.h"
#include "LzmaDec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static const ISzAlloc kAlloc = {SzAlloc, SzFree};
static const ISzAlloc kAllocTemp = {SzAllocTemp, SzFreeTemp};

static int set_error(char* error, size_t n, const char* fmt, const char* what) {
    if (error && n) snprintf(error, n, fmt, what);
    return -1;
}

static void mkdirs(char* path) {   // every parent of path
    for (char* p = path + 1; *p; ++p)
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0775);
            *p = '/';
        }
}

// UTF-16 file name -> UTF-8 path under "GZL/", or 0 when the file is not part
// of the GZL folder (readme, previews, the optional textures, the widescreen
// script). The folder may sit under a top folder ("Hypatia ... /GZL/...").
static int gzl_path(const UInt16* name, char* out, size_t n) {
    char utf8[1024];
    size_t o = 0;
    for (size_t i = 0; name[i] && o + 4 < sizeof utf8; ++i) {
        unsigned c = name[i];
        if (c >= 0xD800 && c < 0xDC00 && name[i + 1] >= 0xDC00 && name[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (name[++i] - 0xDC00);
        if (c == '\\') c = '/';
        if (c < 0x80) utf8[o++] = (char)c;
        else if (c < 0x800) { utf8[o++] = (char)(0xC0 | c >> 6); utf8[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) {
            utf8[o++] = (char)(0xE0 | c >> 12); utf8[o++] = (char)(0x80 | (c >> 6 & 63));
            utf8[o++] = (char)(0x80 | (c & 63));
        } else {
            utf8[o++] = (char)(0xF0 | c >> 18); utf8[o++] = (char)(0x80 | (c >> 12 & 63));
            utf8[o++] = (char)(0x80 | (c >> 6 & 63)); utf8[o++] = (char)(0x80 | (c & 63));
        }
    }
    utf8[o] = '\0';
    const char* gzl = strncmp(utf8, "GZL/", 4) == 0 ? utf8 : strstr(utf8, "/GZL/");
    if (!gzl) return 0;
    if (gzl != utf8) {
        ++gzl;
        // only the pack's own GZL, one folder deep at most, not an optional extra's
        if (memchr(utf8, '/', (size_t)(gzl - 1 - utf8)) != NULL) return 0;
    }
    if (strstr(gzl, "/../") || strstr(gzl, "/./")) return 0;
    if (strlen(gzl) + 1 > n) return 0;
    memcpy(out, gzl, strlen(gzl) + 1);
    return 1;
}

typedef struct {
    UInt32 file;          // index in the archive
    UInt64 size;
    char path[1200];      // "" when not extracted
} Item;

int bw_hd_pack_extract_gzl(const char* archive, const char* dest_dir, BwExtractProgress progress, void* user,
                           char* error, size_t error_size) {
    CFileInStream in_stream;
    CLookToRead2 look;
    CSzArEx db;
    FILE* data = NULL;
    Byte* in_buf = NULL;
    Byte* out_buf = NULL;
    Item* items = NULL;
    UInt16* name = NULL;
    int written = 0, result = -1;
    const size_t kIn = 1u << 20, kOut = 1u << 20;

    if (InFile_Open(&in_stream.file, archive) != 0) return set_error(error, error_size, "cannot open %s", archive);
    FileInStream_CreateVTable(&in_stream);
    LookToRead2_CreateVTable(&look, 0);
    look.buf = (Byte*)malloc(1u << 18);
    look.bufSize = 1u << 18;
    look.realStream = &in_stream.vt;
    LookToRead2_INIT(&look);
    CrcGenerateTable();
    SzArEx_Init(&db);
    if (!look.buf || SzArEx_Open(&db, &look.vt, &kAlloc, &kAllocTemp) != SZ_OK) {
        set_error(error, error_size, "%s is not a readable 7z archive", archive);
        goto done;
    }
    data = fopen(archive, "rb");
    in_buf = (Byte*)malloc(kIn);
    out_buf = (Byte*)malloc(kOut);
    items = (Item*)calloc(db.NumFiles ? db.NumFiles : 1, sizeof(Item));
    if (!data || !in_buf || !out_buf || !items) {
        set_error(error, error_size, "%s", "out of memory");
        goto done;
    }

    UInt64 total = 0, decoded = 0;
    for (UInt32 f = 0; f < db.db.NumFolders; ++f) total += SzAr_GetFolderUnpackSize(&db.db, f);

    for (UInt32 f = 0; f < db.db.NumFolders; ++f) {
        // The files of this folder, in stream order.
        int count = 0;
        for (UInt32 i = 0; i < db.NumFiles; ++i) {
            if (db.FileToFolder[i] != f) continue;
            Item* it = &items[count++];
            it->file = i;
            it->size = SzArEx_GetFileSize(&db, i);
            it->path[0] = '\0';
            const size_t len = SzArEx_GetFileNameUtf16(&db, i, NULL);
            name = (UInt16*)realloc(name, (len + 1) * sizeof(UInt16));
            SzArEx_GetFileNameUtf16(&db, i, name);
            char rel[1024];
            if (!SzArEx_IsDir(&db, i) && gzl_path(name, rel, sizeof rel))
                snprintf(it->path, sizeof it->path, "%s/%s", dest_dir, rel);
        }
        if (count == 0) continue;

        // One coder, LZMA2 (21) or LZMA (03 01 01).
        const Byte* c = db.db.CodersData + db.db.FoCodersOffsets[f];
        const Byte* c_end = db.db.CodersData + db.db.FoCodersOffsets[f + 1];
        if (c + 2 > c_end || c[0] != 1 || (c[1] & 0xD0) != 0) {
            set_error(error, error_size, "%s", "the archive uses a compression this app does not unpack");
            goto done;
        }
        const unsigned id_size = c[1] & 0x0F;
        const Byte* id = c + 2;
        const Byte* props = id + id_size;
        unsigned props_size = 0;
        if (c[1] & 0x20) props_size = *props++;
        const int lzma2 = id_size == 1 && id[0] == 0x21 && props_size == 1;
        const int lzma1 = id_size == 3 && id[0] == 3 && id[1] == 1 && id[2] == 1 && props_size == 5;
        if (!lzma2 && !lzma1) {
            set_error(error, error_size, "%s", "the archive uses a compression this app does not unpack");
            goto done;
        }
        CLzma2Dec dec2;
        CLzmaDec dec1;
        Lzma2Dec_Construct(&dec2);
        LzmaDec_Construct(&dec1);
        SRes alloc = lzma2 ? Lzma2Dec_Allocate(&dec2, props[0], &kAlloc)
                           : LzmaDec_Allocate(&dec1, props, props_size, &kAlloc);
        if (alloc != SZ_OK) {
            set_error(error, error_size, "%s", "not enough memory for the archive's dictionary");
            goto done;
        }
        if (lzma2) Lzma2Dec_Init(&dec2); else LzmaDec_Init(&dec1);

        const UInt32 pack = db.db.FoStartPackStreamIndex[f];
        UInt64 pack_left = db.db.PackPositions[pack + 1] - db.db.PackPositions[pack];
        UInt64 out_left = SzAr_GetFolderUnpackSize(&db.db, f);
        int failed = 0;
        if (fseeko(data, (off_t)(db.dataPos + db.db.PackPositions[pack]), SEEK_SET) != 0) {
            set_error(error, error_size, "%s", "cannot read the archive");
            failed = 1;
        }
        int k = 0;
        UInt64 file_left = items[0].size;
        UInt32 crc = CRC_INIT_VAL;
        FILE* out = NULL;
        size_t in_pos = 0, in_len = 0;
        while (out_left > 0 && !failed) {
            if (in_pos == in_len && pack_left > 0) {
                const size_t want = pack_left < kIn ? (size_t)pack_left : kIn;
                in_len = fread(in_buf, 1, want, data);
                in_pos = 0;
                pack_left -= in_len;
                if (in_len == 0) { set_error(error, error_size, "%s", "the archive is truncated"); failed = 1; break; }
            }
            SizeT dest_len = out_left < kOut ? (SizeT)out_left : kOut;
            SizeT src_len = in_len - in_pos;
            ELzmaStatus status;
            const SRes r = lzma2 ? Lzma2Dec_DecodeToBuf(&dec2, out_buf, &dest_len, in_buf + in_pos, &src_len,
                                                         LZMA_FINISH_ANY, &status)
                                 : LzmaDec_DecodeToBuf(&dec1, out_buf, &dest_len, in_buf + in_pos, &src_len,
                                                       LZMA_FINISH_ANY, &status);
            in_pos += src_len;
            if (r != SZ_OK || (dest_len == 0 && src_len == 0)) {
                set_error(error, error_size, "%s", "the archive is damaged (decoding failed)");
                failed = 1;
                break;
            }
            out_left -= dest_len;
            decoded += dest_len;
            // Route the decoded bytes to the files they belong to.
            size_t at = 0;
            while (at < dest_len || (k < count && file_left == 0)) {
                if (k < count && file_left == 0) {
                    // finished items[k]
                    if (out) {
                        fclose(out);
                        out = NULL;
                        const UInt32 fi = items[k].file;
                        if (SzBitWithVals_Check(&db.CRCs, fi) && db.CRCs.Vals[fi] != CRC_GET_DIGEST(crc)) {
                            set_error(error, error_size, "%s is damaged (CRC)", items[k].path);
                            failed = 1;
                            break;
                        }
                        ++written;
                    }
                    if (++k >= count) break;
                    file_left = items[k].size;
                    crc = CRC_INIT_VAL;
                    continue;
                }
                if (k >= count) break;
                if (!out && items[k].path[0] && file_left == items[k].size) {
                    mkdirs(items[k].path);
                    out = fopen(items[k].path, "wb");
                    if (!out) {
                        set_error(error, error_size, "cannot write %s", items[k].path);
                        failed = 1;
                        break;
                    }
                }
                size_t take = dest_len - at;
                if (take > file_left) take = (size_t)file_left;
                if (out) {
                    crc = CrcUpdate(crc, out_buf + at, take);
                    if (fwrite(out_buf + at, 1, take, out) != take) {
                        set_error(error, error_size, "cannot write %s (is the storage full?)", items[k].path);
                        failed = 1;
                        break;
                    }
                }
                at += take;
                file_left -= take;
            }
            if (progress) progress(user, decoded, total);
        }
        if (out) fclose(out);
        if (lzma2) Lzma2Dec_Free(&dec2, &kAlloc); else LzmaDec_Free(&dec1, &kAlloc);
        if (failed || out_left > 0) {
            if (!failed) set_error(error, error_size, "%s", "the archive is truncated");
            goto done;
        }
    }
    if (written == 0) {
        set_error(error, error_size, "%s", "the archive holds no GZL folder (not a Wind Waker texture pack)");
        goto done;
    }
    result = written;
done:
    if (data) fclose(data);
    free(in_buf);
    free(out_buf);
    free(items);
    free(name);
    free(look.buf);
    SzArEx_Free(&db, &kAlloc);
    File_Close(&in_stream.file);
    return result;
}
