// RetroAchievements hash of a DS ROM: rcheevos' rc_hash_nintendo_ds()
// (header, ARM9 and ARM7 binaries, icon), streamed from the file.  Ported
// from GameSync's ds/source/ra_hash.c.
#ifndef NDS_HASH_H
#define NDS_HASH_H

// 1 and the lowercase hex MD5 in hash_out, or 0 if the file can't be read
int nds_hash_file(const char *path, char hash_out[33]);

#endif
