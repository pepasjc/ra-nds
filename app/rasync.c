// RA Sync: RetroAchievements for the DSi without a PC or server.
//
// nds-bootstrap-ra restarts the console into this app (through Unlaunch, in
// full DSi mode, so WPA2 works) at two points, and the app talks to
// retroachievements.org itself:
//
//  - Prep, before a game whose set is missing, unsigned or for another ROM
//    (sd:/_nds/ra/raprep.nds; nds-bootstrap wrote prep.txt: the ROM, the
//    loader, the ROM's fingerprint): ra_prepare_rom() hashes the ROM and
//    fetches, signs and saves its set (or notes that RA has none).  If RA
//    can't be reached, skip_once.txt lets the game start without a set this
//    once.  Then the loader starts again.
//
//  - Sync, when a game with a set is quit (sd:/_nds/ra/rasync.nds;
//    nds-bootstrap saved the real quit target and the game in return.txt):
//    move the session's signed unlocks from ramDump.bin into unlocks.bin and
//    send the new ones to RA, softcore only for now.  Without "submit=1" in
//    account.txt it is a dry run.
//
// Both end by asking Unlaunch for the next program and restarting.
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "https.h"
#include "ra_client.h"
#include "ra_sync.h"
#include "ra_twl.h"

#define PREP_FILE RA_DIR "/prep.txt"
#define SKIP_FILE RA_DIR "/skip_once.txt"
#define AFTER_PREP_FILE RA_DIR "/after_prep.txt"
#define RETURN_FILE RA_DIR "/return.txt"
#define SYNC_LOG RA_DIR "/rasync_log.txt"
#define DEFAULT_RETURN "sd:/_nds/TWiLightMenu/main.srldr"
#define DEFAULT_LOADER "sd:/_nds/nds-bootstrap-nightly.nds"

#define SAY ra_say

static void pause_frames(int frames) {
    while (frames-- > 0) swiWaitForVBlank();
}

// A WiFi driver stuck in its bring-up stays stuck until the console
// restarts (ra_client.c).  So restart once into the same step (this app, as
// raprep.nds or rasync.nds) and try again; wifi_retry.txt marks the second
// run, which then gives up like any failed connection.
#define WIFI_RETRY_FILE RA_DIR "/wifi_retry.txt"
#define PREP_PATH RA_DIR "/raprep.nds"
#define SYNC_PATH RA_DIR "/rasync.nds"
static int wifi_retried;
static const char *retry_self;      // where the retry restarts into
static char retry_prep_text[600];   // prep.txt for it (prep removed the file)

// On a 3DS (nds-bootstrap-ra marks prep.txt / return.txt with "3ds"): no
// Unlaunch, so the next program is started by TWiLight Menu++'s autorun and
// the console restarts into TWiLight Menu++ (ra_twl.c)
static int on_3ds;

// Restart into path next: through Unlaunch on a DSi, through TWiLight
// Menu++ (whose autorun setting says what runs) on a 3DS
static void handoff(const char *path) {
    if (on_3ds) {
        if (ra_twl_reboot_target()) SAY("\x1b[31mNo TWiLight Menu++ title id\x1b[39m\n");
    } else {
        ra_unlaunch_autoload(path);
    }
}

static void restart_if_wifi_hung(void) {
    if (wifi_retried || !ra_wifi_hung() || !retry_self) return;
    ra_write_text(WIFI_RETRY_FILE, "1");
    if (retry_prep_text[0]) ra_write_text(PREP_FILE, retry_prep_text);
    SAY("WiFi is stuck: restarting to\ntry once more\n");
    if (ra_logf) fclose(ra_logf);
    ra_logf = NULL;
    pause_frames(60);
    handoff(retry_self);  // on a 3DS TWiLight Menu++'s autorun still points here
    exit(0);  // calico restarts the console
}

static int go_online(void) {
    if (ra_wifi_connect(3) && https_init(RA_USER_AGENT) == 0) return 1;
    restart_if_wifi_hung();
    SAY("\x1b[31mNo connection\x1b[39m\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Prep: the set of the game about to start
// ---------------------------------------------------------------------------

static void prep(const ra_account *account, int have_account) {
    char rom[256] = "", loader[256] = "", fingerprint[64] = "";
    ra_read_line(PREP_FILE, 0, rom, sizeof(rom));
    ra_read_line(PREP_FILE, 1, loader, sizeof(loader));
    ra_read_line(PREP_FILE, 2, fingerprint, sizeof(fingerprint));
    remove(PREP_FILE);
    if (!loader[0]) snprintf(loader, sizeof(loader), "%s", DEFAULT_LOADER);
    snprintf(retry_prep_text, sizeof(retry_prep_text), "%s\n%s\n%s\n%s\n", rom, loader, fingerprint,
             on_3ds ? "3ds" : "");
    retry_self = PREP_PATH;

    const char *name = strrchr(rom, '/');
    SAY("Achievements for\n %.60s\n\n", name ? name + 1 : rom);

    int online = 0;
    int done = ra_prepare_rom(account, have_account, rom, fingerprint, go_online, &online);
    // Online anyway: send whatever unlocks are still waiting (usually RA
    // Sync does on quit)
    if (online && account->submit) {
        int moved = ra_move_ring();
        if (moved) SAY("%d unlock%s from the last game\n", moved, moved == 1 ? "" : "s");
        ra_send_unlocks(account);
    }
    https_close();

    if (!done && rom[0]) {
        ra_write_text(SKIP_FILE, rom);
        SAY("Starting without achievements;\nnext start tries again\n");
    }
    pause_frames(90);
    // The loader started from here can't quit straight to TWiLight Menu++
    // (the game hangs on its last frame); this tells it to quit through RA
    // Sync, which returns through Unlaunch
    if (rom[0]) ra_write_text(AFTER_PREP_FILE, rom);
    SAY("\nStarting the game...\n");
    if (on_3ds) ra_twl_autorun(rom, 1);  // TWiLight Menu++ starts it through nds-bootstrap
    handoff(loader);
}

// ---------------------------------------------------------------------------
// Sync: send the new unlocks
// ---------------------------------------------------------------------------

static void sync(const ra_account *account, int have_account, const char *played_rom) {
    retry_self = SYNC_PATH;
    int moved = ra_move_ring();
    if (moved) SAY("%d new unlock%s from the game\n", moved, moved == 1 ? "" : "s");

    long pending = ra_pending_unlocks();
    if (pending <= 0) {
        SAY("Nothing new to send\n");
        return;
    }
    SAY("%ld unlock%s to send\n", pending, pending == 1 ? "" : "s");
    if (!have_account) {
        SAY("No account in\n " RA_ACCOUNT_FILE "\n");
        return;
    }
    if (!account->submit) {
        SAY("Dry run (submit=1 in account.txt\nsends them): kept for later\n");
        return;
    }
    if (!go_online()) {
        SAY("Unlocks kept for next time\n");
        return;
    }
    ra_send_unlocks(account);

    // The account's unlocks for the game just played, for the next start
    if (played_rom[0]) {
        const char *name = strrchr(played_rom, '/');
        name = name ? name + 1 : played_rom;
        char set_path[320], known[33];
        uint32_t game_id = 0;
        snprintf(set_path, sizeof(set_path), RA_SETS_DIR "/%s.txt", name);
        if (ra_known_hash(set_path, NULL, known, &game_id) && game_id) ra_write_account_unlocks(account, name, game_id);
    }
    https_close();
}

int main(void) {
    consoleDemoInit();
    iprintf("RA Sync\n\n");

    char next[256] = DEFAULT_RETURN;
    if (!fatInitDefault()) {
        iprintf("SD card not readable\n");
    } else {
        ra_logf = fopen(SYNC_LOG, "a");
        time_t now = time(NULL);
        if (ra_logf) fprintf(ra_logf, "\n--- %s", ctime(&now));
        int have_key = ra_sync_init();
        wifi_retried = remove(WIFI_RETRY_FILE) == 0;  // this run is the restart-retry
        if (wifi_retried) SAY("(restarted after a stuck WiFi)\n");
        FILE *p = fopen(PREP_FILE, "rb");
        const int prep_mode = p != NULL;
        if (p) fclose(p);
        retry_self = prep_mode ? PREP_PATH : SYNC_PATH;
        char mode[16];
        ra_read_line(prep_mode ? PREP_FILE : RETURN_FILE, prep_mode ? 3 : 2, mode, sizeof(mode));
        on_3ds = !strcmp(mode, "3ds");

        ra_account account;
        int have_account = ra_account_load(&account) == 0;
        // First run: account.txt has the password instead of a token; log in
        // once and keep only the token RA hands out
        if (have_account && !account.token[0]) {
            SAY("Logging in as %s...\n", account.user);
            if (go_online() && ra_login_password(&account) == 0 && ra_account_save(&account) == 0) {
                SAY("\x1b[32mLogged in:\x1b[39m token saved,\npassword removed from account.txt\n");
            } else {
                SAY("\x1b[31mLogin failed:\x1b[39m check user= and\npassword= in account.txt\n");
                have_account = 0;
            }
            memset(account.password, 0, sizeof(account.password));
        }

        if (prep_mode) {
            prep(&account, have_account);  // leaves the loader in Unlaunch's auto-load
            if (ra_logf) fclose(ra_logf);
            return 0;
        }
        // return.txt: the quit target, then the game that was played
        char played[256] = "";
        if (ra_read_line(RETURN_FILE, 0, next, sizeof(next)) || !next[0])
            snprintf(next, sizeof(next), "%s", DEFAULT_RETURN);
        ra_read_line(RETURN_FILE, 1, played, sizeof(played));
        if (!have_key) SAY("\x1b[31mNo console key (eMMC CID):\x1b[39m\nunlocks can't be checked\n");
        sync(&account, have_account, played);
        pause_frames(90);
        if (ra_logf) fclose(ra_logf);
    }
    if (on_3ds) {
        ra_twl_restore();  // the user's autorun settings back
        iprintf("\nBack to TWiLight Menu++\n");
    } else {
        iprintf("\nBack to %s\n", next);
    }
    handoff(next);
    // No jump target: calico restarts the console, into Unlaunch's path
    // (DSi) or TWiLight Menu++'s title (3DS)
    return 0;
}
