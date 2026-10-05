#ifndef BLUEWAKE_HD_PACK_7Z_H
#define BLUEWAKE_HD_PACK_7Z_H

#include <stddef.h>

typedef void (*BwExtractProgress)(void* user, unsigned long long done, unsigned long long total);

// Unpacks the GZL folder of a Dolphin texture pack from a 7z archive into
// dest_dir/GZL (hd_pack_7z.c). Returns the files written, or -1 with a
// message in error.
int bw_hd_pack_extract_gzl(const char* archive, const char* dest_dir, BwExtractProgress progress, void* user,
                           char* error, size_t error_size);

#endif
