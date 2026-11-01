/*
 * hakuX HLE GPU — strip batching.
 *
 * A title draws its meshes as many short triangle strips with nothing
 * changing between them (Blinx: ~500k strip draws a second, state every ~20
 * draws), and the GPU pays ~1.7 us per draw whatever it renders. A run of
 * strip (or fan) draws that records nothing in between becomes one indexed
 * draw: each strip's vertices as 32-bit indices in a per-frame-slot buffer,
 * the strips separated by primitive restart, so the GPU draws exactly the
 * same triangles. vertexOffset is the run's first vertex, so gl_VertexIndex
 * keeps the value each draw gave it.
 *
 * The held draws are recorded before anything else: every recording command
 * in the device table goes through a wrapper (batch_wrap.h) that records them
 * first. A run of one is recorded as the plain draw it was.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/vk/batch.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/vk/vkdev.h"
#include "hw/xbox/hle/front/cmd.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#endif

void hle_draw_ibuf_lost(void);

/* Indices per frame slot (4 MiB each, one slot per command buffer in
 * flight): a run that would not fit is recorded as it stands and a new one
 * starts. */
#define SLOT_INDICES (1u << 20)
#define SLOTS        HLE_FRAMES
#define RESTART      0xFFFFFFFFu

static struct VolkDeviceTable R;    /* the real entry points */

static struct {
    bool on;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t *map;
    uint32_t slot_base, pos;        /* the current slot's first index, the next free */
    /* the run held */
    VkCommandBuffer cb;             /* NULL: none */
    VkPrimitiveTopology prim;
    uint32_t first, count;          /* its first draw, as it came */
    uint32_t base;                  /* vertexOffset of the merged draw */
    uint32_t istart, n;             /* its indices (absolute index, count); n = 0: not written */
    uint32_t draws;
    uint64_t batches, merged;
} g_b;

/* Records the run held on cb: the plain draw for a run of one. */
static void flush(void)
{
    VkCommandBuffer cb = g_b.cb;
    g_b.cb = VK_NULL_HANDLE;
    if (g_b.draws == 1) {
        R.vkCmdDraw(cb, g_b.count, 1, g_b.first, 0);
        return;
    }
    R.vkCmdBindIndexBuffer(cb, g_b.buf, (VkDeviceSize)g_b.istart * 4, VK_INDEX_TYPE_UINT32);
    R.vkCmdSetPrimitiveRestartEnable(cb, VK_TRUE);
    R.vkCmdDrawIndexed(cb, g_b.n, 1, 0, (int32_t)g_b.base, 0);
    R.vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    hle_draw_ibuf_lost();               /* the draw path's index buffer is no longer bound */
    g_b.batches++;
    g_b.merged += g_b.draws;
}

static inline void merge_before(VkCommandBuffer cb);

/* Anything recorded on cb comes after a held strip run */
static inline void strip_before(VkCommandBuffer cb)
{
    if (g_b.cb == cb && cb) {
        flush();
    }
}

/* ... and after a held merge run and the state it held (batch.h) */
static inline void batch_before(VkCommandBuffer cb)
{
    strip_before(cb);
    merge_before(cb);
}

/* ---- draw merging (batch.h) ---------------------------------------------- */

static HleMergeEnv E;

/* What a draw sees bound: as recorded (real) or as the draw path set it
 * since (absorbed while a run is held). */
typedef struct MState {
    VkPipeline pipe;
    bool pc_ok, s1_ok, vb_ok, ib_ok, vi_ok;
    VkShaderStageFlags pc_stages;
    uint8_t pc[256];
    VkDescriptorSet s1;
    uint32_t s1_off;
    uint32_t nvb;
    VkBuffer vb[HLE_DRAW_MAX_BIND];
    VkDeviceSize vb_off[HLE_DRAW_MAX_BIND];
    VkBuffer ib;
    VkDeviceSize ib_off;
    VkIndexType ib_type;
    uint32_t nbd, nad;
    VkVertexInputBindingDescription2EXT bd[HLE_DRAW_MAX_BIND];
    VkVertexInputAttributeDescription2EXT ad[HLE_DRAW_ATTRS];
} MState;

typedef struct MDraw {
    uint8_t pc[256];
    uint32_t s1_off;
    VkDeviceSize vb_off[HLE_DRAW_MAX_BIND];
    VkDeviceSize ib_off;
    bool indexed;
    uint32_t count, first;              /* indices (firstIndex) or vertices (firstVertex) */
    int32_t voff;                       /* vertexOffset */
    const uint16_t *idx;
    int64_t lo, hi;                     /* its vertices in the run's space */
} MDraw;

static struct {
    bool on;
    VkCommandBuffer cb;                 /* the state below is this one's */
    MState L;                           /* as the draw path set it */
    bool absorbed;                      /* L differs from what is recorded */
    HleMergeNext next;
    bool next_ok;
    /* the run held: its first draw's state is recorded (L then) */
    uint32_t n;
    VkPipeline merged;
    bool indexed, strip;
    MDraw d[HLE_MERGE_MAX];
    int64_t lo, hi;                     /* the run's span */
    uint32_t s1_lo, s1_hi;              /* its set 1 blocks */
    uint32_t nidx;                      /* indices the merged draw would have */
    uint64_t merged_draws, draws_in;
    int dbg;                            /* debug.hakux.mergedbg: 1 runs replayed, not merged */
    unsigned frames;
} g_m;

/* Commands on another command buffer (the uploads' in front of the frame's)
 * go through as they come. */
static inline bool m_mine(VkCommandBuffer cb)
{
    return g_m.on && cb && cb == g_m.cb;
}

void hle_merge_cb(VkCommandBuffer cb)
{
#ifdef __ANDROID__
    if (g_m.frames++ % 60 == 0) {       /* debug: live */
        char v[PROP_VALUE_MAX] = "";
        hle_prop_get("debug.hakux.mergedbg", v);
        g_m.dbg = atoi(v);
    }
#endif
    memset(&g_m.L, 0, sizeof(g_m.L));   /* nothing bound in a new recording */
    g_m.cb = cb;
    g_m.absorbed = false;
    g_m.n = 0;
    g_m.next_ok = false;
}

/* Records what L holds over what the held run left recorded. */
static void m_sync(VkCommandBuffer cb)
{
    const MState *L = &g_m.L;
    if (L->pipe) {
        R.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, L->pipe);
    }
    if (L->vi_ok) {
        R.vkCmdSetVertexInputEXT(cb, L->nbd, L->bd, L->nad, L->ad);
    }
    if (L->pc_ok) {
        R.vkCmdPushConstants(cb, E.layout, L->pc_stages, 0, sizeof(L->pc), L->pc);
    }
    if (L->s1_ok) {
        R.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, E.layout, 1, 1, &L->s1, 1,
                                  &L->s1_off);
    }
    if (L->vb_ok) {
        R.vkCmdBindVertexBuffers(cb, 0, L->nvb, L->vb, L->vb_off);
    }
    if (L->ib_ok) {
        R.vkCmdBindIndexBuffer(cb, L->ib, L->ib_off, L->ib_type);
    }
    g_m.absorbed = false;
}

static void m_draw(VkCommandBuffer cb, const MDraw *d)
{
    if (d->indexed) {
        R.vkCmdDrawIndexed(cb, d->count, 1, d->first, d->voff, 0);
    } else {
        R.vkCmdDraw(cb, d->count, 1, d->first, 0);
    }
}

/* The held run as its draws, each with its own state */
static void m_replay(VkCommandBuffer cb)
{
    const MState *L = &g_m.L;
    m_draw(cb, &g_m.d[0]);              /* its state is recorded */
    for (uint32_t k = 1; k < g_m.n; k++) {
        const MDraw *d = &g_m.d[k];
        if (L->pc_ok) {
            R.vkCmdPushConstants(cb, E.layout, L->pc_stages, 0, sizeof(d->pc), d->pc);
        }
        if (L->s1_ok) {
            R.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, E.layout, 1, 1, &L->s1,
                                      1, &d->s1_off);
        }
        if (L->vb_ok) {
            R.vkCmdBindVertexBuffers(cb, 0, L->nvb, L->vb, d->vb_off);
        }
        if (L->ib_ok) {
            R.vkCmdBindIndexBuffer(cb, L->ib, d->ib_off, L->ib_type);
        }
        m_draw(cb, d);
    }
}

/* The held run as one draw of the merged pipeline; false: it can't be */
static bool m_merged(VkCommandBuffer cb)
{
    const MState *L = &g_m.L;
    const uint32_t n = g_m.n;
    const int64_t base = g_m.lo;
    const uint32_t span = (uint32_t)(g_m.hi - base);
    /* indices and the draw numbers (a word-aligned run of bytes) */
    const uint32_t id_words = (span * E.id_bytes + 3) / 4;
    if (g_b.pos + g_m.nidx + id_words > SLOT_INDICES) {
        return false;
    }
    /* the records: each draw's push block and set 1 base (vec4s) */
    static uint8_t recs[HLE_MERGE_MAX * HLE_MERGE_REC];
    const uint32_t s1_base = L->s1_ok ? g_m.s1_lo : 0;
    for (uint32_t k = 0; k < n; k++) {
        uint8_t *r = recs + k * HLE_MERGE_REC;
        memcpy(r, g_m.d[k].pc, 256);
        const uint32_t cbase = L->s1_ok ? (g_m.d[k].s1_off - s1_base) / 16 : 0;
        memcpy(r + 256, &cbase, 4);
        memset(r + 260, 0, HLE_MERGE_REC - 260);
    }
    uint32_t roff;
    if (!E.push_put(recs, n * HLE_MERGE_REC, &roff)) {
        return false;
    }
    uint32_t *ip = g_b.map + g_b.slot_base + g_b.pos;
    const uint32_t istart = g_b.slot_base + g_b.pos;
    uint32_t ni = 0;
    for (uint32_t k = 0; k < n; k++) {
        const MDraw *d = &g_m.d[k];
        const int64_t shift = d->lo - base;     /* the draw's first vertex in the merged space */
        if (k && g_m.strip) {
            ip[ni++] = RESTART;
        }
        if (d->indexed) {
            /* The arena is write-combined: read by element, each index cost
             * a trip to memory (half the render thread); in bulk, then from
             * the cache. */
            static uint16_t tmp[4096];
            for (uint32_t done = 0; done < d->count;) {
                const uint32_t c = MIN(d->count - done, (uint32_t)ARRAY_SIZE(tmp));
                memcpy(tmp, d->idx + done, c * 2);
                for (uint32_t i = 0; i < c; i++) {
                    ip[ni++] = (uint32_t)(shift + tmp[i]);
                }
                done += c;
            }
        } else {
            for (uint32_t i = 0; i < d->count; i++) {
                ip[ni++] = (uint32_t)(shift + i);
            }
        }
    }
    uint8_t *ids = (uint8_t *)(g_b.map + g_b.slot_base + g_b.pos + g_m.nidx);
    const VkDeviceSize ids_off = (VkDeviceSize)(g_b.slot_base + g_b.pos + g_m.nidx) * 4;
    for (uint32_t k = 0; k < n; k++) {
        const MDraw *d = &g_m.d[k];
        const uint32_t a = (uint32_t)(d->lo - base), b = (uint32_t)(d->hi - base);
        if (E.id_bytes == 1) {
            memset(ids + a, (int)k, b - a);
        } else {
            for (uint32_t v = a; v < b; v++) {
                memcpy(ids + (size_t)v * E.id_bytes, &k, 4);
            }
        }
    }
    g_b.pos += g_m.nidx + id_words;

    /* the vertex layout with the draw number's stream after the bindings */
    VkVertexInputBindingDescription2EXT bd[HLE_DRAW_MAX_BIND + 1];
    VkVertexInputAttributeDescription2EXT ad[HLE_DRAW_ATTRS + 1];
    memcpy(bd, L->bd, sizeof(bd[0]) * L->nbd);
    memcpy(ad, L->ad, sizeof(ad[0]) * L->nad);
    bd[L->nbd] = (VkVertexInputBindingDescription2EXT){
        .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
        .binding = L->nbd, .stride = E.id_bytes, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        .divisor = 1 };
    ad[L->nad] = (VkVertexInputAttributeDescription2EXT){
        .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
        .location = HLE_MERGE_LOC, .binding = L->nbd, .format = E.id_fmt, .offset = 0 };
    VkBuffer vb[HLE_DRAW_MAX_BIND + 1];
    VkDeviceSize vo[HLE_DRAW_MAX_BIND + 1];
    for (uint32_t i = 0; i < L->nvb; i++) {
        vb[i] = L->vb[i];
        vo[i] = (VkDeviceSize)((int64_t)g_m.d[0].vb_off[i] + base * (int64_t)L->bd[i].stride);
    }
    vb[L->nvb] = g_b.buf;
    vo[L->nvb] = ids_off;

    R.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_m.merged);
    R.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, E.mlayout, 3, 1, &E.pset, 1,
                              &roff);
    if (L->s1_ok) {
        R.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, E.mlayout, 1, 1, &E.mset, 1,
                                  &s1_base);
    }
    R.vkCmdSetVertexInputEXT(cb, L->nbd + 1, bd, L->nad + 1, ad);
    R.vkCmdBindVertexBuffers(cb, 0, L->nvb + 1, vb, vo);
    R.vkCmdBindIndexBuffer(cb, g_b.buf, (VkDeviceSize)istart * 4, VK_INDEX_TYPE_UINT32);
    if (E.index_range) {
        E.index_range(cb, 0, ni, 0, span - 1);
    }
    if (g_m.strip) {
        R.vkCmdSetPrimitiveRestartEnable(cb, VK_TRUE);
    }
    R.vkCmdDrawIndexed(cb, ni, 1, 0, 0, 0);
    if (g_m.strip) {
        R.vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    }
    g_m.merged_draws++;
    g_m.draws_in += n;
    return true;
}

/* Records the held run (merged, else draw by draw), then what the draw
 * path set since. */
static void m_flush(void)
{
    VkCommandBuffer cb = g_m.cb;
    if (g_m.n) {
        if (g_m.n == 1) {
            m_draw(cb, &g_m.d[0]);
        } else if (g_m.dbg == 1 || !m_merged(cb)) {
            m_replay(cb);
        }
        g_m.n = 0;
        g_m.absorbed = true;            /* the merged draw's state, or the last draw's */
    }
    if (g_m.absorbed) {
        m_sync(cb);
    }
}

/* Anything recorded on cb after the held run */
static inline void merge_before(VkCommandBuffer cb)
{
    if (m_mine(cb) && (g_m.n || g_m.absorbed)) {
        m_flush();
    }
}

void hle_merge_next(const HleMergeNext *m)
{
    g_m.next = *m;
    g_m.next_ok = g_m.on && m->merged;
}

void hle_merge_stats(uint64_t *merged, uint64_t *draws)
{
    *merged = g_m.merged_draws;
    *draws = g_m.draws_in;
}

/* The draw (its state in L) joins the held run: its vertices one space with
 * the run's, apart from theirs, within reach of its constants' window. */
static bool m_join(const MDraw *d)
{
    const MState *L = &g_m.L;
    const MDraw *d0 = &g_m.d[0];
    if (g_m.n >= HLE_MERGE_MAX || d->indexed != g_m.indexed || g_m.next.strip != g_m.strip) {
        return false;
    }
    /* the delta in vertices, the same for every binding */
    int64_t delta = 0;
    bool have = false;
    for (uint32_t i = 0; i < L->nvb; i++) {
        const int64_t diff = (int64_t)d->vb_off[i] - (int64_t)d0->vb_off[i];
        const int64_t st = i < L->nbd ? L->bd[i].stride : 0;
        if (!st) {
            if (diff) {
                return false;
            }
            continue;
        }
        if (diff % st || (have && diff / st != delta)) {
            return false;
        }
        delta = diff / st;
        have = true;
    }
    const int64_t lo = d->lo + delta, hi = d->hi + delta;
    for (uint32_t k = 0; k < g_m.n; k++) {
        if (lo < g_m.d[k].hi && g_m.d[k].lo < hi) {
            return false;
        }
    }
    const int64_t nlo = MIN(lo, g_m.lo), nhi = MAX(hi, g_m.hi);
    if (nhi - nlo > HLE_MERGE_SPAN) {
        return false;
    }
    for (uint32_t i = 0; i < L->nvb; i++) {     /* the merged binding's start in the buffer */
        const int64_t st = i < L->nbd ? L->bd[i].stride : 0;
        if ((int64_t)d0->vb_off[i] + nlo * st < 0) {
            return false;
        }
    }
    uint32_t s1_lo = g_m.s1_lo, s1_hi = g_m.s1_hi;
    if (L->s1_ok) {
        s1_lo = MIN(s1_lo, d->s1_off);
        s1_hi = MAX(s1_hi, d->s1_off + E.blk);
        if (s1_hi - s1_lo > E.win || (uint64_t)s1_lo + E.win > E.arena_size) {
            return false;
        }
    }
    MDraw *e = &g_m.d[g_m.n++];
    *e = *d;
    e->lo = lo;
    e->hi = hi;
    g_m.lo = nlo;
    g_m.hi = nhi;
    g_m.s1_lo = s1_lo;
    g_m.s1_hi = s1_hi;
    g_m.nidx += d->count + (g_m.strip ? 1 : 0);
    return true;
}

/* A draw: held (a run's first, or joining the run), else recorded */
static void m_draw_cmd(VkCommandBuffer cb, bool indexed, uint32_t count, uint32_t inst,
                       uint32_t first, int32_t voff, uint32_t first_inst)
{
    strip_before(cb);
    if (!m_mine(cb)) {
        if (indexed) {
            R.vkCmdDrawIndexed(cb, count, inst, first, voff, first_inst);
        } else {
            R.vkCmdDraw(cb, count, inst, first, first_inst);
        }
        return;
    }
    const bool ok = g_m.next_ok && inst == 1 && first_inst == 0 && count &&
                    g_m.L.vi_ok && g_m.L.vb_ok && g_m.L.pc_ok &&
                    g_m.L.nbd < HLE_DRAW_MAX_BIND && g_m.L.nad < HLE_DRAW_ATTRS &&
                    (!indexed || (g_m.L.ib_ok && g_m.L.ib_type == VK_INDEX_TYPE_UINT16 &&
                                  g_m.next.vcount && g_m.next.indices));
    const HleMergeNext next = g_m.next;
    g_m.next_ok = false;
    g_m.next.strip = next.strip;        /* (m_join reads it) */
    MDraw d;
    if (ok) {
        memcpy(d.pc, g_m.L.pc, sizeof(d.pc));
        d.s1_off = g_m.L.s1_off;
        memcpy(d.vb_off, g_m.L.vb_off, sizeof(d.vb_off[0]) * g_m.L.nvb);
        d.ib_off = g_m.L.ib_off;
        d.indexed = indexed;
        d.count = count;
        d.first = first;
        d.voff = voff;
        d.idx = indexed ? next.indices : NULL;
        d.lo = indexed ? voff : first;
        d.hi = d.lo + (indexed ? next.vcount : count);
        if (g_m.n && next.merged == g_m.merged && m_join(&d)) {
            return;
        }
    }
    if (g_m.n || g_m.absorbed) {
        m_flush();
    }
    if (!ok) {
        if (indexed) {
            R.vkCmdDrawIndexed(cb, count, inst, first, voff, first_inst);
        } else {
            R.vkCmdDraw(cb, count, inst, first, first_inst);
        }
        return;
    }
    /* a run starts: its state is recorded (nothing absorbed) */
    g_m.d[0] = d;
    g_m.n = 1;
    g_m.merged = next.merged;
    g_m.indexed = indexed;
    g_m.strip = next.strip;
    g_m.lo = d.lo;
    g_m.hi = d.hi;
    g_m.s1_lo = d.s1_off;
    g_m.s1_hi = d.s1_off + E.blk;
    g_m.nidx = count;
}

static VKAPI_ATTR void VKAPI_CALL m_vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount,
                                              uint32_t instanceCount, uint32_t firstVertex,
                                              uint32_t firstInstance)
{
    m_draw_cmd(cb, false, vertexCount, instanceCount, firstVertex, 0, firstInstance);
}

static VKAPI_ATTR void VKAPI_CALL m_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount,
                                                     uint32_t instanceCount, uint32_t firstIndex,
                                                     int32_t vertexOffset, uint32_t firstInstance)
{
    m_draw_cmd(cb, true, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

/* Held while a run is: recorded at its flush (m_sync), else now */
static inline bool m_absorb(VkCommandBuffer cb)
{
    strip_before(cb);
    if (g_m.n) {
        g_m.absorbed = true;
        return true;
    }
    return false;
}

static VKAPI_ATTR void VKAPI_CALL m_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout,
                                                       VkShaderStageFlags stages, uint32_t offset,
                                                       uint32_t size, const void *values)
{
    if (!m_mine(cb)) {
        strip_before(cb);
        R.vkCmdPushConstants(cb, layout, stages, offset, size, values);
        return;
    }
    if (layout != E.layout || offset != 0 || size != sizeof(g_m.L.pc)) {
        merge_before(cb);
        strip_before(cb);
        R.vkCmdPushConstants(cb, layout, stages, offset, size, values);
        g_m.L.pc_ok = false;
        return;
    }
    const bool held = m_absorb(cb);
    memcpy(g_m.L.pc, values, size);
    g_m.L.pc_stages = stages;
    g_m.L.pc_ok = true;
    if (!held) {
        R.vkCmdPushConstants(cb, layout, stages, offset, size, values);
    }
}

static VKAPI_ATTR void VKAPI_CALL m_vkCmdBindDescriptorSets(VkCommandBuffer cb,
                                                            VkPipelineBindPoint bp,
                                                            VkPipelineLayout layout,
                                                            uint32_t first, uint32_t count,
                                                            const VkDescriptorSet *sets,
                                                            uint32_t ndyn, const uint32_t *dyn)
{
    if (!m_mine(cb)) {
        strip_before(cb);
        R.vkCmdBindDescriptorSets(cb, bp, layout, first, count, sets, ndyn, dyn);
        return;
    }
    const bool set1 = bp == VK_PIPELINE_BIND_POINT_GRAPHICS && layout == E.layout &&
                      first == 1 && count == 1 && ndyn == 1;
    if (!set1 || (g_m.n && sets[0] != g_m.L.s1)) {
        merge_before(cb);
        strip_before(cb);
        R.vkCmdBindDescriptorSets(cb, bp, layout, first, count, sets, ndyn, dyn);
        if (set1) {
            g_m.L.s1 = sets[0];
            g_m.L.s1_off = dyn[0];
            g_m.L.s1_ok = true;
        } else if (first <= 1 && first + count > 1) {
            g_m.L.s1_ok = false;
        }
        return;
    }
    const bool held = m_absorb(cb);
    g_m.L.s1 = sets[0];
    g_m.L.s1_off = dyn[0];
    g_m.L.s1_ok = true;
    if (!held) {
        R.vkCmdBindDescriptorSets(cb, bp, layout, first, count, sets, ndyn, dyn);
    }
}

static VKAPI_ATTR void VKAPI_CALL m_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first,
                                                           uint32_t count, const VkBuffer *bufs,
                                                           const VkDeviceSize *offs)
{
    if (!m_mine(cb)) {
        strip_before(cb);
        R.vkCmdBindVertexBuffers(cb, first, count, bufs, offs);
        return;
    }
    bool same = first == 0 && count <= HLE_DRAW_MAX_BIND;
    if (same && g_m.n) {
        same = count == g_m.L.nvb && !memcmp(bufs, g_m.L.vb, sizeof(bufs[0]) * count);
    }
    if (!same) {
        merge_before(cb);
        strip_before(cb);
        R.vkCmdBindVertexBuffers(cb, first, count, bufs, offs);
        g_m.L.vb_ok = false;
        if (first == 0 && count <= HLE_DRAW_MAX_BIND) {
            g_m.L.nvb = count;
            memcpy(g_m.L.vb, bufs, sizeof(bufs[0]) * count);
            memcpy(g_m.L.vb_off, offs, sizeof(offs[0]) * count);
            g_m.L.vb_ok = true;
        }
        return;
    }
    const bool held = m_absorb(cb);
    g_m.L.nvb = count;
    memcpy(g_m.L.vb, bufs, sizeof(bufs[0]) * count);
    memcpy(g_m.L.vb_off, offs, sizeof(offs[0]) * count);
    g_m.L.vb_ok = true;
    if (!held) {
        R.vkCmdBindVertexBuffers(cb, first, count, bufs, offs);
    }
}

static VKAPI_ATTR void VKAPI_CALL m_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf,
                                                         VkDeviceSize off, VkIndexType type)
{
    if (!m_mine(cb)) {
        strip_before(cb);
        R.vkCmdBindIndexBuffer(cb, buf, off, type);
        return;
    }
    if (g_m.n && (buf != g_m.L.ib || type != g_m.L.ib_type)) {
        merge_before(cb);
    }
    const bool held = m_absorb(cb);
    g_m.L.ib = buf;
    g_m.L.ib_off = off;
    g_m.L.ib_type = type;
    g_m.L.ib_ok = true;
    if (!held) {
        R.vkCmdBindIndexBuffer(cb, buf, off, type);
    }
}

static VKAPI_ATTR void VKAPI_CALL m_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp,
                                                      VkPipeline pipe)
{
    batch_before(cb);                   /* the held run draws with the pipeline before */
    R.vkCmdBindPipeline(cb, bp, pipe);
    if (m_mine(cb) && bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        g_m.L.pipe = pipe;
    }
}

static VKAPI_ATTR void VKAPI_CALL m_vkCmdSetVertexInputEXT(
    VkCommandBuffer cb, uint32_t nbd, const VkVertexInputBindingDescription2EXT *bd, uint32_t nad,
    const VkVertexInputAttributeDescription2EXT *ad)
{
    batch_before(cb);                   /* the held run reads the layout before */
    R.vkCmdSetVertexInputEXT(cb, nbd, bd, nad, ad);
    if (!m_mine(cb)) {
        return;
    }
    g_m.L.vi_ok = nbd <= HLE_DRAW_MAX_BIND && nad <= HLE_DRAW_ATTRS;
    if (g_m.L.vi_ok) {
        g_m.L.nbd = nbd;
        g_m.L.nad = nad;
        memcpy(g_m.L.bd, bd, sizeof(bd[0]) * nbd);
        memcpy(g_m.L.ad, ad, sizeof(ad[0]) * nad);
    }
}

bool hle_merge_wanted(void)
{
    static int w = -1;
    if (w < 0) {
        /* off by default: on the Mali-G52 each merged draw's switches to
         * its pipeline and vertex layout and back cost the driver more than
         * the jobs it saves (Sega GT 2002's race: 44 fps merged, 50 not) */
        const char *e = getenv("HAKUX_HLE_MERGE");
        w = e ? atoi(e) != 0 : 0;
#ifdef __ANDROID__
        char v[PROP_VALUE_MAX] = "";
        if (!e && hle_prop_get("debug.hakux.merge", v) > 0 && v[0]) {
            w = atoi(v) != 0;
        }
#endif
    }
    return w;
}

void hle_merge_init(const HleMergeEnv *env)
{
    E = *env;
    g_m.on = g_b.buf != VK_NULL_HANDLE && E.mlayout != VK_NULL_HANDLE;
    hle_log("hakuX-hle-back", "draw merging %s", g_m.on ? "on" : "off (no index buffer)");
}

#include "hw/xbox/hle/vk/batch_wrap.h"

void hle_batch_install(struct VolkDeviceTable *t)
{
    R = *t;
    wrap_table(t);
    /* draw merging's own: the state between draws held while a run is */
    t->vkCmdDraw = m_vkCmdDraw;
    t->vkCmdDrawIndexed = m_vkCmdDrawIndexed;
    t->vkCmdPushConstants = m_vkCmdPushConstants;
    t->vkCmdBindDescriptorSets = m_vkCmdBindDescriptorSets;
    t->vkCmdBindVertexBuffers = m_vkCmdBindVertexBuffers;
    t->vkCmdBindIndexBuffer = m_vkCmdBindIndexBuffer;
    t->vkCmdBindPipeline = m_vkCmdBindPipeline;
    if (t->vkCmdSetVertexInputEXT) {
        t->vkCmdSetVertexInputEXT = m_vkCmdSetVertexInputEXT;
    }
}

/* Writes a strip's indices (relative to the run's base) at the slot's head. */
static bool put(uint32_t first, uint32_t count, bool restart)
{
    uint32_t need = count + (restart ? 1 : 0);
    if (g_b.pos + need > SLOT_INDICES || first < g_b.base) {
        return false;
    }
    uint32_t *p = g_b.map + g_b.slot_base + g_b.pos;
    if (restart) {
        *p++ = RESTART;
    }
    for (uint32_t i = 0; i < count; i++) {
        p[i] = first - g_b.base + i;
    }
    g_b.pos += need;
    g_b.n += need;
    return true;
}

bool hle_batch_draw(VkCommandBuffer cb, VkPrimitiveTopology prim, uint32_t first, uint32_t count)
{
    if (!g_b.on || !count) {
        return false;
    }
    merge_before(cb);
    if (g_b.cb == cb && g_b.prim == prim) {
        if (g_b.draws == 1) {           /* a second strip: the first goes in as indices too */
            g_b.istart = g_b.slot_base + g_b.pos;
            g_b.n = 0;
            if (!put(g_b.first, g_b.count, false)) {
                flush();
                goto start;
            }
        }
        uint32_t pos = g_b.pos, n = g_b.n;
        if (put(first, count, true)) {
            g_b.draws++;
            return true;
        }
        g_b.pos = pos;
        g_b.n = n;
        flush();
    } else if (g_b.cb) {
        flush();
    }
start:
    g_b.cb = cb;
    g_b.prim = prim;
    g_b.first = first;
    g_b.count = count;
    g_b.base = first;
    g_b.draws = 1;
    g_b.n = 0;
    return true;
}

void hle_batch_slot(unsigned slot)
{
    g_b.slot_base = (slot % SLOTS) * SLOT_INDICES;
    g_b.pos = 0;
}

void hle_batch_flush(VkCommandBuffer cb)
{
    strip_before(cb);                   /* a merge run restores the index buffer itself */
}

void hle_batch_stats(uint64_t *batches, uint64_t *merged)
{
    *batches = g_b.batches;
    *merged = g_b.merged;
}

static bool batch_wanted(void)
{
    const char *e = getenv("HAKUX_HLE_BATCH");
    if (e) {
        return atoi(e) != 0;
    }
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    if (hle_prop_get("debug.hakux.batch", v) > 0 && v[0]) {
        return atoi(v) != 0;
    }
#endif
    return true;
}

void hle_batch_init(void)
{
    if (g_b.buf || !batch_wanted()) {
        hle_log("hakuX-hle-back", "strip batching %s", g_b.buf ? "on" : "off");
        return;
    }
    if (!g_hvk.caps.eds2) {             /* restart is the pipelines' then, off (vk/draw.c) */
        hle_log("hakuX-hle-back", "strip batching off: no dynamic primitive restart");
        return;
    }
    const VkDeviceSize size = (VkDeviceSize)SLOTS * SLOT_INDICES * 4;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (R.vkCreateBuffer(g_hvk.dev, &bci, NULL, &g_b.buf) != VK_SUCCESS) {
        g_b.buf = VK_NULL_HANDLE;
        return;
    }
    VkMemoryRequirements req;
    R.vkGetBufferMemoryRequirements(g_hvk.dev, g_b.buf, &req);
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int mt = hle_vk_memtype(req.memoryTypeBits, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mt < 0) {
        mt = hle_vk_memtype(req.memoryTypeBits, hv, 0);
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || R.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_b.mem) != VK_SUCCESS) {
        R.vkDestroyBuffer(g_hvk.dev, g_b.buf, NULL);
        g_b.buf = VK_NULL_HANDLE;
        hle_log("hakuX-hle-back", "strip batching off: no index memory");
        return;
    }
    R.vkBindBufferMemory(g_hvk.dev, g_b.buf, g_b.mem, 0);
    R.vkMapMemory(g_hvk.dev, g_b.mem, 0, size, 0, (void **)&g_b.map);
    g_b.on = g_b.map != NULL;
    hle_log("hakuX-hle-back", "strip batching %s (%u KiB of indices per frame slot)", g_b.on ? "on" : "off",
            SLOT_INDICES * 4 / 1024);
}

