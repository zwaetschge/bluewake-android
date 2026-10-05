// The in-app language overlay against the image-building scripts: for each
// language, every replaced file must be byte-identical to the same file in a
// GZLE01-xx image that build_lang_iso.py + localize_file_select.py made from
// the same European disc. Needs the discs, so it is run by hand:
//   cc -O1 -DBW_LANG_OVERLAY_NO_WRAP -I<GXRuntime>/include -Iandroid/src \
//      tests/language_overlay_test.c -lz -o /tmp/lot
//   /tmp/lot GZLE01.iso GZLP01.iso ref-de.iso de [fr es it with their refs]
#include "../android/src/language_overlay.c"

int main(int argc, char** argv) {
    if (argc < 5 || (argc - 3) % 2) {
        fprintf(stderr, "usage: %s USA.iso PAL.iso REF-xx.iso xx [REF-yy.iso yy ...]\n", argv[0]);
        return 2;
    }
    int failures = 0;
    for (int a = 3; a + 1 < argc; a += 2) {
        if (!bw_lang_overlay_build(argv[1], argv[2], argv[a + 1])) {
            printf("%s: build failed: %s\n", argv[a + 1], bw_lang_overlay_status());
            ++failures;
            continue;
        }
        Disc usa = {0}, ref = {0};
        if (!disc_open(&usa, argv[1], "GZLE01") || !disc_open(&ref, argv[a], "GZLE01")) {
            printf("cannot open the discs\n");
            return 2;
        }
        int changed = 0, same = 0;
        for (u32 i = 1; i < usa.count; ++i) {
            if (is_dir(&usa, i)) continue;
            const bool ref_changed = memcmp(usa.fst + i * 12u, ref.fst + i * 12u, 12) != 0;
            const u8* data; u32 size, start;
            const bool mine = bw_lang_overlay_file((s32)i, &data, &size, &start);
            if (!ref_changed && !mine) continue;
            ++changed;
            const u32 off = be32(ref.fst + i * 12u + 4);
            u32 rsize = be32(ref.fst + i * 12u + 8);
            u8* want = malloc(rsize ? rsize : 1);
            fseeko(ref.f, off, SEEK_SET);
            if (fread(want, 1, rsize, ref.f) != rsize) rsize = 0;
            const char* name = usa.strings + (be32(usa.fst + i * 12u) & 0xFFFFFF);
            if (!mine || size != rsize || memcmp(data, want, rsize) != 0) {
                printf("%s: %s differs (overlay %s %u bytes, reference %u)\n", argv[a + 1], name,
                       mine ? "has" : "lacks", mine ? size : 0, rsize);
                ++failures;
            } else {
                ++same;
            }
            free(want);
        }
        printf("%s: %d of %d replaced files identical; status \"%s\"\n", argv[a + 1], same, changed,
               bw_lang_overlay_status());
        disc_close(&usa);
        disc_close(&ref);
    }
    return failures ? 1 : 0;
}
