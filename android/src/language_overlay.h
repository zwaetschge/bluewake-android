#ifndef BLUEWAKE_LANGUAGE_OVERLAY_H
#define BLUEWAKE_LANGUAGE_OVERLAY_H

// The game's text and word images in German, French, Spanish or Italian,
// built from the player's European disc when the USA disc opens
// (language_overlay.c). Android only: the link wraps GXRuntime's DVD layer.

#include "core/types.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Builds the replacement files for `language` (de, fr, es, it) from the USA
// image and the European one; false (and the game in English) for any other
// language or a disc that does not fit. dvd_open_image calls it with
// BLUEWAKE_LANGUAGE and BLUEWAKE_LANGUAGE_DISC.
bool bw_lang_overlay_build(const char* usa_path, const char* pal_path, const char* language);

// The replacement for a USA FST entry: its bytes and the offset it is served at.
bool bw_lang_overlay_file(s32 entry, const u8** data, u32* size, u32* start);

// For the options menu: "English", "English (<why>)", or "<code>, from <disc>".
const char* bw_lang_overlay_status(void);

#ifdef __cplusplus
}
#endif

#endif
