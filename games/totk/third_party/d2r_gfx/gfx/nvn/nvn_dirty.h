// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem, size_t size);        // Pool destruction (the storage and size it was added with): restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // Set the covered pages read-only
uint64_t nvn_dirty_now(void);                                      // Current generation: taken before upload, as the stamp of that content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp);
int nvn_dirty_check(const void *addr, size_t len, uint64_t stamp);  // TOTK: -1 untracked, else nvn_dirty_since (one range lookup, chunk-level skip)
void *nvn_dirty_gpu_write_begin(void *addr, size_t len);   // write guest memory on behalf of the GPU: not counted as a CPU write
void nvn_dirty_gpu_write_end(void *tok);
typedef struct { uintptr_t page; uint64_t pc, lr; uint32_t src; } nvn_dirty_rec;   // src 0: guest write (fault), 1: host write
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max);   // latest unprotections in [lo, hi) (DM_NVN_TILELOG)
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // Currently live slots, slot table high-water mark, total number of registrations
// TOTK frames in flight: CPU writes wait for the GPU submissions that still read the page (see nvn_dirty.c)
void nvn_dirty_guard_init(uint64_t (*done)(void), int audit);      // done: highest completed submission fence (atomic read); audit: count would-be waits, never wait (unsafe)
int nvn_dirty_guard(const void *addr, size_t len, uint64_t fence);  // the submission ending with fence reads [addr, addr+len) zero-copy: 0 = untracked, cannot be guarded
void nvn_dirty_guard_nowait(void);                                 // this thread never waits in the guard (GPU completion callbacks)
uint64_t nvn_dirty_range_fence(const void *mem);                   // newest guarded fence of the range holding mem (0: none)
int nvn_dirty_span_fence(const void *addr, size_t len, uint64_t *fence);  // newest guarded fence over every range the span lies in; 0 = part of it untracked
void nvn_dirty_add_stats(uint64_t v[3]);                           // cumulative pool registrations: clipped (shares a host page), covered by other ranges, untracked
void nvn_dirty_guard_stats(uint64_t v[6]);                         // cumulative: waits, ns waited, waits timed out, guard calls, calls with nothing to protect, untracked ranges
void nvn_dirty_guard_audit(uint64_t *n, uint64_t sample[8]);      // audit mode: writes that would have waited, last pages written
void nvn_dirty_fast_stats(int *on, uint64_t *blk_skips, uint64_t *hash_hits, uint64_t *scans);   // TOTK_DIRTY_FAST: chunks skipped by nvn_dirty_since / nvn_dirty_check, range lookups answered by the page table / by the scan
