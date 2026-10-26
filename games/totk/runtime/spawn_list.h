/* The Spawn tab's list (Tools › Spawn, runtime/spawn_panel_mac.h, ios/Sources/spawn_menu.m): the actor list the build
   makes from the player's RomFS (scripts/prepare_actor_list.py, build/actor_list.tsv in the app), its categories, the
   search, and the player's favorites and recents (spawn-picks.txt beside the profile, next to cheats-enabled.txt).
   Plain C, header only, no game code: tests/test_spawn_list.c runs it on any Mac. */
#pragma once
#include "actor_spawn_queue.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The category column, in order. System (locators, sound, camera, player parts) is never shown. Dishes (cooked meals,
   Item_Cook_*) have no actor pack: they are given into the inventory instead of spawned (runtime/dish_give.h). */
enum { SPAWN_CAT_FAVORITES, SPAWN_CAT_RECENTS, SPAWN_CAT_CREATURES, SPAWN_CAT_NPCS, SPAWN_CAT_ITEMS, SPAWN_CAT_DISHES,
       SPAWN_CAT_WEAPONS, SPAWN_CAT_ARMOR, SPAWN_CAT_ZONAI, SPAWN_CAT_OBJECTS, SPAWN_CAT_SYSTEM, SPAWN_NCATS };
enum { SPAWN_SHOWN_CATS = SPAWN_CAT_SYSTEM };   /* the column: Favorites .. Objects */
static const char *const SPAWN_CAT_NAMES[SPAWN_NCATS] = {"Favorites", "Recents", "Creatures", "NPCs", "Items", "Dishes",
                                                         "Weapons", "Armor", "Zonai", "Objects", "System"};
static const char *const SPAWN_CAT_TOKENS[SPAWN_NCATS] = {NULL, NULL, "creatures", "npcs", "items", "dishes", "weapons",
                                                          "armor", "zonai", "objects", "system"};
/* Where the display name comes from (the list's last column). Internal: no name was found, the internal one made
   readable; hidden unless Show unnamed is on. */
enum { SPAWN_SRC_GAME, SPAWN_SRC_LOOKUP, SPAWN_SRC_DECIPHERED, SPAWN_SRC_INTERNAL };
static const char *const SPAWN_SRC_TOKENS[] = {"game", "lookup", "deciphered", "internal"};

typedef struct { const char *name, *display; unsigned char category, source, pickup; } SpawnActor;
/* A meal effect of the game's cooking table: the level range and the base duration in seconds (0: the effect has no
   duration, e.g. extra hearts). LifeRecover is the hearts field itself: its max is the most hearts a dish restores. */
typedef struct { const char *name; int min_level, max_level, base_time; } SpawnEffect;
enum { SPAWN_EFFECT_MAX = 32 };
typedef struct { char *text; SpawnActor *actor; int count; SpawnEffect effect[SPAWN_EFFECT_MAX]; int neffect; } SpawnList;
static inline int spawn_actor_is_dish(const SpawnActor *a) { return a && a->category == SPAWN_CAT_DISHES; }

static inline int spawn_token_index(const char *const *tokens, int n, const char *word) {
    for (int i = 0; i < n; i++) if (tokens[i] && !strcmp(tokens[i], word)) return i;
    return -1;
}

/* actor_list.tsv: '#' comments, then name<TAB>category<TAB>pickup|world|pouch<TAB>display<TAB>source ("pouch": a dish,
   inventory only), and "@effect<TAB>name<TAB>min level<TAB>max level<TAB>base seconds" lines for the cooking table's
   meal effects (an older parser skips them: '@' is not a name). Lines with a bad name, an unknown category or source,
   or missing columns are skipped. Returns the number of actors (the list owns a copy). */
static inline int spawn_list_parse(SpawnList *l, const char *text, size_t len) {
    memset(l, 0, sizeof *l);
    if (!text) return 0;
    l->text = malloc(len + 1);
    if (!l->text) return 0;
    memcpy(l->text, text, len); l->text[len] = 0;
    int lines = 1;
    for (size_t i = 0; i < len; i++) lines += l->text[i] == '\n';
    l->actor = calloc((size_t)lines, sizeof *l->actor);
    if (!l->actor) { free(l->text); l->text = NULL; return 0; }
    char *p = l->text;
    while (*p) {
        char *eol = strchr(p, '\n'), *next = eol ? eol + 1 : p + strlen(p);
        if (eol) { *eol = 0; if (eol > p && eol[-1] == '\r') eol[-1] = 0; }
        char *col[5]; int n = 0;
        if (*p != '#') {
            col[n++] = p;
            for (char *q = p; *q && n < 5; q++) if (*q == '\t') { *q = 0; col[n++] = q + 1; }
        }
        if (n == 5 && !strcmp(col[0], "@effect") && spawn_name_valid(col[1]) && l->neffect < SPAWN_EFFECT_MAX) {
            char *end1, *end2, *end3;
            long lo = strtol(col[2], &end1, 10), hi = strtol(col[3], &end2, 10), t = strtol(col[4], &end3, 10);
            if (*col[2] && *col[3] && *col[4] && !*end1 && !*end2 && !*end3 && lo >= 0 && hi >= lo && hi <= 10000 && t >= 0 && t <= 100000) {
                SpawnEffect *e = &l->effect[l->neffect++];
                e->name = col[1]; e->min_level = (int)lo; e->max_level = (int)hi; e->base_time = (int)t;
            }
        } else if (n == 5 && spawn_name_valid(col[0]) && *col[3]) {
            int cat = spawn_token_index(SPAWN_CAT_TOKENS, SPAWN_NCATS, col[1]);
            int src = spawn_token_index(SPAWN_SRC_TOKENS, 4, col[4]);
            int pouch = !strcmp(col[2], "pouch");   /* only dishes, and dishes only so */
            if (cat >= SPAWN_CAT_CREATURES && src >= 0 && pouch == (cat == SPAWN_CAT_DISHES) &&
                (pouch || !strcmp(col[2], "pickup") || !strcmp(col[2], "world"))) {
                SpawnActor *a = &l->actor[l->count++];
                a->name = col[0]; a->display = col[3]; a->category = (unsigned char)cat; a->source = (unsigned char)src;
                a->pickup = !strcmp(col[2], "pickup");
            }
        }
        p = next;
    }
    return l->count;
}
static inline void spawn_list_free(SpawnList *l) { free(l->text); free(l->actor); memset(l, 0, sizeof *l); }
static inline int spawn_list_find(const SpawnList *l, const char *name) {
    for (int i = 0; i < l->count; i++) if (!strcmp(l->actor[i].name, name)) return i;
    return -1;
}
static inline const SpawnEffect *spawn_list_effect(const SpawnList *l, const char *name) {
    for (int i = 0; name && i < l->neffect; i++) if (!strcmp(l->effect[i].name, name)) return &l->effect[i];
    return NULL;
}
static inline int spawn_list_has_dishes(const SpawnList *l) {
    for (int i = 0; i < l->count; i++) if (spawn_actor_is_dish(&l->actor[i])) return 1;
    return 0;
}

/* ---- favorites and recents ---- */

enum { SPAWN_FAV_MAX = 200, SPAWN_RECENT_MAX = 20 };
typedef struct {
    char fav[SPAWN_FAV_MAX][SPAWN_NAME_MAX]; int nfav;            /* in the order they were marked */
    char recent[SPAWN_RECENT_MAX][SPAWN_NAME_MAX]; int nrecent;   /* newest first */
    int place;                                                    /* SPAWN_PLACE_*: the Place popup */
} SpawnPicks;

static inline int spawn_picks_is_favorite(const SpawnPicks *p, const char *name) {
    for (int i = 0; i < p->nfav; i++) if (!strcmp(p->fav[i], name)) return 1;
    return 0;
}
/* Marks or unmarks it: the new state (1 favorite), or -1 for a bad name or a full list. */
static inline int spawn_picks_toggle_favorite(SpawnPicks *p, const char *name) {
    if (!spawn_name_valid(name)) return -1;
    for (int i = 0; i < p->nfav; i++)
        if (!strcmp(p->fav[i], name)) {
            memmove(p->fav[i], p->fav[i + 1], (size_t)(p->nfav - i - 1) * sizeof p->fav[0]);
            p->nfav--;
            return 0;
        }
    if (p->nfav >= SPAWN_FAV_MAX) return -1;
    strcpy(p->fav[p->nfav++], name);   /* spawn_name_valid bounded it */
    return 1;
}
/* A spawn: to the front of the recents (once), the oldest falls off. */
static inline void spawn_picks_add_recent(SpawnPicks *p, const char *name) {
    if (!spawn_name_valid(name)) return;
    int at = p->nrecent < SPAWN_RECENT_MAX ? p->nrecent : SPAWN_RECENT_MAX - 1;
    for (int i = 0; i < p->nrecent; i++) if (!strcmp(p->recent[i], name)) { at = i; break; }
    if (at == p->nrecent) p->nrecent++;
    memmove(p->recent[1], p->recent[0], (size_t)at * sizeof p->recent[0]);
    strcpy(p->recent[0], name);
}
/* spawn-picks.txt: "favorite <name>" and "recent <name>" lines (recents newest first), "place front|above|far|feet",
   '#' comments. Bad lines are
   skipped, so a damaged file loses only those. */
static inline void spawn_picks_parse(SpawnPicks *p, const char *text) {
    memset(p, 0, sizeof *p);
    while (text && *text) {
        const char *eol = strchr(text, '\n'); size_t len = eol ? (size_t)(eol - text) : strlen(text);
        char line[160];
        if (len < sizeof line) {
            memcpy(line, text, len); line[len] = 0;
            for (char *e = line + strlen(line); e > line && isspace((unsigned char)e[-1]); ) *--e = 0;
            if (!strncmp(line, "favorite ", 9) && spawn_name_valid(line + 9) && !spawn_picks_is_favorite(p, line + 9) && p->nfav < SPAWN_FAV_MAX)
                strcpy(p->fav[p->nfav++], line + 9);
            else if (!strncmp(line, "recent ", 7) && spawn_name_valid(line + 7) && p->nrecent < SPAWN_RECENT_MAX) {
                int seen = 0;
                for (int i = 0; i < p->nrecent; i++) seen |= !strcmp(p->recent[i], line + 7);
                if (!seen) strcpy(p->recent[p->nrecent++], line + 7);
            } else if (!strncmp(line, "place ", 6) && spawn_place_parse(line + 6) >= 0) p->place = spawn_place_parse(line + 6);
        }
        text = eol ? eol + 1 : text + len;
    }
}
/* The file's text; returns its length (snprintf rules: more than n means it did not fit). */
static inline int spawn_picks_format(const SpawnPicks *p, char *out, size_t n) {
    size_t used = 0;
#define SPAWN_PUT(...) do { int k = snprintf(out + (used < n ? used : n), used < n ? n - used : 0, __VA_ARGS__); if (k > 0) used += (size_t)k; } while (0)
    SPAWN_PUT("# The Spawn tab's favorites, recents (newest first) and placement. The game rewrites this file.\n");
    for (int i = 0; i < p->nfav; i++) SPAWN_PUT("favorite %s\n", p->fav[i]);
    for (int i = 0; i < p->nrecent; i++) SPAWN_PUT("recent %s\n", p->recent[i]);
    if (p->place > 0 && p->place < SPAWN_PLACE_COUNT) SPAWN_PUT("place %s\n", SPAWN_PLACE_WORDS[p->place]);
#undef SPAWN_PUT
    return (int)used;
}

/* ---- what the list shows ---- */

/* Case-insensitive (ASCII) substring. */
static inline const char *spawn_find_ci(const char *hay, const char *needle, size_t n) {
    if (!n) return hay;
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < n && hay[k] && tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == n) return hay;
    }
    return NULL;
}
/* 0 no match, else a rank (1 best): every word of the query is in the display or the internal name; 1 when the display
   name starts with the query, 2 when one of its words starts with the query's first word, 3 otherwise. */
static inline int spawn_match(const SpawnActor *a, const char *query) {
    const char *q = query, *first = NULL; size_t first_len = 0;
    while (*q) {
        while (*q && isspace((unsigned char)*q)) q++;
        const char *w = q; while (*q && !isspace((unsigned char)*q)) q++;
        size_t n = (size_t)(q - w);
        if (!n) break;
        if (!first) { first = w; first_len = n; }
        if (!spawn_find_ci(a->display, w, n) && !spawn_find_ci(a->name, w, n)) return 0;
    }
    if (!first) return 0;
    while (isspace((unsigned char)*query)) query++;
    size_t qn = strlen(query); while (qn && isspace((unsigned char)query[qn - 1])) qn--;
    if (spawn_find_ci(a->display, query, qn) == a->display) return 1;
    for (const char *d = a->display; *d; d++)
        if ((d == a->display || !isalnum((unsigned char)d[-1])) && spawn_find_ci(d, first, first_len) == d) return 2;
    return 3;
}
static inline int spawn_query_empty(const char *q) {
    for (; q && *q; q++) if (!isspace((unsigned char)*q)) return 0;
    return 1;
}
/* The indices to show, at most max; returns how many. A query searches every category (System left out), best matches
   first and the list's order within a rank; no query shows the category: Favorites in the list's order, Recents
   newest first, the others without the unnamed (internal) entries unless show_unnamed. Favorites and recents show
   whatever their name. Without dishes (a menu that cannot give them) they are left out everywhere. */
static inline int spawn_list_filter_dishes(const SpawnList *l, const SpawnPicks *p, int category, const char *query, int show_unnamed,
                                           int dishes, int *out, int max) {
    int n = 0;
    if (!spawn_query_empty(query)) {
        for (int rank = 1; rank <= 3; rank++)
            for (int i = 0; i < l->count && n < max; i++) {
                const SpawnActor *a = &l->actor[i];
                if (a->category == SPAWN_CAT_SYSTEM || (!show_unnamed && a->source == SPAWN_SRC_INTERNAL) || (!dishes && spawn_actor_is_dish(a))) continue;
                if (spawn_match(a, query) == rank) out[n++] = i;
            }
        return n;
    }
    if (category == SPAWN_CAT_RECENTS) {
        for (int r = 0; p && r < p->nrecent && n < max; r++) {
            int i = spawn_list_find(l, p->recent[r]);
            if (i >= 0 && (dishes || !spawn_actor_is_dish(&l->actor[i]))) out[n++] = i;
        }
        return n;
    }
    for (int i = 0; i < l->count && n < max; i++) {
        const SpawnActor *a = &l->actor[i];
        if (!dishes && spawn_actor_is_dish(a)) continue;
        if (category == SPAWN_CAT_FAVORITES ? (p && spawn_picks_is_favorite(p, a->name))
                                            : a->category == category && category != SPAWN_CAT_SYSTEM && (show_unnamed || a->source != SPAWN_SRC_INTERNAL))
            out[n++] = i;
    }
    return n;
}
static inline int spawn_list_filter(const SpawnList *l, const SpawnPicks *p, int category, const char *query, int show_unnamed,
                                    int *out, int max) {
    return spawn_list_filter_dishes(l, p, category, query, show_unnamed, 1, out, max);
}

/* ---- files (the menus; tests use the parts above) ---- */

/* spawn-picks.txt beside the profile (TOTK_PROFILE_DIR's folder, like cheats-enabled.txt); TOTK_SPAWN_PICKS overrides. */
static inline int spawn_picks_path(char *out, size_t n) {
    const char *e = getenv("TOTK_SPAWN_PICKS"), *p = getenv("TOTK_PROFILE_DIR");
    if (e && *e) return snprintf(out, n, "%s", e) < (int)n;
    if (!p || !*p) return 0;
    size_t len = strlen(p);
    while (len > 1 && p[len - 1] == '/') len--;
    while (len > 0 && p[len - 1] != '/') len--;   /* the folder that holds the profile */
    if (!len) return snprintf(out, n, "spawn-picks.txt") < (int)n;
    return snprintf(out, n, "%.*sspawn-picks.txt", (int)len, p) < (int)n;
}
static inline char *spawn_read_file(const char *path, size_t limit, size_t *len) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    char *buf = malloc(limit + 1); size_t n = buf ? fread(buf, 1, limit + 1, f) : 0;
    fclose(f);
    if (!buf || n > limit) { free(buf); return NULL; }
    buf[n] = 0; if (len) *len = n;
    return buf;
}
static inline void spawn_picks_load(SpawnPicks *p) {
    char path[1024]; char *text = spawn_picks_path(path, sizeof path) ? spawn_read_file(path, 1u << 20, NULL) : NULL;
    spawn_picks_parse(p, text);
    free(text);
}
/* Written through a temporary file, so a crash leaves the old one. 0 on success. */
static inline int spawn_picks_save(const SpawnPicks *p) {
    char path[1024], tmp[1040];
    if (!spawn_picks_path(path, sizeof path)) return -1;
    int len = spawn_picks_format(p, NULL, 0);
    char *text = malloc((size_t)len + 1); if (!text) return -1;
    spawn_picks_format(p, text, (size_t)len + 1);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); int ok = f && fwrite(text, 1, (size_t)len, f) == (size_t)len;
    if (f && fclose(f)) ok = 0;
    free(text);
    if (!ok || rename(tmp, path)) { remove(tmp); return -1; }
    return 0;
}
/* The app's actor list (Mac: build/actor_list.tsv in the game folder; iOS: GameData/actor_list.tsv). */
static inline int spawn_list_load(SpawnList *l, const char *path) {
    size_t len = 0; char *text = spawn_read_file(path, 16u << 20, &len);
    int n = spawn_list_parse(l, text, len);
    free(text);
    return n;
}
