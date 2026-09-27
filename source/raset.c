// Set file rendering; see include/raset.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raset.h"

// Growable text
typedef struct {
    char *data;
    size_t len, cap;
} text_t;

static void text_add(text_t *t, const char *s, size_t n) {
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 8192;
        while (cap < t->len + n + 1) cap *= 2;
        t->data = realloc(t->data, cap);
        t->cap = cap;
    }
    memcpy(t->data + t->len, s, n);
    t->len += n;
    t->data[t->len] = '\0';
}

static void text_str(text_t *t, const char *s) {
    text_add(t, s, strlen(s));
}

static void text_num(text_t *t, unsigned long n) {
    char num[16];
    snprintf(num, sizeof(num), "%lu", n);
    text_str(t, num);
}

static int is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// " ".join(text.split()) as the server does: runs of whitespace become one
// space, none at either end
static void text_clean(text_t *t, const char *s) {
    int words = 0, gap = 0;
    for (; s && *s; s++) {
        if (is_space(*s)) {
            gap = 1;
            continue;
        }
        if (gap && words) text_add(t, " ", 1);
        gap = 0;
        words = 1;
        text_add(t, s, 1);
    }
}

char *raset_render(const rc_api_fetch_game_data_response_t *game, const char *md5,
                   unsigned *count, size_t *length) {
    text_t t = {0};
    text_str(&t, "RASET\t1\ngame\t");
    text_num(&t, game->id);
    text_str(&t, "\t");
    for (const char *c = md5; *c; c++) {
        char lower = (*c >= 'A' && *c <= 'Z') ? *c + 32 : *c;
        text_add(&t, &lower, 1);
    }
    text_str(&t, "\t");
    text_clean(&t, game->title);
    text_str(&t, "\n");
    *count = 0;
    for (uint32_t i = 0; i < game->num_achievements; i++) {
        const rc_api_achievement_definition_t *ach = &game->achievements[i];
        if (ach->category != RC_ACHIEVEMENT_CATEGORY_CORE || ach->id >= RA_WARNING_ACHIEVEMENT_ID) continue;
        const char *mem = ach->definition;
        if (!mem || !mem[0] || strchr(mem, '\t') || strchr(mem, '\n')) continue;
        text_str(&t, "ach\t");
        text_num(&t, ach->id);
        text_str(&t, "\t");
        text_num(&t, ach->points);
        text_str(&t, "\t");
        text_str(&t, mem);
        text_str(&t, "\t");
        text_clean(&t, ach->title);
        text_str(&t, "\t");
        text_clean(&t, ach->description);
        text_str(&t, "\n");
        (*count)++;
    }
    if (length) *length = t.len;
    return t.data;
}
