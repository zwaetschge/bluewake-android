#ifndef BLUEWAKE_HD_PACK_H
#define BLUEWAKE_HD_PACK_H

// Hypatia's HD texture pack, downloaded and unpacked from the options menu
// (hd_pack.c). The states match HdPack.java's.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BW_HD_PACK_IDLE = 0, BW_HD_PACK_DOWNLOADING = 1, BW_HD_PACK_UNPACKING = 2, BW_HD_PACK_DONE = 3,
       BW_HD_PACK_FAILED = 4 };

// Where the pack goes: <external files>/Load/Textures/GZLE01 (its GZL inside).
const char* bw_hd_pack_dest(void);
bool bw_hd_pack_installed(void);
// Starts the download and the unpacking on the activity's thread.
bool bw_hd_pack_start(void);
// The state, with the bytes done and in all, and the last message.
int bw_hd_pack_state(unsigned long long* done, unsigned long long* total, char* message, size_t n);
// True once after an install finished.
bool bw_hd_pack_take_finished(void);

#ifdef __cplusplus
}
#endif

#endif
