/*
 * esp32-memtest - test runner.
 *
 * SPDX-License-Identifier: MIT
 *
 * The runner keeps to the classic MemTest86+ loop:
 *
 *     repeat
 *         for every sub-pass of the pattern
 *             write the whole region        (no reads, no compares)
 *             verify the whole region       (no writes)
 *
 * Writing and verifying are two separate sweeps on purpose: a value that is
 * written and immediately read back can still be sitting in the CPU data
 * cache, so sweeping several megabytes between the write and the read is what
 * actually exercises the cells (and, on the S3, the PSRAM cache and SPI link).
 */

#include "memtest_core.h"

#include <string.h>

/* Let other tasks run this often. The board layer's yield really blocks for one
 * tick so that the idle task can feed the task watchdog, so this interval is a
 * direct throughput/safety trade: too small and the test spends its life asleep,
 * too large and another task (or the watchdog service) is starved. */
#define MT_YIELD_BYTES   (2u * 1024u * 1024u)
/* Report progress and poll the keyboard this often. Key handling has to stay
 * responsive, and the UI redraw is the expensive part, so this is a compromise
 * between "skip feels instant" and "drawing does not dominate the test". */
#define MT_REPORT_TICK   (16u)
/* Patterns that run 32 or 64 sub-passes only sweep this much of the pool.
 *
 * bitone, bitzero and modulox repeat the whole pool once per sub-pass, so on an
 * 8 MiB PSRAM that is 128 extra full-pool passes - roughly a gigabyte of traffic
 * for patterns whose job is to walk bit positions, not to cover every byte.
 * A stuck bit is a property of a cell, so sweeping every bit position across a
 * sample finds it just as reliably while keeping the runtime sane. The
 * single-sub-pass patterns still cover the entire pool. */
#define MT_SUBSAMPLE_BYTES (512u * 1024u)

static void yield_if_needed(mt_yield_fn yield_fn, size_t *counter, size_t step)
{
    if (yield_fn == NULL) {
        return;
    }
    *counter += step;
    if (*counter >= MT_YIELD_BYTES) {
        *counter = 0;
        yield_fn();
    }
}

static void merge_details(mt_result_t *res, const mt_scan_t *scan)
{
    for (size_t i = 0; i < scan->detail_count && res->error_count < MT_MAX_DETAIL_ERRORS; i++) {
        res->errors[res->error_count++] = scan->details[i];
    }
}

static void report(mt_progress_cb cb, void *user, const mt_progress_t *p)
{
    if (cb) {
        cb(p, user);
    }
}

void mt_run_test(mt_pool_t *pool, const mt_test_spec_t *spec, uint32_t default_seed,
                 mt_clock_fn clock, mt_yield_fn yield_fn,
                 mt_progress_cb progress, void *progress_user,
                 volatile bool *aborted,
                 mt_result_t *result, mt_stop_reason_t *stop_reason)
{
    mt_stop_reason_t reason = MT_STOP_NONE;
    memset(result, 0, sizeof(*result));
    if (stop_reason) {
        *stop_reason = MT_STOP_NONE;
    }

    const mt_pattern_desc_t *desc = mt_pattern_desc(spec->pattern);
    const uint32_t subs = desc ? (desc->subpasses ? desc->subpasses : 1u) : 1u;
    /* Multi-sub-pass patterns sweep only a sample of the pool; see
     * MT_SUBSAMPLE_BYTES. Single-sub-pass patterns always use everything. */
    size_t block_limit = pool->count;
    if (subs > 1u) {
        size_t budget = MT_SUBSAMPLE_BYTES;
        size_t used = 0;
        size_t n = 0;
        while (n < pool->count && used < budget) {
            used += pool->blocks[n].size;
            n++;
        }
        block_limit = n ? n : 1u;
    }
    const uint32_t seed = spec->seed ? spec->seed : default_seed;
    const uint32_t passes = spec->passes;
    const uint32_t passes_target = passes ? passes : 0xFFFFFFFFu;
    uint64_t sampled_bytes = 0;
    for (size_t b = 0; b < block_limit; b++) {
        sampled_bytes += pool->blocks[b].size;
    }
    const uint64_t bytes_total = sampled_bytes * subs * (passes ? passes : 1u) * 2u;
    const uint64_t t0 = clock ? clock() : 0;

    result->name = desc ? desc->name : "unknown";
    result->bytes = pool->total_bytes;
    result->passes_target = passes_target;
    result->complete = true;

    mt_progress_t p = {
        .state = "run",
        .pass = 0,
        .passes_total = passes_target,
        .sub = 0,
        .subs_total = subs,
        .bytes_done = 0,
        .bytes_total = bytes_total,
        .percent = 0,
        .finished = false,
    };

    mt_scan_t scan;
    mt_scan_reset(&scan);
    size_t yield_counter = 0;
    uint64_t cells_checked = 0;

    for (uint32_t pass = 1; pass <= passes_target; pass++) {
        p.pass = pass;

        for (uint32_t sub = 0; sub < subs; sub++) {
            p.sub = sub + 1;
            p.state = (sub > 1 || subs > 1) ? "write" : "run";

            /* --- write sweep ------------------------------------------- */
            for (size_t b = 0; b < block_limit; b++) {
                mt_pattern_ctx_t ctx = {
                    .pattern = spec->pattern,
                    .sub = sub,
                    .seed = seed,
                    .block_id = (uint32_t)b,
                };
                (void)mt_pattern_apply(&ctx, pool->blocks[b].addr,
                                      pool->blocks[b].size, true, NULL);
                p.bytes_done += pool->blocks[b].size;
                p.percent = p.bytes_total ? (uint32_t)((p.bytes_done * 100u) / p.bytes_total) : 0u;
                yield_if_needed(yield_fn, &yield_counter, pool->blocks[b].size);
                if (aborted && *aborted) {
                    reason = MT_STOP_USER;
                    goto done;
                }
            }

            /* --- verify sweep ------------------------------------------ */
            mt_scan_reset(&scan);
            p.state = "verify";
            for (size_t b = 0; b < block_limit; b++) {
                mt_pattern_ctx_t ctx = {
                    .pattern = spec->pattern,
                    .sub = sub,
                    .seed = seed,
                    .block_id = (uint32_t)b,
                };
                (void)mt_pattern_apply(&ctx, pool->blocks[b].addr,
                                      pool->blocks[b].size, false, &scan);
                p.bytes_done += pool->blocks[b].size;
                p.percent = p.bytes_total ? (uint32_t)((p.bytes_done * 100u) / p.bytes_total) : 0u;
                yield_if_needed(yield_fn, &yield_counter, pool->blocks[b].size);
                report(progress, progress_user, &p);
                if (aborted && *aborted) {
                    reason = MT_STOP_USER;
                    goto done;
                }
            }

            cells_checked += scan.cells_checked;
            result->cells_failed += scan.cells_failed;
            result->bits_wrong += scan.bits_wrong;
            result->cells_flaky += scan.cells_flaky;
            if (scan.cells_failed) {
                merge_details(result, &scan);
            }
        }

        result->passes = pass;
        if (aborted && *aborted) {
            reason = MT_STOP_USER;
            goto done;
        }
        if (passes == 0) {
            /* Infinite mode: keep the UI alive and show elapsed time. */
            p.state = "repeat";
            report(progress, progress_user, &p);
        }
    }

done:
    result->cells = cells_checked;
    result->seconds = clock ? (double)(clock() - t0) / 1000000.0 : 0.0;
    /* Reported throughput covers both directions: every verified cell was
     * also written once. */
    result->mbps = (result->seconds > 0.0 && result->cells > 0)
                       ? ((double)result->cells * 8.0 / (1024.0 * 1024.0)) / result->seconds
                       : 0.0;
    result->complete = (reason == MT_STOP_NONE || reason == MT_STOP_FINISHED);
    if (!result->complete) {
        result->passes_aborted = 1;
    }
    if (reason == MT_STOP_NONE) {
        reason = MT_STOP_FINISHED;
    }
    if (stop_reason) {
        *stop_reason = reason;
    }

    p.state = "done";
    p.percent = result->complete ? 100u : p.percent;
    p.finished = true;
    report(progress, progress_user, &p);
}