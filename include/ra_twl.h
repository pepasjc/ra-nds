// The 3DS hand-off: no Unlaunch there, so RA Prep and RA Sync are started by
// TWiLight Menu++'s own "autorun last game" (settings.ini: AUTORUNGAME,
// ROM_PATH, LAUNCH_TYPE, PREVIOUS_USED_DEVICE), and each step returns by
// restarting the console into TWiLight Menu++'s title (TLNC, with the title
// id TWiLight Menu++ leaves in sd:/_nds/nds-bootstrap/srBackendId.bin).
// nds-bootstrap-ra (ra_boot.cpp) does the same from its side; the user's
// own values of those settings are kept in sd:/_nds/ra/twl_restore.txt.
#ifndef RA_TWL_H
#define RA_TWL_H

// TWiLight Menu++ starts rom next: through nds-bootstrap (a game) or
// directly (homebrew such as raprep.nds / rasync.nds).  0 on success.
int ra_twl_autorun(const char *rom, int through_bootstrap);

// Puts the user's autorun settings back (twl_restore.txt), if saved
void ra_twl_restore(void);

// Sets TLNC so that returning from main() restarts into TWiLight Menu++.
// 0 on success (-1: no title id).
int ra_twl_reboot_target(void);

#endif
