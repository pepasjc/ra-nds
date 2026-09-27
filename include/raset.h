// The achievement set file the nds-bootstrap RA engine reads, rendered from
// RA's patch data exactly as the GameSync server's render_set() does:
//
//     RASET<TAB>1
//     game<TAB><game id><TAB><md5><TAB><title>
//     ach<TAB><id><TAB><points><TAB><MemAddr><TAB><title><TAB><description>
//
// Core achievements only; RA's fake "Unknown Emulator" warning is dropped.
#ifndef RASET_H
#define RASET_H

#include <stddef.h>

#include "rc_api_runtime.h"

// RA adds fake achievements from this id up ("Warning: Unknown Emulator")
// for clients it doesn't know.
#define RA_WARNING_ACHIEVEMENT_ID 101000001

// malloc'd set text (free() it); *count gets the number of achievements.
char *raset_render(const rc_api_fetch_game_data_response_t *game, const char *md5,
                   unsigned *count, size_t *length);

#endif
