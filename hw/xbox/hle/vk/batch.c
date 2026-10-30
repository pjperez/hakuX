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
#include "hw/xbox/hle/vk/guest_ram.h"
#include "hw/xbox/hle/vk/vkdev.h"

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
    bool idx;                       /* its draws are indexed (16-bit, the arena) */
    uint32_t first, count;          /* its first draw, as it came */
    uint32_t d_ib;                  /* its first draw's index offset (indexed runs) */
    uint32_t base;                  /* vertexOffset of the merged draw */
    uint32_t istart, n;             /* its indices (absolute index, count); n = 0: not written */
    uint32_t draws;
    uint64_t batches, merged;
    uint64_t runs1, runsN;          /* t618 debug: runs ended at one draw, at more */
} g_b;

/* Records the run held on cb: the plain draw for a run of one. */
static void flush(void)
{
    VkCommandBuffer cb = g_b.cb;
    g_b.cb = VK_NULL_HANDLE;
    if (g_b.draws == 1) {
        g_b.runs1++;
        if (g_b.idx) {
            R.vkCmdBindIndexBuffer(cb, g_harena.buf, 0, VK_INDEX_TYPE_UINT16);
            R.vkCmdDrawIndexed(cb, g_b.count, 1, g_b.d_ib / 2, (int32_t)g_b.first, 0);
            hle_draw_ibuf_lost();
        } else {
            R.vkCmdDraw(cb, g_b.count, 1, g_b.first, 0);
        }
        return;
    }
    g_b.runsN++;
    R.vkCmdBindIndexBuffer(cb, g_b.buf, (VkDeviceSize)g_b.istart * 4, VK_INDEX_TYPE_UINT32);
    if (R.vkCmdSetPrimitiveRestartEnable) {
        R.vkCmdSetPrimitiveRestartEnable(cb, VK_TRUE);
        R.vkCmdDrawIndexed(cb, g_b.n, 1, 0, (int32_t)g_b.base, 0);
        R.vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    } else {
        /* restart is the pipeline's (eds2 off, built on in vk/draw.c) */
        R.vkCmdDrawIndexed(cb, g_b.n, 1, 0, (int32_t)g_b.base, 0);
    }
    hle_draw_ibuf_lost();               /* the draw path's index buffer is no longer bound */
    g_b.batches++;
    g_b.merged += g_b.draws;
}

/* Anything recorded on cb comes after the held run. */
static inline void batch_before(VkCommandBuffer cb)
{
    if (g_b.cb == cb && cb) {
        flush();
    }
}

#include "hw/xbox/hle/vk/batch_wrap.h"

void hle_batch_install(struct VolkDeviceTable *t)
{
    R = *t;
    wrap_table(t);
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

/* The same for an indexed draw: its 16-bit indices copied from the arena,
 * each moved to the run's vertexOffset (voff - base) so gl_VertexIndex keeps
 * the value the draw gave it. */
static bool put16(uint32_t ib_off, uint32_t count, uint32_t voff, bool restart)
{
    uint32_t need = count + (restart ? 1 : 0);
    if (g_b.pos + need > SLOT_INDICES || (ib_off & 1)) {
        return false;
    }
    const int64_t d = (int64_t)voff - (int64_t)g_b.base;
    if (d < -(1ll << 30) || d > (1ll << 30)) {
        return false;
    }
    const uint16_t *src = (const uint16_t *)(void *)((const uint8_t *)g_harena.map + ib_off);
    uint32_t *p = g_b.map + g_b.slot_base + g_b.pos;
    if (restart) {
        *p++ = RESTART;
    }
    for (uint32_t i = 0; i < count; i++) {
        const int64_t v = d + src[i];
        if (v < 0 || v > 0x7fffffff) {
            return false;              /* not within the run's vertex range */
        }
        p[i] = (uint32_t)v;
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
    if (g_b.cb == cb && g_b.prim == prim && !g_b.idx) {
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
    g_b.idx = false;
    g_b.first = first;
    g_b.count = count;
    g_b.base = first;
    g_b.draws = 1;
    g_b.n = 0;
    return true;
}

bool hle_batch_draw_idx(VkCommandBuffer cb, VkPrimitiveTopology prim, uint32_t ib_off,
                        uint32_t count, uint32_t voff)
{
    if (!g_b.on || !count || (ib_off & 1)) {
        return false;
    }
    if (g_b.cb == cb && g_b.prim == prim && g_b.idx) {
        if (g_b.draws == 1) {           /* a second strip: the first goes in as indices too */
            g_b.istart = g_b.slot_base + g_b.pos;
            g_b.n = 0;
            if (!put16(g_b.d_ib, g_b.count, g_b.first, false)) {
                flush();
                goto start;
            }
        }
        uint32_t pos = g_b.pos, n = g_b.n;
        if (put16(ib_off, count, voff, true)) {
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
    g_b.idx = true;
    g_b.first = voff;
    g_b.count = count;
    g_b.d_ib = ib_off;
    g_b.base = voff;
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
    batch_before(cb);
}

void hle_batch_stats(uint64_t *batches, uint64_t *merged)
{
    *batches = g_b.batches;
    *merged = g_b.merged;
    static int64_t last;
    const int64_t now = hle_now_ns();
    if (now - last >= 5000000000ll) {          /* t618 debug: why runs break */
        last = now;
        hle_log("hakuX-hle-back", "strip batching runs: %llu of one, %llu joined (%llu draws)",
                (unsigned long long)g_b.runs1, (unsigned long long)g_b.runsN,
                (unsigned long long)g_b.merged);
        g_b.runs1 = g_b.runsN = 0;
    }
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
    /* With eds2 (vkCmdSetPrimitiveRestartEnable) the joined draw turns restart
     * on and off around itself (flush). Without it the pipelines are built
     * with primitiveRestartEnable on (vk/draw.c): ignored by the unindexed
     * draws the runs are made of, and the runs' 32-bit 0xFFFFFFFF separator
     * is not a value a guest 16-bit index carries. That is what lets Vulkan
     * 1.1 drivers without extended dynamic state 2 batch too. */
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

