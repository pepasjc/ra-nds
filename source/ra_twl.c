// 3DS hand-off through TWiLight Menu++; see include/ra_twl.h.  Must agree
// with nds-bootstrap-ra's ra_boot.cpp (same files, keys and format).
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_client.h"
#include "ra_twl.h"

#define TWL_SETTINGS "sd:/_nds/TWiLightMenu/settings.ini"
#define TWL_RESTORE RA_DIR "/twl_restore.txt"
#define TWL_TITLE_ID "sd:/_nds/nds-bootstrap/srBackendId.bin"

static const char *const keys[] = { "AUTORUNGAME", "ROM_PATH", "LAUNCH_TYPE", "PREVIOUS_USED_DEVICE" };
#define KEY_COUNT 4

static char *read_all(const char *path, long *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(n + 1);
    if (data && fread(data, 1, n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) {
        data[n] = '\0';
        if (length) *length = n;
    }
    return data;
}

// "KEY = value" at the start of a line (not KEY_SOMETHING)
static char *find_key(char *text, const char *key) {
    size_t n = strlen(key);
    for (char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (!strncmp(line, key, n) && (line[n] == ' ' || line[n] == '=')) return line;
    }
    return NULL;
}

static void get_value(char *text, const char *key, char *out, size_t size) {
    out[0] = '\0';
    char *line = find_key(text, key);
    if (!line) return;
    char *eq = strchr(line, '=');
    char *end = strchr(line, '\n');
    if (!eq || (end && eq > end)) return;
    eq++;
    while (*eq == ' ') eq++;
    size_t n = end ? (size_t)(end - eq) : strlen(eq);
    while (n && (eq[n - 1] == '\r' || eq[n - 1] == ' ')) n--;
    if (n >= size) n = size - 1;
    memcpy(out, eq, n);
    out[n] = '\0';
}

// Rewrites settings.ini with these keys set (added under [SRLOADER] if absent)
static int set_values(const char *const *names, const char *const *values, int count) {
    long length = 0;
    char *text = read_all(TWL_SETTINGS, &length);
    if (!text) return -1;
    size_t cap = length + 1024;
    for (int i = 0; i < count; i++) cap += strlen(values[i]) + strlen(names[i]) + 8;
    char *out = malloc(cap);
    if (!out) {
        free(text);
        return -1;
    }
    size_t used = 0;
    int done[KEY_COUNT] = {0};
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        int replaced = 0;
        for (int i = 0; i < count && !replaced; i++) {
            size_t n = strlen(names[i]);
            if (!done[i] && !strncmp(line, names[i], n) && (line[n] == ' ' || line[n] == '=')) {
                used += sprintf(out + used, "%s = %s\r\n", names[i], values[i]);
                done[i] = replaced = 1;
            }
        }
        if (!replaced) {
            memcpy(out + used, line, len);
            used += len;
            if (!strncmp(line, "[SRLOADER]", 10)) {
                // missing keys go right under the section header
                for (int i = 0; i < count; i++) {
                    char probe[64];
                    snprintf(probe, sizeof(probe), "%s", names[i]);
                    if (!find_key(text, probe)) {
                        used += sprintf(out + used, "%s = %s\r\n", names[i], values[i]);
                        done[i] = 1;
                    }
                }
            }
        }
        line += len;
    }
    free(text);
    FILE *f = fopen(TWL_SETTINGS, "wb");
    int ok = f && fwrite(out, 1, used, f) == used;
    if (f) ok = fclose(f) == 0 && ok;
    free(out);
    return ok ? 0 : -1;
}

// The user's values, saved once per chain (the first change)
static void save_originals(void) {
    FILE *f = fopen(TWL_RESTORE, "rb");
    if (f) {
        fclose(f);
        return;
    }
    char *text = read_all(TWL_SETTINGS, NULL);
    if (!text) return;
    f = fopen(TWL_RESTORE, "wb");
    for (int i = 0; f && i < KEY_COUNT; i++) {
        char value[300];
        get_value(text, keys[i], value, sizeof(value));
        fprintf(f, "%s=%s\n", keys[i], value);
    }
    if (f) fclose(f);
    free(text);
}

int ra_twl_autorun(const char *rom, int through_bootstrap) {
    save_originals();
    const char *values[KEY_COUNT] = { "1", rom, through_bootstrap ? "1" : "2", "0" };
    return set_values(keys, values, KEY_COUNT);
}

void ra_twl_restore(void) {
    char *text = read_all(TWL_RESTORE, NULL);
    if (!text) return;
    const char *names[KEY_COUNT];
    char values[KEY_COUNT][300];
    const char *value_ptrs[KEY_COUNT];
    int count = 0;
    for (char *line = strtok(text, "\r\n"); line && count < KEY_COUNT; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        for (int i = 0; i < KEY_COUNT; i++) {
            if (!strcmp(line, keys[i])) {
                names[count] = keys[i];
                snprintf(values[count], sizeof(values[count]), "%s", eq + 1);
                value_ptrs[count] = values[count];
                count++;
            }
        }
    }
    free(text);
    if (count && set_values(names, value_ptrs, count) == 0) remove(TWL_RESTORE);
}

int ra_twl_autorun_is_ours(void) {
    char *text = read_all(TWL_SETTINGS, NULL);
    if (!text) return 0;
    char autorun[8], rom[300];
    get_value(text, "AUTORUNGAME", autorun, sizeof(autorun));
    get_value(text, "ROM_PATH", rom, sizeof(rom));
    free(text);
    return !strcmp(autorun, "1") && strstr(rom, "/_nds/ra/") != NULL;
}

int ra_twl_pending(void) {
    FILE *f = fopen(TWL_RESTORE, "rb");
    if (f) fclose(f);
    return f != NULL || ra_twl_autorun_is_ours();
}

void ra_twl_stop_autorun(void) {
    if (!ra_twl_autorun_is_ours()) return;
    const char *name[1] = { "AUTORUNGAME" };
    const char *value[1] = { "0" };
    set_values(name, value, 1);
}

int ra_twl_reboot_target(void) {
    u32 id[2] = {0, 0};
    FILE *f = fopen(TWL_TITLE_ID, "rb");
    if (!f) return -1;
    int ok = fread(id, sizeof(u32), 2, f) == 2;
    fclose(f);
    if (!ok || (!id[0] && !id[1])) return -1;
    TlncData data = {0};
    data.target_tid = ((u64)id[1] << 32) | id[0];
    data.valid = 1;
    data.app_type = TlncAppType_Nand;
    tlncSetData(&data);
    pmClearResetJumpTarget();
    return 0;
}
