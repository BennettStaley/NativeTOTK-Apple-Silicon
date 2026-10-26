// Metal execution layer of the NVN null device (C interface): memory pool = zero-copy MTLBuffer, submit = MTLCommandBuffer, sync = MTLSharedEvent.
// Ordering convention (to avoid repeating the GO_MAP_SHARE corrupted-picture mistake of the x86 route): every visible effect of "GPU completion" (sync object ready, event values, counters)
// happens only after the Metal command buffer has actually completed; CPU-side waits look only at the completed value of the MTLSharedEvent.
#pragma once
int mtl_probe_writeback(void);
#include <stdint.h>
#include <stddef.h>

void mtl_init(void);
void bf_want_note(const char *key);

// Memory pool: [mem, mem+size) is expanded outward to page boundaries to build a zero-copy buffer; look up the buffer and offset by GPU address (= CPU address)
void mtl_pool_add(const void *mem, uint64_t size);
void mtl_pool_del(const void *mem);
void mtl_queue_finish(void);   // nvnQueueFinish: submitted GPU work, not queued presents (TOTK_FINISH_PRESENTS=1: both)
void mtl_present_flush_mem(const void *mem, uint64_t size, uint64_t serial);   // queued presents from that memory / texture reach the GPU first
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);   // 1 on success; buf is a (__bridge) id<MTLBuffer>

typedef struct {
    uint32_t pso, cps, shaders, dss;
    uint32_t view_nonnull, view_slots, live_tex, sview_nonnull;
    uint32_t pools;
    uint64_t pool_adds;
    uint32_t c1_bufs; uint64_t c1_alloc, c1_data;   // Shader c1 constant segment: buffer count (4 MiB blocks + dedicated 64 KiB), allocated bytes, actual bytes
} mtl_res_count;
void mtl_res_counts(uint32_t tick, const char *why);   // Print one line of resource counts (the host calls it per tick / event, see dm_res_report_register in dm.h)
void mtl_res_snapshot(mtl_res_count *out);

// One submit: begin -> several operations -> commit (returns the fence value at which this submit completes)
void mtl_sub_begin(void);
/* TOTK: uniform contents as of the submit call. capture runs in nvnQueueSubmitCommands (game thread) when the replay is
   handed to the translation thread; snapshot_cap builds the submission's copies at the start of its replay (cap may be
   NULL: copy from memory now) and frees cap. */
void *mtl_sub_capture_ubos(const uint64_t *h, int n);
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n);               // Buffer copy in GPU ordering
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v);   // Write the event value after completion (0 write / 1 add)
void mtl_sub_counter(uint64_t addr, uint32_t type);                       // Write the counter record after completion
uint64_t mtl_sub_commit(void);
int mtl_probe_vertex_streams(void);
int mtl_probe_vertex_input_types(void);
int mtl_probe_fragment_output_types(void);
int mtl_probe_pipeline_archive(void);
int mtl_probe_arg_epoch(void);
int mtl_probe_tex_slots(void);
int mtl_probe_sampler_border(void);
int mtl_probe_pool_retire(void);
int mtl_probe_anomaly(void);
int mtl_probe_widen(void);     // TOTK: standalone probes of isolated GPU paths (die on mismatch, return checked count)
int mtl_probe_blockrt(void);
int mtl_probe_midclear(void);
int mtl_probe_srgb_view(void);
int mtl_probe_storage_view(void);
int mtl_probe_compressed_alias(void);
int mtl_probe_linear_rt_upload(void);
int mtl_probe_texels_region(void);
int mtl_probe_texture_retire(void);
int mtl_probe_ds_clear(void);
int mtl_probe_ub_rename(void);
int mtl_probe_ubo_snapshot(void);
int mtl_probe_drv_c0(void);
int mtl_probe_dispatch_chain(void);
int mtl_probe_copy_alias(void);
int mtl_probe_layer_merge(void);
int mtl_probe_cube_merge(void);
void mtl_cube_merge_stats(uint64_t v[4]);
int mtl_probe_winding(void);
int mtl_probe_depth_output(void);
int mtl_probe_tile(void);

uint64_t mtl_last_fence(void);                       // Fence value of the most recent submit (0 = never submitted)
// TOTK frames in flight (nvn_mtl.m): submission flags by class and the counters behind the periodic [fif] line
enum { FIF_FL_OCCL = 1, FIF_FL_UNG = 2, FIF_FL_STAT = 4, FIF_FL_VIRT = 8, FIF_FL_OVER = 16, FIF_FL_NOREC = 32 };
#define MTL_FIF_NSTAT 17
uint32_t mtl_fence_flags(uint64_t after, uint64_t upto);
void mtl_fif_stats(uint64_t v[MTL_FIF_NSTAT], uint64_t sample[4][2]);
int mtl_wait(uint64_t fence, uint64_t timeout_ns);   // Returns 1 when the GPU has completed up to fence, 0 on timeout
void mtl_finish(void);                               // Wait for all submitted commands to complete
void mtl_drain(void);                                // Wait for all commands on the queue (including presents) to complete: submit an empty command buffer and wait for it
void mtl_xv_window_tex(uint64_t serial);   // Register a window (swapchain) texture: the cross-submit validity ledger never collects it
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy);   // Drag-follow image for shop items: when on=1, the icon of the item that covers screen point (px,py) and sits on the shop shelf grid `grid` (top-left x,y, width and height of one cell) is translated to center (cx,cy) and drawn once more at the end of the UI pass (normalized, top-left origin); callable from any thread
int mtl_ui_ghost_rect(float r[4]);   // Screen rectangle (normalized) of the item recognized above; returns 0 if not recognized yet

int mtl_probe_upload_batch(void);
int mtl_probe_vertex_wait(void);
int mtl_probe_wb_batch(void);
