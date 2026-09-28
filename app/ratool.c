// RA Tool: RA-NDS by hand, started from TWiLight Menu++ like any homebrew.
//
//   A  test the connection: WiFi, HTTPS, the account
//   X  prepare every ROM in sd:/roms/nds: hash it and fetch, sign and save
//      its achievement set (what RA Prep does before a game, for all at once)
//   Y  send the unlocks that are waiting
//
// Handy on a console where RA Prep and RA Sync can't run yet (a 3DS: they
// start through Unlaunch), and to prepare a whole collection in one go.
// Everything goes to the screen and sd:/_nds/ra/ratool_log.txt.
#include <nds.h>
#include <dirent.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "https.h"
#include "ra_client.h"
#include "ra_sync.h"

#define TOOL_LOG RA_DIR "/ratool_log.txt"
#define ROM_DIR "sd:/roms/nds"
#define MAX_ROMS 512

#define SAY ra_say

static ra_account account;
static int have_account;

static int go_online(void) {
    if (ra_wifi_connect(3) && https_init(RA_USER_AGENT) == 0) return 1;
    SAY("\x1b[31mNo connection\x1b[39m\n");
    return 0;
}

static void wait_button(void) {
    iprintf("\nPress A\n");
    while (pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & (KEY_A | KEY_B | KEY_START)) break;
    }
}

// First run with password= in account.txt: log in and keep the token
static int ensure_token(void) {
    if (!have_account) {
        SAY("No account in\n " RA_ACCOUNT_FILE "\n");
        return 0;
    }
    if (account.token[0]) return 1;
    SAY("Logging in as %s...\n", account.user);
    if (go_online() && ra_login_password(&account) == 0 && ra_account_save(&account) == 0) {
        SAY("\x1b[32mLogged in:\x1b[39m token saved\n");
        return 1;
    }
    SAY("\x1b[31mLogin failed\x1b[39m\n");
    have_account = 0;
    return 0;
}

static void test_connection(void) {
    SAY("--- Connection test\n");
    if (!ensure_token() || !go_online()) return;
    timer_start();
    if (ra_login(&account) == 0) SAY("HTTPS and account OK (%u ms)\n", timer_ms());
    https_close();
}

static int has_nds_extension(const char *name) {
    size_t n = strlen(name);
    return n > 4 && (!strcasecmp(name + n - 4, ".nds") || !strcasecmp(name + n - 4, ".dsi"));
}

// ROMs under dir, one folder level deep
static int list_roms(const char *dir, char **roms, int count, int depth) {
    DIR *d = opendir(dir);
    if (!d) return count;
    struct dirent *e;
    while ((e = readdir(d)) && count < MAX_ROMS) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            if (depth < 1) count = list_roms(path, roms, count, depth + 1);
        } else if (has_nds_extension(e->d_name)) {
            roms[count++] = strdup(path);
        }
    }
    closedir(d);
    return count;
}

static void prepare_all(void) {
    SAY("--- Prepare ROMs in " ROM_DIR "\n");
    if (!ensure_token()) return;
    char **roms = malloc(MAX_ROMS * sizeof(char *));
    int count = list_roms(ROM_DIR, roms, 0, 0);
    SAY("%d ROMs (hold B to stop)\n", count);
    int ready = 0, failed = 0;
    for (int i = 0; i < count; i++) {
        const char *name = strrchr(roms[i], '/');
        SAY("\n[%d/%d] %.40s\n", i + 1, count, name ? name + 1 : roms[i]);
        int online = 0;
        if (ra_prepare_rom(&account, have_account, roms[i], NULL, go_online, &online)) ready++;
        else failed++;
        scanKeys();
        if (keysHeld() & KEY_B) {
            SAY("Stopped\n");
            break;
        }
    }
    https_close();
    SAY("\n%d ready, %d not\n", ready, failed);
    for (int i = 0; i < count; i++) free(roms[i]);
    free(roms);
}

static void send_waiting(void) {
    SAY("--- Send waiting unlocks\n");
    int moved = ra_move_ring();
    if (moved) SAY("%d unlock%s from the last game\n", moved, moved == 1 ? "" : "s");
    long pending = ra_pending_unlocks();
    if (pending <= 0) {
        SAY("Nothing to send\n");
        return;
    }
    SAY("%ld to send\n", pending);
    if (!account.submit) {
        SAY("submit=1 isn't in account.txt:\nnot sending\n");
        return;
    }
    if (!ensure_token() || !go_online()) return;
    ra_send_unlocks(&account);
    https_close();
}

static void show_menu(void) {
    char key_id[9];
    ra_key_id(key_id);
    consoleClear();
    iprintf("RA Tool (RA-NDS)\n\n");
    iprintf("Console key: %s\n", key_id);
    iprintf("Account: %s\n", have_account ? account.user : "(none)");
    iprintf("Unlocks waiting: %ld\n\n", ra_pending_unlocks());
    iprintf("A: test the connection\n");
    iprintf("X: prepare all ROMs\n");
    iprintf("Y: send waiting unlocks\n");
    iprintf("START: exit\n");
}

int main(void) {
    consoleDemoInit();
    if (!fatInitDefault()) {
        iprintf("SD card not readable\n");
        wait_button();
        return 0;
    }
    ra_logf = fopen(TOOL_LOG, "a");
    time_t now = time(NULL);
    if (ra_logf) fprintf(ra_logf, "\n--- %s", ctime(&now));
    ra_sync_init();
    char key_id[9];
    ra_key_id(key_id);
    if (ra_logf) fprintf(ra_logf, "console key %s, %s mode\n", key_id, isDSiMode() ? "DSi" : "DS");
    have_account = ra_account_load(&account) == 0;

    show_menu();
    while (pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        u32 keys = keysDown();
        if (keys & KEY_START) break;
        if (!(keys & (KEY_A | KEY_X | KEY_Y))) continue;
        consoleClear();
        if (keys & KEY_A) test_connection();
        else if (keys & KEY_X) prepare_all();
        else send_waiting();
        wait_button();
        show_menu();
    }
    if (ra_logf) fclose(ra_logf);
    return 0;
}
