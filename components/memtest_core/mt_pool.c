/*
 * esp32-memtest - test pool management.
 *
 * SPDX-License-Identifier: MIT
 *
 * A "pool" is one or more heap blocks that together make up the region
 * under test.  Several smaller blocks are accepted on purpose: a fragmented
 * heap still lets us test (almost) everything that is free, instead of
 * giving up because no single large block is available.
 */

#include "memtest_core.h"

#include <string.h>

#define MT_MIN_USEFUL_BLOCK (4u * 1024u)

static size_t round_down4(size_t v)
{
    return v & ~(size_t)3u;
}

bool mt_pool_acquire(mt_pool_t *pool, const mt_pool_request_t *req,
                     mt_alloc_fn alloc, void *alloc_user)
{
    if (pool == NULL || req == NULL || alloc == NULL) {
        return false;
    }
    memset(pool, 0, sizeof(*pool));

    size_t min_block = req->min_block ? req->min_block : MT_MIN_USEFUL_BLOCK;
    size_t want = req->block_bytes ? req->block_bytes : MT_MIN_USEFUL_BLOCK;
    size_t left = req->target_bytes ? req->target_bytes : (size_t)-1;
    bool got_any = false;

    while (pool->count < MT_MAX_BLOCKS && left > 0) {
        size_t ask = round_down4(want < left ? want : left);
        if (ask < min_block) {
            break;
        }

        void *p = NULL;
        /* Shrink the request until the allocator is happy.  This walks the
         * heap fragmentation instead of bailing out on the first failure. */
        for (unsigned attempt = 0; attempt < 12 && p == NULL; attempt++) {
            p = alloc(ask, req->caps, alloc_user);
            if (p == NULL) {
                ask /= 2u;
                ask = round_down4(ask);
                if (ask < min_block) {
                    break;
                }
            }
        }
        if (p == NULL) {
            break;
        }

        mt_block_t *blk = &pool->blocks[pool->count++];
        blk->addr = (uint8_t *)p;
        blk->size = ask;
        pool->total_bytes += ask;
        if (ask > pool->largest_block) {
            pool->largest_block = ask;
        }
        uintptr_t start = (uintptr_t)p;
        uintptr_t end = start + ask;
        if (pool->count == 1 || start < pool->lowest_addr) {
            pool->lowest_addr = start;
        }
        if (pool->count == 1 || end > pool->highest_addr) {
            pool->highest_addr = end;
        }
        got_any = true;

        if (left != (size_t)-1) {
            left -= ask;
        }
        /* Keep asking for the same size so that a fragmented heap is fully
         * consumed; only shrink when nothing big enough is left. */
        if (left != (size_t)-1 && left < min_block) {
            break;
        }
    }

    return got_any;
}

void mt_pool_release(mt_pool_t *pool, mt_free_fn release, void *free_user)
{
    if (pool == NULL || release == NULL) {
        return;
    }
    for (size_t i = 0; i < pool->count; i++) {
        if (pool->blocks[i].addr) {
            release(pool->blocks[i].addr, free_user);
        }
    }
    memset(pool, 0, sizeof(*pool));
}