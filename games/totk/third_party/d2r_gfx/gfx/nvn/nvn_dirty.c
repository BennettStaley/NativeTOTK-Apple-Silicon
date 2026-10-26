// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>

// Order of page state and mprotect (the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY):
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, and one whose state is PROT is always read-only. Setting read-only used to be "record PROT first, batch up a run, then mprotect"; during that time another thread's unprotect-before-write
// could set the state back to NONE, and then the batched mprotect landed => the page read-only with state NONE: when the game thread wrote to it the page-fault handler only returned "re-execute" and spun forever;
// when the host read a file into it with a system call, prep_write saw state NONE and skipped, the system call did not go through the page-fault handler and failed with EFAULT, leaving the data incomplete.
// BUSY lasts at most one mprotect (plus, when setting read-only, the few atomic writes that change the state of the pages in the run). The wait has an upper limit BUSY_WAIT_MS and reports red beyond it:
// in the page-fault handler, exceeding it returns "not our page" and lets the crash handler report the crash truthfully instead of spinning silently. The wait only reads atomics and takes no lock.
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 16384 // was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing. TOTK (2026-10-17): 4096 was full in the user's own games and most long test games (about 3,900 live pools in the world, more when moving between regions); now 16384
/* TOTK: cgen[c] = highest page generation among pages [c << CHUNK_SHIFT, (c + 1) << CHUNK_SHIFT) (1 MB of 16 KB pages), raised
   after the page's own generation is stored, so a chunk at or below a stamp holds no page above it (nvn_dirty_check). */
enum { CHUNK_SHIFT = 6 };
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; _Atomic uint64_t *cgen; size_t cap; _Atomic int live; _Atomic uint64_t latest;
    _Atomic uint64_t fence, gstamp; uintptr_t owner; size_t osize; } range_t;   // owner / osize: the pool (storage, size) that registered the range (nvn_dirty_remove_range)   // fence / gstamp (TOTK frames in flight): newest submission fence that reads this range, "now" when its pages were last all made read-only   // latest (TOTK): highest page generation in the range
/* TOTK_DIRTY_FAST (default on; =0 off), 2026-10-23. Two lookups the replay makes for every CPU-mirrored texture at its
   check in each submission (tex_mtl: about 2.5% of the translate thread in the Korok Forest profile):
   - nvn_dirty_since walked every page of the texture whenever anything else in its pool had been written since the stamp
     (the big material pools are written by streaming all the time; a 42 MB texture is 2,600 pages). The range's cgen[]
     (above: the highest page generation per chunk of 64 pages, raised by unprotect_page after the page's own generation
     and before the range's latest) answers it: a chunk whose maximum is not above the stamp holds no page that is, so it
     is skipped (pages_since), and the answer is the page walk's. Upstream kept the same maxima in a blk[] array of its
     own; this tree's cgen[] (also used by the frames-in-flight guard) serves both.
   - find() scanned the ~4,000 live ranges whenever the address was not in one of the thread's last 8 ranges. A per-thread
     table by page of the address answers first; an entry counts only if its range is live and holds the address, as for
     the recent list, and live ranges never share a page, so it is the range the scan would find. */
enum { BLK_SHIFT = CHUNK_SHIFT, BLK_PAGES = 1 << BLK_SHIFT, FIND_HASH = 1024 };
static int g_dirty_fast = 1;   // set once, before the first range exists (nvn_dirty_add_range)
static _Atomic uint64_t g_blk_skips, g_find_hash_hits, g_find_scans;
#if defined(GAME_BOTW)
/* BOTW: a pool that shares host pages with live ranges gets ranges for its other pages (the gaps), so ranges never share
   a page and the fault handler still finds at most one range per page. Before, such a pool got no range: its textures
   counted as untracked and were uploaded again at every submission (one 256x32 texture, 1 MB a frame). A texture across a
   range border is looked at range by range. g_rmem is the pool that owns each range: a pool is destroyed through its own
   address, which can lie in another pool's range (it starts on that pool's last page), so the address lookup dropped the
   other pool's range (after the map's Hero's Path playback about 350 uploads, 130 MB a frame, 33 ms of submit CPU).
   g_rdying: set before the removal unprotects the range, so a texture of a neighbour pool that shares a page with it does
   not protect that page meanwhile (a page left read-only in a dead range would fault with no range to find). */
static uintptr_t g_rmem[MAX_RANGE];
static _Atomic uint8_t g_rdying[MAX_RANGE];
static _Atomic uint64_t g_ov_inside, g_ov_split, g_ov_same, g_ov_many, g_rm_none;
#endif
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static _Atomic uint64_t g_add_class[3];   // TOTK: pools registered clipped (shares a host page with another pool), adding nothing (all pages tracked by others), untracked (contains another range)
void nvn_dirty_add_stats(uint64_t v[3]) { for (int i = 0; i < 3; i++) v[i] = atomic_load(&g_add_class[i]); }
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static void mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = mach_absolute_time(); mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
}

static mach_timebase_info_data_t g_tb;   // Obtained when a pool is first registered (the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("[red] dirty tracking: page %p stuck BUSY for over %d ms (the protecting thread did not finish); no longer waiting", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static range_t *find(uintptr_t a) {
    /* TOTK: the replay asks about the same few pools over and over; the last hits per thread (8, round robin) are checked
       first (a slot reused for another pool fails the bounds or live test and falls through to the scan), so the scan over
       the ~4,000 live pools of the world mostly runs for the first touch of a pool. */
    static __thread range_t *recent[8]; static __thread unsigned next;
    static __thread range_t *hashed[FIND_HASH];   // TOTK_DIRTY_FAST: by page of the address
    for (int k = 0; k < 8; k++) {
        range_t *l = recent[k];
        if (l && atomic_load_explicit(&l->live, memory_order_acquire) && a >= l->base && a < l->end) return l;
    }
    unsigned h = 0;
    if (g_dirty_fast) {
        uintptr_t pg = a >> g_shift; h = (unsigned)(pg ^ (pg >> 10) ^ (pg >> 20)) & (FIND_HASH - 1);
        range_t *l = hashed[h];
        if (l && atomic_load_explicit(&l->live, memory_order_acquire) && a >= l->base && a < l->end) { recent[next++ & 7] = l; atomic_fetch_add_explicit(&g_find_hash_hits, 1, memory_order_relaxed); return l; }
        atomic_fetch_add_explicit(&g_find_scans, 1, memory_order_relaxed);
    }
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && a >= g_r[i].base && a < g_r[i].end) { recent[next++ & 7] = &g_r[i]; if (g_dirty_fast) hashed[h] = &g_r[i]; return &g_r[i]; }
    return NULL;
}
static void dirty_fast_init(void) { const char *e = getenv("TOTK_DIRTY_FAST"); g_dirty_fast = !(e && !atoi(e)); if (!g_dirty_fast) LOG("TOTK_DIRTY_FAST=0: dirty checks walk every page, range lookups scan the list"); }
/* Pages [p, q] of r: one written after stamp? (TOTK_DIRTY_FAST: blocks whose maximum is not above the stamp are skipped.) */
static int pages_since(range_t *r, uintptr_t p, uintptr_t q, uint64_t stamp) {
    if (!g_dirty_fast) { for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1; return 0; }
    uint64_t skipped = 0;
    for (uintptr_t i = p; i <= q;) {
        uintptr_t last = ((i >> BLK_SHIFT) << BLK_SHIFT) + BLK_PAGES - 1; if (last > q) last = q;
        if (atomic_load(&r->cgen[i >> BLK_SHIFT]) <= stamp) { i = last + 1; skipped++; continue; }
        for (; i <= last; i++) if (atomic_load(&r->gen[i]) > stamp) { if (skipped) atomic_fetch_add_explicit(&g_blk_skips, skipped, memory_order_relaxed); return 1; }
    }
    if (skipped) atomic_fetch_add_explicit(&g_blk_skips, skipped, memory_order_relaxed);
    return 0;
}
void nvn_dirty_fast_stats(int *on, uint64_t *blk_skips, uint64_t *hash_hits, uint64_t *scans) {
    *on = g_dirty_fast; *blk_skips = atomic_load(&g_blk_skips); *hash_hits = atomic_load(&g_find_hash_hits); *scans = atomic_load(&g_find_scans);
}
/* TOTK diagnostic (on with DM_NVN_TILELOG): a ring of the latest page unprotections with the writer, read by
   nvn_dirty_recent: game writes (fault: pc / lr of the faulting code) and host writes (dm_guest_write_prepare caller). */
#include <sys/ucontext.h>
static nvn_dirty_rec g_drec[4096]; static _Atomic uint64_t g_drec_n; static int g_drec_on;
uint64_t dm_host_write_pc;   // set by dm_guest_write_prepare (runtime/gfx_bridge.c) before calling prep_write
static void drec_add(uintptr_t page, uint64_t pc, uint64_t lr, uint32_t src) {
    if (!g_drec_on) return;
    uint64_t i = atomic_fetch_add(&g_drec_n, 1) % 4096; g_drec[i] = (nvn_dirty_rec){ page, pc, lr, src };
}
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max) {
    uint64_t n = atomic_load(&g_drec_n); int k = 0;
    for (uint64_t j = n > 4096 ? n - 4096 : 0; j < n && k < max; j++) { nvn_dirty_rec r = g_drec[j % 4096]; if (r.page >= lo && r.page < hi) out[k++] = r; }
    return k;
}
static void unprotect_page(range_t *r, uintptr_t p) {   // The caller has already changed the state from PROT to BUSY
    mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1, l = atomic_load(&r->latest);
    atomic_store(&r->gen[p], g);
    { _Atomic uint64_t *c = &r->cgen[p >> CHUNK_SHIFT]; uint64_t cg = atomic_load(c); while (cg < g && !atomic_compare_exchange_weak(c, &cg, g)) {} }   // before latest and the second "now" step
    while (l < g && !atomic_compare_exchange_weak(&r->latest, &l, g)) {}
    /* TOTK (#90): "now" moves on once more after the page's generation is stored. A checker that read "now" between the
       first step and the stores found the page clean and kept that "now" as its last look (tex_mtl's now == seen); the page
       is writable from here on, so without a newer "now" every later write went unseen until something else faulted. */
    atomic_fetch_add(&g_now, 1);
    atomic_store(&r->state[p], PG_NONE);
}
/* TOTK frames in flight (TOTK_FRAMES_IN_FLIGHT=2): the game may run one frame ahead of the GPU, so it can write memory
   a submission still in flight reads straight from the pool (zero-copy). nvn_dirty_guard (called at commit for every
   pool the submission read) records that submission's fence on the range and makes the range's pages read-only; a CPU
   write that faults on such a page (or a host write through prep_write) first waits until that fence is complete, then
   takes the usual dirty-tracking path. The wait only polls an atomic "completed fence" and sleeps (usable in the fault
   handler, no locks). Threads marked with nvn_dirty_guard_nowait (the GPU completion callbacks, which write counters
   into pool memory) never wait: they would wait for their own submission. Cheap when nothing is in flight: one atomic
   compare of the newest guarded fence against the completed one. */
static uint64_t (*g_done_fn)(void);
static _Atomic uint64_t g_guard_max, g_gw_n, g_gw_ticks, g_gw_timeouts, g_guard_calls, g_guard_skipped, g_guard_untracked;
static __thread int t_nowait;
enum { GUARD_WAIT_MS = 2000 };
static int g_guard_audit; static _Atomic uint64_t g_audit_n, g_audit_sample[8];   // TOTK_FIF_AUDIT: writes that would have waited, and the last pages
void nvn_dirty_guard_init(uint64_t (*done)(void), int audit) { g_done_fn = done; g_guard_audit = audit; if (!g_tb.denom) mach_timebase_info(&g_tb); }
void nvn_dirty_guard_nowait(void) { t_nowait = 1; }
static void guard_wait(range_t *r, uintptr_t addr) {
    uint64_t f = atomic_load_explicit(&r->fence, memory_order_acquire);
    if (!f || !g_done_fn || t_nowait || g_done_fn() >= f) return;
    if (g_guard_audit) { uint64_t i = atomic_fetch_add(&g_audit_n, 1); atomic_store(&g_audit_sample[i & 7], addr & ~(g_pg - 1)); return; }
    uint64_t t0 = mach_absolute_time();
    for (unsigned n = 0; g_done_fn() < f; n++) {
        if (n < 64) { sched_yield(); continue; }
        struct timespec ts = {0, 50000}; nanosleep(&ts, NULL);
        if ((mach_absolute_time() - t0) * g_tb.numer / g_tb.denom > (uint64_t)GUARD_WAIT_MS * 1000000ull) {
            if (!atomic_fetch_add(&g_gw_timeouts, 1)) LOG("[red] frames in flight: a CPU write to %p waited over %d ms for fence %llu; writing anyway", (void *)r->base, GUARD_WAIT_MS, (unsigned long long)f);
            break; }
    }
    atomic_fetch_add(&g_gw_n, 1); atomic_fetch_add(&g_gw_ticks, mach_absolute_time() - t0);
}
static inline void guard_check(range_t *r, uintptr_t addr) {
    if (__builtin_expect(atomic_load_explicit(&g_guard_max, memory_order_relaxed) != 0, 0) && g_done_fn && g_done_fn() < atomic_load(&g_guard_max)) guard_wait(r, addr);
}
void nvn_dirty_protect(const void *addr, size_t len);
static void guard_range(range_t *r, uint64_t fence) {
    { uint64_t f = atomic_load(&r->fence); while (f < fence && !atomic_compare_exchange_weak(&r->fence, &f, fence)) {} }
    { uint64_t m = atomic_load(&g_guard_max); while (m < fence && !atomic_compare_exchange_weak(&g_guard_max, &m, fence)) {} }
    uint64_t gs = atomic_load(&r->gstamp);
    if (gs && atomic_load(&r->latest) <= gs) { atomic_fetch_add(&g_guard_skipped, 1); return; }   // no page of the range was unprotected since it was all read-only
    uint64_t s = atomic_load(&g_now);   // before the pass: a page unprotected after this gets a generation above it and is seen next time
    size_t np = (r->end - r->base) >> g_shift, nc = (np + (1u << CHUNK_SHIFT) - 1) >> CHUNK_SHIFT;
    for (size_t c = 0; c < nc; c++) {
        if (gs && atomic_load(&r->cgen[c]) <= gs) continue;   // nothing in this 1 MB chunk was unprotected since the last pass
        uintptr_t a = r->base + ((uintptr_t)c << (CHUNK_SHIFT + g_shift)), e = a + ((uintptr_t)1 << (CHUNK_SHIFT + g_shift));
        if (e > r->end) e = r->end;
        nvn_dirty_protect((const void *)a, e - a);
    }
    { uint64_t g = atomic_load(&r->gstamp); while (g < s && !atomic_compare_exchange_weak(&r->gstamp, &g, s)) {} }
}
/* The span may lie in several ranges (a pool sharing its end pages with neighbours, see nvn_dirty_add_range): each one
   is guarded; a page of the span no range covers makes it unguardable (0). */
int nvn_dirty_guard(const void *addr, size_t len, uint64_t fence) {
    atomic_fetch_add(&g_guard_calls, 1);
    if (!len) { atomic_fetch_add(&g_guard_untracked, 1); return 0; }
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) {
        range_t *r = find(a);
        if (!r) { atomic_fetch_add(&g_guard_untracked, 1); return 0; }
        guard_range(r, fence);
        a = r->end;
    }
    return 1;
}
uint64_t nvn_dirty_range_fence(const void *mem) { range_t *r = find((uintptr_t)mem); return r ? atomic_load(&r->fence) : 0; }
int nvn_dirty_span_fence(const void *addr, size_t len, uint64_t *fence) {
    uint64_t f = 0; uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) { range_t *r = find(a); if (!r) return 0; uint64_t g = atomic_load(&r->fence); if (g > f) f = g; a = r->end; }
    *fence = f; return 1;
}
void nvn_dirty_guard_audit(uint64_t *n, uint64_t sample[8]) { *n = atomic_load(&g_audit_n); for (int i = 0; i < 8; i++) sample[i] = atomic_load(&g_audit_sample[i]); }
void nvn_dirty_guard_stats(uint64_t v[6]) {
    v[0] = atomic_load(&g_gw_n); v[1] = atomic_load(&g_gw_ticks) * (g_tb.denom ? g_tb.numer : 1) / (g_tb.denom ? g_tb.denom : 1);
    v[2] = atomic_load(&g_gw_timeouts); v[3] = atomic_load(&g_guard_calls); v[4] = atomic_load(&g_guard_skipped); v[5] = atomic_load(&g_guard_untracked);
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    range_t *r = is_write ? find((uintptr_t)addr) : NULL;
    if (!r) return 0;
    guard_check(r, (uintptr_t)addr);
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift;
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_faults, 1);
        if (g_drec_on && uc) { _STRUCT_MCONTEXT64 *mc = ((ucontext_t *)uc)->uc_mcontext; drec_add(r->base + (p << g_shift), mc->__ss.__pc, mc->__ss.__lr, 0); }
        return 1; }
    if (st == PG_BUSY) return busy_wait(r, p);   // Another thread is changing this page's protection: wait for it to finish and re-execute (if it became read-only, it faults once more and takes the branch above)
    return atomic_load(&r->gen[p]) != 0;   // Just unprotected by another thread (this fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        guard_check(r, a);
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q; p++) {
            for (;;) { uint8_t st = PG_PROT;   // After waiting out BUSY, look again: the other side may have been setting read-only, in which case the page is PROT after the wait and still has to be unprotected
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_preps, 1); drec_add(r->base + (p << g_shift), dm_host_write_pc, 0, 1); break; }
                if (st != PG_BUSY || !busy_wait(r, p)) break; }
        }
        a = stop;
    }
}

/* Writes on behalf of the GPU (a rendering written back into CPU-visible memory): the bytes change without counting as
   a CPU write. Protected pages are taken to BUSY (a guest write meanwhile waits in the fault handler, then faults again
   and is recorded as usual), made writable, and protected again by nvn_dirty_gpu_write_end with their generation kept.
   Pages already unprotected are left as they are. Returns a token for nvn_dirty_gpu_write_end (NULL: nothing tracked). */
#if !defined(GAME_BOTW)
typedef struct { range_t *r; uintptr_t p0, p1; uint8_t *was; } gpu_write_tok;
void *nvn_dirty_gpu_write_begin(void *addr, size_t len) {
    if (!len) return NULL;
    range_t *r = find((uintptr_t)addr); if (!r || (uintptr_t)addr + len > r->end) return NULL;
    uintptr_t a = (uintptr_t)addr, p0 = (a - r->base) >> g_shift, p1 = (a + len - 1 - r->base) >> g_shift;
    gpu_write_tok *t = malloc(sizeof *t); uint8_t *was = calloc(p1 - p0 + 1, 1); if (!t || !was) { free(t); free(was); return NULL; }
    *t = (gpu_write_tok){ r, p0, p1, was };
    for (uintptr_t p = p0; p <= p1; p++) {
        for (;;) { uint8_t st = PG_PROT;
            if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE); was[p - p0] = 1; break; }
            if (st != PG_BUSY || !busy_wait(r, p)) break; }
    }
    return t;
}
void nvn_dirty_gpu_write_end(void *tok) {
    gpu_write_tok *t = tok; if (!t) return;
    for (uintptr_t p = t->p0; p <= t->p1; p++) if (t->was[p - t->p0]) { mp(t->r->base + (p << g_shift), g_pg, PROT_READ); atomic_store(&t->r->state[p], PG_PROT); }
    free(t->was); free(t);
}
#endif

#if !defined(GAME_BOTW)
void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dirty_fast_init(); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); g_drec_on = getenv("DM_NVN_TILELOG") != NULL; }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    /* TOTK: one tracking range per page: find() returns the first live range for an address, so a page covered by two
       ranges would have its protection state looked up in the wrong one (a fault on a page protected under the older range
       reported as a crash). NVN pools are 4 KiB aligned, host pages 16 KiB, so pools the game places next to each other
       often share one host page. The new range is clipped to the pages no live range covers yet: a shared page at either
       end stays with the range that has it (2026-10-23: before, such a pool was left untracked as a whole -- every texture
       in it treated as dirty, and with TOTK_FRAMES_IN_FLIGHT=2 every submission reading it unguarded, about 20 pools a
       frame in the world). A pool whose pages all lie in other ranges adds nothing (still fully tracked, by them); only an
       older range strictly inside the new pool's span leaves it untracked (counted). */
    uintptr_t nb = (uintptr_t)mem & ~(g_pg - 1), ne = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    const uintptr_t nb0 = nb, ne0 = ne;
    for (int moved = 1; moved && nb < ne; ) {
        moved = 0;
        for (int i = 0; i < n && nb < ne; i++) {
            if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire) || ne <= g_r[i].base || nb >= g_r[i].end) continue;
            if (g_r[i].base <= nb) { nb = g_r[i].end; moved = 1; }
            else if (g_r[i].end >= ne) { ne = g_r[i].base; moved = 1; }
            else {
                atomic_fetch_add(&g_add_class[2], 1);
                static _Atomic int said; int expected = 0;
                if (atomic_compare_exchange_strong(&said, &expected, 1))
                    LOG("dirty tracking: pool %p+%#zx contains tracked range %#lx-%#lx; not tracked", mem, size, (unsigned long)g_r[i].base, (unsigned long)g_r[i].end);
                pthread_mutex_unlock(&g_slot_mu); return;
            }
        }
    }
    if (nb >= ne) { atomic_fetch_add(&g_add_class[1], 1); pthread_mutex_unlock(&g_slot_mu); return; }   // every page already tracked by other ranges
    if (nb != nb0 || ne != ne0) atomic_fetch_add(&g_add_class[0], 1);
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("[red] dirty tracking ranges full (%d); pool %p not tracked", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    uintptr_t base = nb, end = ne;
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen, *cgen = r->cgen;
    size_t nc = (np + (1u << CHUNK_SHIFT) - 1) >> CHUNK_SHIFT;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); cgen = calloc(nc, sizeof *cgen);
        if (!state || !gen || !cgen) die("dirty page table allocation failed (%zu pages)", np);
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset((void *)cgen, 0, nc * sizeof *cgen);
    }
    r->owner = (uintptr_t)mem; r->osize = size;
    r->base = base; r->end = end; r->state = state; r->gen = gen; r->cgen = cgen; atomic_store(&r->latest, 0); atomic_store(&r->fence, 0); atomic_store(&r->gstamp, 0);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem, size_t size) {
    pthread_mutex_lock(&g_slot_mu);
    /* TOTK: the range this pool registered, not find(mem): a pool that shares its first page with an older pool (or lies
       inside it) would otherwise remove the older pool's range. Nothing is removed when the pool added no range. */
    range_t *r = NULL; int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && g_r[i].owner == (uintptr_t)mem && g_r[i].osize == size) { r = &g_r[i]; break; }
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    range_t *r = find((uintptr_t)addr); return r && (uintptr_t)addr + len <= r->end;
}
// Set read-only: mark the page BUSY first, and mark it PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    range_t *r = find((uintptr_t)addr);
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // Consecutive pages set to PROT are merged into one mprotect (per-page calls ran to over a thousand per frame when large textures were first uploaded; character select, startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break a run: they are not protected again, and pages another thread is changing are left alone
    // TOTK (#90): a BUSY page is waited out (the run so far is protected first, so nothing of ours is BUSY meanwhile) and
    // looked at again. An unprotect in progress leaves the page writable with a generation the caller's stamp may already
    // cover: skipped, later writes to it never faulted and the texture stayed stale (letters missing in the glyph cache).
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ); for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT); open = 0; }
        if (!took && st == PG_BUSY && busy_wait(r, i)) i--;   // this page again
    }
}
#endif
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
#if !defined(GAME_BOTW)
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    if (!nvn_dirty_tracked(addr, len)) return 1;   // When untracked, conservatively treat as dirty: better to upload more than to show stale content
    range_t *r = find((uintptr_t)addr);
    if (atomic_load(&r->latest) <= stamp) return 0;   // TOTK: nothing in the whole pool was written since the stamp (no per-page scan)
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    return pages_since(r, p, q, stamp);   // The generation of the last write before upload is <= the stamp; any later write must be > the stamp
}
/* TOTK: nvn_dirty_tracked and nvn_dirty_since in one range lookup (they looked the range up twice), and the page scan
   skips each 64-page chunk whose highest generation is at or below the stamp. Returns -1 when untracked (nvn_dirty_tracked
   0), else what nvn_dirty_since returns. Same answer as the page scan: every page generation store is followed by raising
   its chunk's value before latest and before the second "now" step (unprotect_page), and generations only grow. */
int nvn_dirty_check(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    range_t *r = find((uintptr_t)addr);
    if (!r || (uintptr_t)addr + len > r->end) return -1;
    if (atomic_load(&r->latest) <= stamp) return 0;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    return pages_since(r, p, q, stamp);
}
#endif
#if defined(GAME_BOTW)
/* BOTW versions of the functions above: a pool's pages can lie in several ranges (its own gaps and the neighbours' shared
   pages, see g_rmem), so the lookups walk range by range; a page in no live range makes the span untracked. */
static void range_put(int slot, int n, uintptr_t base, uintptr_t end, const void *mem) {   // under g_slot_mu
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    size_t np = (end - base) >> g_shift;
    size_t nc = ((np - 1) >> CHUNK_SHIFT) + 1;   // cgen: the shared fault path raises it (unprotect_page)
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen, *cgen = r->cgen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); cgen = calloc(nc, sizeof *cgen);
        if (!state || !gen || !cgen) die("dirty page table allocation failed (%zu pages)", np);
        r->cap = np;   // the old arrays are not freed (see nvn_dirty_add_range above)
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset((void *)cgen, 0, nc * sizeof *cgen);
    }
    r->base = base; r->end = end; r->state = state; r->gen = gen; r->cgen = cgen; atomic_store(&r->latest, 0);
    atomic_store(&r->fence, 0); atomic_store(&r->gstamp, 0); r->owner = (uintptr_t)mem; r->osize = 0;
    g_rmem[slot] = (uintptr_t)mem; atomic_store(&g_rdying[slot], 0);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
}
static void ov_log(const char *what, _Atomic uint64_t *c, const void *mem, size_t size) {
    uint64_t k = atomic_fetch_add(c, 1) + 1;
    if (k <= 3 || !(k & (k - 1))) LOG("dirty tracking: pool %p+%#zx overlaps tracked ranges: %s (%llu times)", mem, size, what, (unsigned long long)k);
}
void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dirty_fast_init(); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); g_drec_on = getenv("DM_NVN_TILELOG") != NULL; }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    uintptr_t nb = (uintptr_t)mem & ~(g_pg - 1), ne = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    /* the live ranges this pool overlaps, by base; the pool gets one range for each gap between them */
    enum { MAX_OV = 16 }; uintptr_t ob[MAX_OV], oe[MAX_OV]; int no = 0, same = 0;
    for (int i = 0; i < n; i++) {
        range_t *r = &g_r[i];
        if (!atomic_load_explicit(&r->live, memory_order_acquire) || ne <= r->base || nb >= r->end) continue;
        same |= g_rmem[i] == (uintptr_t)mem;
        if (no == MAX_OV) { ov_log("too many overlapping ranges, not tracked separately", &g_ov_many, mem, size); pthread_mutex_unlock(&g_slot_mu); return; }
        int k = no++; while (k && ob[k - 1] > r->base) { ob[k] = ob[k - 1]; oe[k] = oe[k - 1]; k--; }
        ob[k] = r->base; oe[k] = r->end;
    }
    if (same) ov_log("same start as another pool", &g_ov_same, mem, size);
    int made = 0;
    for (int k = 0; k <= no; k++) {
        uintptr_t gb = k ? oe[k - 1] : nb, ge = k < no ? ob[k] : ne;
        if (gb < nb) gb = nb;
        if (ge > ne) ge = ne;
        if (gb >= ge) continue;
        int slot = -1;
        for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
        if (slot < 0) {
            if (n >= MAX_RANGE) {
                int expected = 0;
                if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("[red] dirty tracking ranges full (%d), pool %p not tracked", MAX_RANGE, mem);
                break;
            }
            slot = n;
        }
        range_put(slot, n, gb, ge, mem); made++;
        if (slot == n) n++;
    }
    if (no) ov_log(made ? "only its own pages are tracked" : "wholly inside tracked ranges, tracked by them", made ? &g_ov_split : &g_ov_inside, mem, size);
    pthread_mutex_unlock(&g_slot_mu);
}
void nvn_dirty_remove_range(const void *mem, size_t size) {   // size: unused here (ranges are found by g_rmem)
    (void)size;
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr), found = 0;
    for (int i = 0; i < n; i++) {
        range_t *r = &g_r[i];
        if (!atomic_load_explicit(&r->live, memory_order_acquire) || g_rmem[i] != (uintptr_t)mem) continue;
        atomic_store(&g_rdying[i], 1);   // before the unprotect: see protect_seg
        prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); found++;
    }
    /* "now" moves on: a texture of a neighbour pool that crossed into the removed range is untracked from here on, and the
       replay's shortcut (nothing written since the last look = clean, nvn_mtl.m) must look again */
    if (found) atomic_fetch_add(&g_now, 1);
    if (!found) { uint64_t k = atomic_fetch_add(&g_rm_none, 1) + 1;
        if (k <= 3 || !(k & (k - 1))) LOG("dirty tracking: destroyed pool %p has no range of its own; other pools' ranges stay (%llu times)", mem, (unsigned long long)k); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    do { range_t *r = find(a); if (!r) return 0; a = r->end; } while (a < e);
    return 1;
}
/* Set read-only the pages of [a, e) in range r (as nvn_dirty_protect above). A run is set read-only only if the range is not
   being removed: the remover sets g_rdying before it unprotects, so either this sees the flag or the remover meets the
   run's pages BUSY, waits, and unprotects them. */
static void protect_seg(range_t *r, uintptr_t a, uintptr_t e) {
    uintptr_t p = (a - r->base) >> g_shift, q = (e - 1 - r->base) >> g_shift, run = 0; int open = 0;
    _Atomic uint8_t *dying = &g_rdying[r - g_r];
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) {
            int keep = !atomic_load(dying);
            if (keep) mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ);
            for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], keep ? PG_PROT : PG_NONE);
            open = 0; }
        if (!took && st == PG_BUSY && busy_wait(r, i)) i--;   // this page again
    }
}
void nvn_dirty_protect(const void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) { range_t *r = find(a); if (!r) return; uintptr_t stop = e < r->end ? e : r->end; protect_seg(r, a, stop); a = stop; }
}
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) {
        range_t *r = find(a); if (!r) return 1;   // untracked: dirty
        uintptr_t stop = e < r->end ? e : r->end;
        if (atomic_load(&r->latest) > stamp && pages_since(r, (a - r->base) >> g_shift, (stop - 1 - r->base) >> g_shift, stamp)) return 1;
        a = stop;
    }
    return 0;
}
/* nvn_dirty_check (above) for BOTW: -1 when untracked, else nvn_dirty_since, range by range. */
int nvn_dirty_check(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    if (!nvn_dirty_tracked(addr, len)) return -1;
    return nvn_dirty_since(addr, len, stamp);
}
/* GPU writes (as above), over up to 4 ranges */
typedef struct { int n; range_t *r[4]; uintptr_t p0[4], p1[4]; uint8_t *was[4]; } gpu_write_tok;
void nvn_dirty_gpu_write_end(void *tok);
void *nvn_dirty_gpu_write_begin(void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return NULL;
    gpu_write_tok *t = calloc(1, sizeof *t); if (!t) return NULL;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) {
        range_t *r = find(a); uintptr_t stop = r ? (e < r->end ? e : r->end) : e;
        uintptr_t p0 = r ? (a - r->base) >> g_shift : 0, p1 = r ? (stop - 1 - r->base) >> g_shift : 0;
        uint8_t *was = r && t->n < 4 ? calloc(p1 - p0 + 1, 1) : NULL;
        if (!was) { nvn_dirty_gpu_write_end(t); return NULL; }   // a range went away meanwhile, or more than 4: writes fault as usual
        int k = t->n++; t->r[k] = r; t->p0[k] = p0; t->p1[k] = p1; t->was[k] = was;
        for (uintptr_t p = p0; p <= p1; p++) {
            for (;;) { uint8_t st = PG_PROT;
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE); was[p - p0] = 1; break; }
                if (st != PG_BUSY || !busy_wait(r, p)) break; }
        }
        a = stop;
    }
    return t;
}
void nvn_dirty_gpu_write_end(void *tok) {
    gpu_write_tok *t = tok; if (!t) return;
    for (int k = 0; k < t->n; k++) {
        range_t *r = t->r[k]; int keep = !atomic_load(&g_rdying[r - g_r]);   // a range being removed gets its pages back writable
        for (uintptr_t p = t->p0[k]; p <= t->p1[k]; p++) if (t->was[k][p - t->p0[k]]) { if (keep) mp(r->base + (p << g_shift), g_pg, PROT_READ); atomic_store(&r->state[p], keep ? PG_PROT : PG_NONE); }
        free(t->was[k]);
    }
    free(t);
}
#endif
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
