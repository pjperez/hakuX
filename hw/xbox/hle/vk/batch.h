/*
 * hakuX HLE GPU — strip batching (see batch.c).
 */
#ifndef HW_XBOX_HLE_VK_BATCH_H
#define HW_XBOX_HLE_VK_BATCH_H

#include "hw/xbox/hle/vk/vkdev.h"

#include <stdbool.h>
#include <stdint.h>

/* Routes the table's recording commands through the batch (vkdev.c, once
 * the device's entry points are loaded). */
void hle_batch_install(struct VolkDeviceTable *t);

/* The index buffer, once the device is up; off (HAKUX_HLE_BATCH=0 or
 * debug.hakux.batch=0) records every draw as it comes. */
void hle_batch_init(void);

/* A command buffer starts for frame slot `slot`, whose previous recording
 * has completed: its share of the index buffer is free again. */
void hle_batch_slot(unsigned slot);

/* A non-indexed strip or fan draw: true when the batch holds it (recorded
 * later, with the strips that follow it), false to record it now. */
bool hle_batch_draw(VkCommandBuffer cb, VkPrimitiveTopology prim, uint32_t first, uint32_t count);

/* The run held on cb, recorded now: a run binds its own index buffer, so
 * the draw path records it before relying on the one it bound last. */
void hle_batch_flush(VkCommandBuffer cb);

/* Draws recorded and the draws they replaced, since start. */
void hle_batch_stats(uint64_t *batches, uint64_t *merged);

/*
 * Draw merging (batch.c): consecutive draws that change nothing between them
 * but their push constants, set 1's dynamic offset (their constants), their
 * vertex buffers' offsets (by whole vertices) and their index data become one
 * indexed draw of the pipeline's merged variant (vk/draw.c merged_src): each
 * draw's push block and set 1 base in set 3 (the push ring), a byte per
 * vertex naming its draw.
 */
#define HLE_MERGE_WIN  65536u           /* set 1's window: the merged draws' constants */
#define HLE_MERGE_LOC  15               /* the draw number's vertex attribute */
#define HLE_MERGE_VARY 10               /* its flat varying */
#define HLE_MERGE_MAX  64               /* draws a merged draw holds */
#define HLE_MERGE_REC  272              /* a draw's record in set 3: push block, set 1 base */
#define HLE_MERGE_SPAN 65536            /* vertices a merged draw spans */
#define HLE_XSTR_(x) #x
#define HLE_STR(x) HLE_XSTR_(x)

typedef struct HleMergeEnv {
    VkPipelineLayout layout, mlayout;   /* the draws', the merged pipelines' */
    VkDescriptorSet mset, pset;         /* set 1's window, set 3 (the push ring) */
    uint32_t win;                       /* the window's bytes */
    uint32_t arena_size;                /* set 1's buffer */
    uint32_t blk;                       /* a set 1 block's bytes at most */
    VkFormat id_fmt;                    /* the draw number's vertex format */
    uint32_t id_bytes;                  /* its size */
    /* the records into the push ring: their offset; false: no room */
    bool (*push_put)(const void *recs, uint32_t bytes, uint32_t *off);
    /* the driver's index range hint (malisx2), or NULL */
    void (*index_range)(VkCommandBuffer cb, uint32_t first, uint32_t count, uint32_t min,
                        uint32_t max);
} HleMergeEnv;

/* HAKUX_HLE_MERGE=0 / debug.hakux.merge=0: off (read once). */
bool hle_merge_wanted(void);
/* With the batch's index buffer up (hle_batch_init), from draw.c's init. */
void hle_merge_init(const HleMergeEnv *env);

/* The draw about to be recorded (its vkCmdDraw / vkCmdDrawIndexed next) */
typedef struct HleMergeNext {
    VkPipeline merged;                  /* its pipeline's merged variant, built; NULL: none */
    const uint16_t *indices;            /* indexed: its 16-bit indices from firstIndex */
    uint32_t vcount;                    /* indexed: every index below it; 0 unknown */
    bool strip;                         /* a strip or fan: draws apart by a restart */
} HleMergeNext;
void hle_merge_next(const HleMergeNext *m);
/* The frame's command buffer, just begun: the one draws are merged on. */
void hle_merge_cb(VkCommandBuffer cb);

/* Merged draws recorded and the draws in them, since start. */
void hle_merge_stats(uint64_t *merged, uint64_t *draws);

#endif
