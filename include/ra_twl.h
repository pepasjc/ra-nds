// The 3DS hand-off: no Unlaunch there, so RA Prep and RA Sync are started by
// TWiLight Menu++'s own "autorun last game" (settings.ini: AUTORUNGAME,
// ROM_PATH, LAUNCH_TYPE, PREVIOUS_USED_DEVICE), and each app step returns
// to TWiLight Menu++ by returning from main() (a TLNC restart into its title
// left both screens white).  nds-bootstrap-ra (ra_boot.cpp) sets the same
// keys from its side and restarts through TLNC; the user's
// own values of those settings are kept in sd:/_nds/ra/twl_restore.txt.
#ifndef RA_TWL_H
#define RA_TWL_H

// TWiLight Menu++ starts rom next: through nds-bootstrap (a game) or
// directly (homebrew such as raprep.nds / rasync.nds).  0 on success.
int ra_twl_autorun(const char *rom, int through_bootstrap);

// Puts the user's autorun settings back (twl_restore.txt), if saved
void ra_twl_restore(void);

// TWiLight Menu++'s autorun points at one of our apps (sd:/_nds/ra/...)
int ra_twl_autorun_is_ours(void);

// A 3DS chain is under way: twl_restore.txt exists or autorun is ours
int ra_twl_pending(void);

// Safety net: autorun off if it still points at one of our apps, so a lost
// step can't restart into RA Sync for ever
void ra_twl_stop_autorun(void);

#endif
