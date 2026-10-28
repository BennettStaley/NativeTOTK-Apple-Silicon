/* The player's map pins (Mac Tools › Teleport), read from the newest save of the profile, the way a save
   editor does: progress.sav is a table of (GameData name hash, value or offset) pairs from 0x28 up to the GUID table
   (hash 0xa3db7114); an array's offset points at its length and then its elements. MapData.IconData.MapPinData.Pos
   (0x5f93e70f, Vector3Array) holds the pins' positions and .Type (0x1b62aa42, EnumArray) their colours, Invalid for an
   unused slot (names and hashes: marcrobledo/savegame-editors zelda-totk, MIT). The game writes the save often (every
   autosave), so a pin placed since then shows after the next save. Plain C, header only (tests/dish/probe.c checks it). */
#pragma once
#include "dish_give.h"   /* dish_murmur3: the GameData name hash */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { MAP_PINS_MAX = 16 };
typedef struct { float xyz[3]; const char *colour; } MapPin;
static const char *const MAP_PIN_COLOURS[] = {"Red", "Blue", "Yellow", "Green", "Purple", "LightBlue"};

/* The pins in a progress.sav image; returns how many (0 when the table or the arrays are not there). */
static inline int map_pins_parse(const uint8_t *b, size_t n, MapPin *out, int max) {
    if (n < 0x30) return 0;
    uint32_t magic; memcpy(&magic, b, 4);
    if (magic != 0x01020304) return 0;
    uint32_t pos = 0, type = 0;
    for (size_t i = 0x28; i + 8 <= n; i += 8) {
        uint32_t h, v; memcpy(&h, b + i, 4); memcpy(&v, b + i + 4, 4);
        if (h == 0xa3db7114u) break;
        if (h == 0x5f93e70fu) pos = v; else if (h == 0x1b62aa42u) type = v;
    }
    if (!pos || !type || pos + 4 > n || type + 4 > n) return 0;
    uint32_t np, nt; memcpy(&np, b + pos, 4); memcpy(&nt, b + type, 4);
    if (np > 1024 || nt != np || pos + 4 + 12ull * np > n || type + 4 + 4ull * nt > n) return 0;
    int got = 0;
    for (uint32_t k = 0; k < np && got < max; k++) {
        uint32_t t; memcpy(&t, b + type + 4 + 4 * k, 4);
        const char *colour = NULL;
        for (size_t c = 0; c < sizeof MAP_PIN_COLOURS / sizeof *MAP_PIN_COLOURS; c++) if (t == dish_murmur3(MAP_PIN_COLOURS[c])) colour = MAP_PIN_COLOURS[c];
        if (!colour) continue;   /* Invalid: an empty slot */
        MapPin *p = &out[got];
        memcpy(p->xyz, b + pos + 4 + 12 * k, 12);
        if (p->xyz[0] != p->xyz[0] || p->xyz[1] != p->xyz[1] || p->xyz[2] != p->xyz[2]) continue;   /* NaN */
        p->colour = colour; got++;
    }
    return got;
}
/* The newest <profile>/save/<user>/slot_NN/progress.sav: its path into newest (empty when none). */
static inline void map_pins_newest_save(const char *profile, char *newest, size_t size) {
    char dir[1024]; newest[0] = 0; time_t best = 0;
    snprintf(dir, sizeof dir, "%s/save", profile);
    DIR *users = opendir(dir); struct dirent *u;
    while (users && (u = readdir(users))) {
        if (u->d_name[0] == '.') continue;
        char udir[1400]; snprintf(udir, sizeof udir, "%s/%s", dir, u->d_name);
        DIR *slots = opendir(udir); struct dirent *s;
        while (slots && (s = readdir(slots))) {
            if (strncmp(s->d_name, "slot_", 5)) continue;
            char path[1800]; struct stat st;
            snprintf(path, sizeof path, "%s/%s/progress.sav", udir, s->d_name);
            if (!stat(path, &st) && st.st_mtime >= best) { best = st.st_mtime; snprintf(newest, size, "%s", path); }
        }
        if (slots) closedir(slots);
    }
    if (users) closedir(users);
}
/* The pins of the newest save of profile; returns how many, -1 when there is no save. */
static inline int map_pins_read(const char *profile, MapPin *out, int max, char *from, size_t from_size) {
    char path[1800]; map_pins_newest_save(profile, path, sizeof path);
    if (from) snprintf(from, from_size, "%s", path);
    if (!path[0]) return -1;
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    uint8_t *b = malloc(4 << 20); size_t n = b ? fread(b, 1, 4 << 20, f) : 0; fclose(f);
    int got = b ? map_pins_parse(b, n, out, max) : 0;
    free(b);
    return got;
}
