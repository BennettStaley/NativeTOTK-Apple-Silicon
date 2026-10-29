/* NVN quad strips (NVN_DRAW_PRIMITIVE_QUAD_STRIP, prim 8; upstream #1216): each successive pair of vertices completes a
   quad (a, b, d, c). Metal has no quads, so each quad becomes two triangles. Both keep d as their last vertex, preserving
   flat (last-vertex) inputs. Hero's Path draws its route with them. Pure C, no Metal: tests/test_quad_strip.c checks it. */
#ifndef NVN_QUAD_STRIP_H
#define NVN_QUAD_STRIP_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t nvn_quad_strip_quads(uint32_t vertices) {
    return vertices >= 4 ? vertices / 2 - 1 : 0;   // an odd last vertex completes nothing
}

/* The shared index list of a non-indexed strip of `quads` quads, starting at vertex 0 (the draw's first vertex is the
   base vertex): batches of it keep the vertex IDs of the original strip. */
static inline void nvn_quad_strip_indices(uint32_t *indices, uint32_t quads) {
    for (uint32_t q = 0; q < quads; q++) {
        uint32_t a = q * 2;
        indices[q * 6 + 0] = a;
        indices[q * 6 + 1] = a + 1;
        indices[q * 6 + 2] = a + 3;
        indices[q * 6 + 3] = a + 2;
        indices[q * 6 + 4] = a;
        indices[q * 6 + 5] = a + 3;
    }
}

/* Expands host-visible 8/16/32-bit indices into triangle indices (32-bit), splitting at the type's restart index. An odd
   tail is dropped in each strip on its own rather than joined to the next strip. Returns the indices written (at most
   nvn_quad_strip_quads(count) * 6). */
static inline size_t nvn_quad_strip_indexed(uint32_t *out, const void *input, uint32_t count, unsigned width) {
    const uint8_t *bytes = input;
    uint32_t pair[4], used = 0;
    size_t written = 0;
    uint32_t restart = width == 1 ? UINT8_MAX : width == 2 ? UINT16_MAX : UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t value = 0;
        memcpy(&value, bytes + (size_t)i * width, width);
        if (value == restart) { used = 0; continue; }
        pair[used++] = value;
        if (used != 4) continue;
        out[written++] = pair[0]; out[written++] = pair[1]; out[written++] = pair[3];
        out[written++] = pair[2]; out[written++] = pair[0]; out[written++] = pair[3];
        pair[0] = pair[2]; pair[1] = pair[3]; used = 2;
    }
    return written;
}
#endif
