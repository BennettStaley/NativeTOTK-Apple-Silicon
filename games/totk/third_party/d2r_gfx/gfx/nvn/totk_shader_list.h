/* The shared shader list as the Mac app downloads it (totk_shader_list.m, docs/shader-list.md): the parser and the
   difference with the local translation cache. The download is untrusted input, so the parser takes only the documented
   rows (scripts/shader_list.py's grammar, the same patterns), caps the file, the lines and the rows, and rejects the
   whole file on anything else: the previous list stays in use. Of a valid file only the V rows (a shader key and the
   bind state it was translated for) are kept; L, S, C and R rows are checked and dropped (the build uses them).
   Foundation only, header only: tests/test_shader_list_fetch.py checks it through tests/shader_list_fetch/probe.m. */
#pragma once
#import <Foundation/Foundation.h>
#include <string.h>

enum {
    TSL_MAX_BYTES = 48 << 20,     /* the list was 14 MB in October 2026 */
    TSL_MAX_LINE = 8192,          /* an R row (a pipeline descriptor in hex) is the longest, about 2 KB */
    TSL_MAX_ROWS = 1000000,
    TSL_MAX_DEPS = 4096,          /* the longest deps were 316 characters */
    TSL_MAX_SLOTS = 256,
};
#define TSL_HEADER "# TOTK shader list v1"

static inline int tsl_hex(const char *s, size_t n, size_t min, size_t max) {
    if (n < min || n > max) return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
static inline int tsl_key(NSString *s) { return s.length == 64 && tsl_hex(s.UTF8String, 64, 64, 64); }
static inline int tsl_word(NSString *s, int upper) {   /* [a-z0-9_]{1,32} (upper: A-Z too) */
    const char *c = s.UTF8String; size_t n = strlen(c);
    if (!n || n > 32) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((c[i] >= 'a' && c[i] <= 'z') || (c[i] >= '0' && c[i] <= '9') || c[i] == '_' || (upper && c[i] >= 'A' && c[i] <= 'Z'))) return 0;
    return 1;
}
static inline int tsl_slot(NSString *s) {   /* a slot number: 0..999 without leading zeros */
    const char *c = s.UTF8String; size_t n = strlen(c);
    if (!n || n > 3 || (n > 1 && c[0] == '0')) return 0;
    for (size_t i = 0; i < n; i++) if (c[i] < '0' || c[i] > '9') return 0;
    return 1;
}
static inline NSArray<NSString *> *tsl_sorted(NSDictionary *d) {   /* Python's sort_keys order (code points) */
    return [d.allKeys sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
        int r = strcmp(a.UTF8String, b.UTF8String); return r < 0 ? NSOrderedAscending : r > 0 ? NSOrderedDescending : NSOrderedSame; }];
}
/* The translator's deps as the list writes them (json.dumps(sort_keys=True, separators=(',', ':')), shader_list.py
   canonical), or nil when they are not deps: kinds tex / img / attr / fout, each a map of slot number to a type word or,
   for an image, to a small map of words. Only these characters can appear, so no JSON escaping is ever needed. */
static NSString *tsl_canonical(id deps) {
    static NSSet *kinds; static dispatch_once_t once;
    dispatch_once(&once, ^{ kinds = [NSSet setWithArray:@[@"tex", @"img", @"attr", @"fout"]]; });
    if (![deps isKindOfClass:[NSDictionary class]]) return nil;
    NSMutableString *out = [NSMutableString stringWithString:@"{"];
    for (NSString *kind in tsl_sorted(deps)) {
        NSDictionary *slots = deps[kind];
        if (![kind isKindOfClass:[NSString class]] || ![kinds containsObject:kind] || ![slots isKindOfClass:[NSDictionary class]] ||
            slots.count > TSL_MAX_SLOTS) return nil;
        [out appendFormat:@"%@\"%@\":{", out.length > 1 ? @"," : @"", kind];
        BOOL first = YES;
        for (NSString *slot in tsl_sorted(slots)) {
            id v = slots[slot];
            if (![slot isKindOfClass:[NSString class]] || !tsl_slot(slot)) return nil;
            [out appendFormat:@"%@\"%@\":", first ? @"" : @",", slot]; first = NO;
            if ([v isKindOfClass:[NSString class]]) {
                // "*" / "*_depth": a texture slot of the default bind state (totk_shader_rt.m: any texture, or any depth texture)
                if (!tsl_word(v, 0) && !([kind isEqualToString:@"tex"] && ([v isEqualToString:@"*"] || [v isEqualToString:@"*_depth"]))) return nil;
                [out appendFormat:@"\"%@\"", v];
            } else if ([v isKindOfClass:[NSDictionary class]] && [v count] <= 8) {
                [out appendString:@"{"];
                BOOL f2 = YES;
                for (NSString *k in tsl_sorted(v)) {
                    id x = ((NSDictionary *)v)[k];
                    if (![k isKindOfClass:[NSString class]] || !tsl_word(k, 0) || ![x isKindOfClass:[NSString class]] || !tsl_word(x, 1)) return nil;
                    [out appendFormat:@"%@\"%@\":\"%@\"", f2 ? @"" : @",", k, x]; f2 = NO;
                }
                [out appendString:@"}"];
            } else return nil;
        }
        [out appendString:@"}"];
    }
    [out appendString:@"}"];
    return out;
}
/* The deps text of a V row, checked: valid JSON, deps, and written exactly as canonical writes them. */
static inline NSString *tsl_deps_text(NSString *text) {
    if (!text.length || text.length > TSL_MAX_DEPS || [text characterAtIndex:0] != '{') return nil;
    id obj = [NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    NSString *canon = obj ? tsl_canonical(obj) : nil;
    return [canon isEqualToString:text] ? canon : nil;
}
static inline int tsl_flags(NSString *f) { return [f isEqualToString:@"t"] || [f isEqualToString:@"m"] || [f isEqualToString:@"tm"]; }
static inline int tsl_variant(NSString *v) {   /* <key>-<12 hex>_b */
    const char *c = v.UTF8String; size_t n = strlen(c);
    return n == 79 && tsl_hex(c, 64, 64, 64) && c[64] == '-' && tsl_hex(c + 65, 12, 12, 12) && !strcmp(c + 77, "_b");
}
static inline int tsl_source(NSString *s) {   /* a locator's RomFS path or module:<name> */
    const char *c = s.UTF8String; size_t n = strlen(c);
    if (!n || n > 512) return 0;
    if (!strncmp(c, "module:", 7)) {
        if (n == 7) return 0;
        for (size_t i = 7; i < n; i++) if (!((c[i] >= 'a' && c[i] <= 'z') || (c[i] >= '0' && c[i] <= '9'))) return 0;
        return 1;
    }
    for (NSString *part in [s componentsSeparatedByString:@"/"]) {
        if (!part.length || [part isEqualToString:@"."] || [part isEqualToString:@".."]) return 0;
        for (const char *p = part.UTF8String; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("_.()+-", *p))) return 0;
    }
    return 1;
}

/* A downloaded list: shader key -> its listed deps texts. nil (with *why) when the file is not the shared list. */
static NSDictionary<NSString *, NSArray<NSString *> *> *tsl_parse(NSData *data, NSString **why) {
#define TSL_REJECT(...) do { if (why) *why = [NSString stringWithFormat:__VA_ARGS__]; return nil; } while (0)
    if (!data.length) TSL_REJECT(@"empty");
    if (data.length > TSL_MAX_BYTES) TSL_REJECT(@"%lu bytes, more than the %d MB a list may have", (unsigned long)data.length, TSL_MAX_BYTES >> 20);
    const uint8_t *b = data.bytes; size_t n = data.length;
    for (size_t i = 0; i < n; i++)   /* plain ASCII text: tabs, line ends, printable characters */
        if (!(b[i] == '\t' || b[i] == '\n' || (b[i] == '\r' && i + 1 < n && b[i + 1] == '\n') || (b[i] >= 0x20 && b[i] < 0x7f)))
            TSL_REJECT(@"byte 0x%02x at offset %lu is not list text", b[i], (unsigned long)i);
    if (n < strlen(TSL_HEADER) || memcmp(b, TSL_HEADER, strlen(TSL_HEADER))) TSL_REJECT(@"no \"%s\" header", TSL_HEADER);
    NSMutableDictionary<NSString *, NSMutableOrderedSet<NSString *> *> *rows = [NSMutableDictionary dictionary];
    size_t line_no = 0, count = 0;
    for (size_t at = 0; at < n; ) {   /* no pool per line: a rejection returns its reason out of the loop */
        const uint8_t *eol = memchr(b + at, '\n', n - at);
        size_t len = (eol ? (size_t)(eol - (b + at)) : n - at), next = at + len + (eol ? 1 : 0);
        line_no++;
        if (len > TSL_MAX_LINE) TSL_REJECT(@"line %zu is longer than %d characters", line_no, TSL_MAX_LINE);
        if (len && b[at + len - 1] == '\r') len--;
        NSString *line = [[NSString alloc] initWithBytes:b + at length:len encoding:NSASCIIStringEncoding];
        at = next;
        if (!line.length || [line hasPrefix:@"#"]) continue;
        if (++count > TSL_MAX_ROWS) TSL_REJECT(@"more than %d rows", TSL_MAX_ROWS);
        NSArray<NSString *> *f = [line componentsSeparatedByString:@"\t"];
        NSString *kind = f[0];
        if ([kind isEqualToString:@"V"]) {
            NSString *deps = f.count == 3 && tsl_key(f[1]) ? tsl_deps_text(f[2]) : nil;
            if (!deps) TSL_REJECT(@"line %zu is not a V row (key, deps)", line_no);
            NSMutableOrderedSet *set = rows[f[1]];
            if (!set) rows[f[1]] = set = [NSMutableOrderedSet orderedSet];
            [set addObject:deps];
        } else if ([kind isEqualToString:@"L"]) {
            if (!(f.count == 6 && tsl_key(f[1]) && tsl_source(f[2]) && tsl_hex(f[3].UTF8String, f[3].length, 1, 10) &&
                  tsl_hex(f[4].UTF8String, f[4].length, 1, 10) && tsl_hex(f[5].UTF8String, f[5].length, 1, 10)))
                TSL_REJECT(@"line %zu is not an L row", line_no);
        } else if ([kind isEqualToString:@"S"]) {
            if (!(f.count == 4 && tsl_variant(f[1]) && ([f[2] isEqualToString:@"0"] || [f[2] isEqualToString:@"1"] || [f[2] isEqualToString:@"5"]) && tsl_flags(f[3])))
                TSL_REJECT(@"line %zu is not an S row", line_no);
        } else if ([kind isEqualToString:@"C"]) {
            if (!(f.count == 3 && tsl_variant(f[1]) && tsl_flags(f[2]))) TSL_REJECT(@"line %zu is not a C row", line_no);
        } else if ([kind isEqualToString:@"R"]) {
            if (!(f.count == 3 && f[1].length % 2 == 0 && tsl_hex(f[1].UTF8String, f[1].length, 2, TSL_MAX_LINE) && tsl_flags(f[2])))
                TSL_REJECT(@"line %zu is not an R row", line_no);
        } else if ([kind isEqualToString:@"B"]) {   // a render pipeline's border recipes (vertex, fragment; "-" for none) and flags: warm-up only
            if (!(f.count == 5 && f[1].length % 2 == 0 && tsl_hex(f[1].UTF8String, f[1].length, 2, TSL_MAX_LINE) && f[2].length && f[3].length && tsl_flags(f[4])))
                TSL_REJECT(@"line %zu is not a B row", line_no);
        } else if ([kind isEqualToString:@"A"]) {   // the bind state behind a variant name's suffix (upstream ef10acff, shader_list.py resolve): packaging only
            if (!(f.count == 3 && f[1].length == 12 && tsl_hex(f[1].UTF8String, f[1].length, 12, 12) && tsl_deps_text(f[2])))
                TSL_REJECT(@"line %zu is not an A row (suffix, deps)", line_no);
        } else TSL_REJECT(@"line %zu is no shader list row", line_no);
    }
    NSMutableDictionary<NSString *, NSArray<NSString *> *> *out = [NSMutableDictionary dictionaryWithCapacity:rows.count];
    for (NSString *key in rows) out[key] = rows[key].array;
    return out;
#undef TSL_REJECT
}

/* The deps texts a translation cache holds for one shader (<cache>/<key>.variants, the runtime's index). */
static NSSet<NSString *> *tsl_cache_deps(NSString *cache, NSString *key) {
    NSData *d = [NSData dataWithContentsOfFile:[cache stringByAppendingFormat:@"/%@.variants", key]];
    NSArray *a = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    NSMutableSet *out = [NSMutableSet set];
    if ([a isKindOfClass:[NSArray class]])
        for (NSDictionary *v in a) { NSString *c = [v isKindOfClass:[NSDictionary class]] ? tsl_canonical(v[@"deps"]) : nil; if (c) [out addObject:c]; }
    return out;
}
/* The list's variants the cache does not have: (key, deps) pairs in key order. have(key) gives the cache's deps texts. */
static NSArray<NSArray<NSString *> *> *tsl_missing(NSDictionary<NSString *, NSArray<NSString *> *> *list, NSSet<NSString *> *(^have)(NSString *key)) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *key in [list.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        NSSet<NSString *> *cached = have(key);
        for (NSString *deps in list[key]) if (![cached containsObject:deps]) [out addObject:@[key, deps]];
    }
    return out;
}
