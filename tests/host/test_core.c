/*
 * esp32-memtest - host unit tests for the portable test engine.
 *
 * SPDX-License-Identifier: MIT
 *
 * These tests run on a normal PC (gcc/clang, no ESP-IDF) and cover the parts
 * that are easy to get wrong on hardware: pattern determinism, error
 * detection, pool bookkeeping and the test set parser.
 *
 *   make -C tests/host run
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memtest_core.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define SECTION(name) printf("[ %s ]\n", name)

/* --------------------------------------------------------------------- */

static uint64_t fake_clock_us;

static uint64_t test_clock(void)
{
    fake_clock_us += 1000;
    return fake_clock_us;
}

static void test_yield(void)
{
}

/* --------------------------------------------------------------------- */

static void test_pattern_descriptors(void)
{
    SECTION("pattern descriptors");
    for (unsigned i = 0; i < MT_PATTERN_COUNT; i++) {
        const mt_pattern_desc_t *d = mt_pattern_desc((mt_pattern_t)i);
        CHECK(d != NULL, "descriptor %u missing", i);
        CHECK(d && d->key && d->name && d->desc, "descriptor %u has empty text", i);
        CHECK(d && d->subpasses > 0, "descriptor %u has no sub-passes", i);
        CHECK(d && d->subpasses <= MT_MAX_SUBPASSES, "descriptor %u too many sub-passes", i);
    }
    CHECK(mt_pattern_desc(MT_PATTERN_COUNT) == NULL, "out of range lookup must fail");
    CHECK(mt_pattern_desc((mt_pattern_t)-1) == NULL, "negative lookup must fail");

    for (unsigned i = 0; i < MT_PATTERN_COUNT; i++) {
        int back = mt_pattern_from_name(mt_pattern_table[i].key);
        CHECK(back == (int)i, "round trip failed for %s", mt_pattern_table[i].key);
    }
    CHECK(mt_pattern_from_name("ZERO") == MT_PAT_ZERO, "name lookup must be case insensitive");
    CHECK(mt_pattern_from_name("nosuchpattern") == -1, "unknown pattern must be rejected");
    CHECK(mt_pattern_from_name("") == -1, "empty pattern must be rejected");
    CHECK(mt_pattern_from_name(NULL) == -1, "NULL pattern must be rejected");
}

static void test_rng(void)
{
    SECTION("pseudo random generator");
    mt_rng_t a, b;
    mt_rng_seed(&a, 1234);
    mt_rng_seed(&b, 1234);
    for (int i = 0; i < 1000; i++) {
        CHECK(mt_rng_next(&a) == mt_rng_next(&b), "same seed must give the same stream (%d)", i);
    }

    mt_rng_seed(&a, 0);
    mt_rng_seed(&b, 1);
    int different = 0;
    for (int i = 0; i < 64; i++) {
        different += (mt_rng_next(&a) != mt_rng_next(&b));
    }
    CHECK(different > 60, "different seeds must give different streams (%d/64)", different);

    /* Rough bit balance check: catches a broken mixer. */
    uint32_t ones = 0;
    mt_rng_seed(&a, 0xC0FFEE);
    for (int i = 0; i < 4096; i++) {
        uint32_t v = mt_rng_next(&a);
        for (int b = 0; b < 32; b++) {
            ones += (v >> b) & 1u;
        }
    }
    CHECK(ones > 4096u * 32u * 45u / 100u && ones < 4096u * 32u * 55u / 100u,
          "bit distribution looks broken (%u ones of %u)", ones, 4096u * 32u);

    /* A stuck or constant stream would make the random pattern worthless. */
    mt_rng_seed(&a, 42);
    uint32_t v0 = mt_rng_next(&a);
    int unique = 0;
    for (int i = 0; i < 256; i++) {
        unique += (mt_rng_next(&a) != v0);
    }
    CHECK(unique > 250, "stream repeats too often");
}

static void test_patterns_are_clean(void)
{
    SECTION("patterns survive a clean round trip");
    for (unsigned i = 0; i < MT_PATTERN_COUNT; i++) {
        const mt_pattern_desc_t *d = mt_pattern_desc((mt_pattern_t)i);
        for (uint32_t sub = 0; sub < d->subpasses; sub++) {
            mt_scan_t scan;
            size_t bytes = 4096 + 4 * (sub * 7 % 1000);
            uint8_t *buf = malloc(bytes);
            CHECK(buf != NULL, "out of memory");
            if (!buf) {
                continue;
            }
            memset(buf, 0xA5, bytes);   /* dirty: a partial fill must not hide */
            mt_pattern_ctx_t ctx = { .pattern = (mt_pattern_t)i, .sub = sub,
                                     .seed = 0x1234u + sub, .block_id = 3 };
            (void)mt_pattern_apply(&ctx, buf, bytes, true, NULL);
            mt_scan_reset(&scan);
            uint32_t bad = mt_pattern_apply(&ctx, buf, bytes, false, &scan);
            CHECK(bad == 0, "%s sub-pass %u: %u errors", d->name, sub, bad);
            free(buf);
        }
    }
}

static void test_random_is_deterministic(void)
{
    SECTION("random pattern is reproducible");
    size_t bytes = 64 * 1024;
    uint8_t *a = malloc(bytes);
    uint8_t *b = malloc(bytes);
    CHECK(a && b, "out of memory");
    if (!a || !b) {
        return;
    }
    mt_pattern_ctx_t ctx = { .pattern = MT_PAT_RANDOM, .sub = 0, .seed = 7, .block_id = 1 };

    (void)mt_pattern_apply(&ctx, a, bytes, true, NULL);
    memset(b, 0x5A, bytes);
    (void)mt_pattern_apply(&ctx, a, bytes, true, NULL);
    (void)mt_pattern_apply(&ctx, b, bytes, true, NULL);
    CHECK(memcmp(a, b, bytes) == 0, "same seed must produce the same buffer");

    ctx.seed = 8;
    (void)mt_pattern_apply(&ctx, b, bytes, true, NULL);
    CHECK(memcmp(a, b, bytes) != 0, "different seed must produce a different buffer");

    ctx.seed = 7;
    ctx.block_id = 2;
    (void)mt_pattern_apply(&ctx, b, bytes, true, NULL);
    CHECK(memcmp(a, b, bytes) != 0, "different block must produce a different buffer");

    free(a);
    free(b);
}

static void test_patterns_differ(void)
{
    SECTION("different patterns produce different data");
    const size_t bytes = 16 * 1024;
    uint8_t *bufs[MT_PATTERN_COUNT];

    for (unsigned i = 0; i < MT_PATTERN_COUNT; i++) {
        bufs[i] = malloc(bytes);
    }
    for (unsigned i = 0; i < MT_PATTERN_COUNT; i++) {
        CHECK(bufs[i] != NULL, "out of memory");
        if (!bufs[i]) {
            return;
        }
        memset(bufs[i], 0xA5, bytes);
        mt_pattern_ctx_t ctx = { .pattern = (mt_pattern_t)i, .sub = 0, .seed = 1, .block_id = 0 };
        (void)mt_pattern_apply(&ctx, bufs[i], bytes, true, NULL);
    }
    for (unsigned i = 0; i < MT_PATTERN_COUNT; i++) {
        for (unsigned j = i + 1; j < MT_PATTERN_COUNT; j++) {
            CHECK(memcmp(bufs[i], bufs[j], bytes) != 0, "%s and %s produce identical data",
                  mt_pattern_table[i].name, mt_pattern_table[j].name);
        }
        free(bufs[i]);
    }
}

static void test_address_pattern_covers_all_bytes(void)
{
    SECTION("address pattern varies in every byte lane");
    size_t bytes = 4096;
    uint8_t *buf = malloc(bytes);
    CHECK(buf != NULL, "out of memory");
    if (!buf) {
        return;
    }
    memset(buf, 0xA5, bytes);
    mt_pattern_ctx_t ctx = { .pattern = MT_PAT_ADDRESS, .sub = 0, .seed = 5, .block_id = 0 };
    (void)mt_pattern_apply(&ctx, buf, bytes, true, NULL);

    uint32_t seen[4] = { 0, 0, 0, 0 };
    uint32_t *words = (uint32_t *)buf;
    for (size_t i = 0; i < bytes / 4; i++) {
        for (int b = 0; b < 4; b++) {
            seen[b] |= 1u << ((words[i] >> (8 * b)) & 0xFF);
        }
    }
    for (int b = 0; b < 4; b++) {
        CHECK(seen[b] == 0xFFFFFFFFu, "byte lane %d only used %08x of the value space", b,
              seen[b]);
    }
    free(buf);
}

static void test_error_detection(void)
{
    SECTION("error detection");
    const size_t bytes = 8192;
    uint8_t *buf = malloc(bytes);
    CHECK(buf != NULL, "out of memory");
    if (!buf) {
        return;
    }

    for (unsigned i = 0; i < MT_PATTERN_COUNT; i++) {
        mt_pattern_desc_t *d = &mt_pattern_table[i];
        mt_pattern_ctx_t ctx = { .pattern = (mt_pattern_t)i, .sub = 0, .seed = 9, .block_id = 2 };
        (void)mt_pattern_apply(&ctx, buf, bytes, true, NULL);

        /* flip a single bit in cell 100 (offset 400) */
        buf[400] ^= 0x08u;

        mt_scan_t scan;
        mt_scan_reset(&scan);
        uint32_t bad = mt_pattern_apply(&ctx, buf, bytes, false, &scan);
        CHECK(bad == 1, "%s: expected 1 bad cell, got %u", d->name, bad);
        CHECK(scan.cells_failed == 1, "%s: cells_failed %llu", d->name,
              (unsigned long long)scan.cells_failed);
        CHECK(scan.bits_wrong == 1, "%s: bits_wrong %llu", d->name,
              (unsigned long long)scan.bits_wrong);
        CHECK(scan.has_first, "%s: no first error recorded", d->name);
        CHECK(scan.first.offset == 400, "%s: first offset %u, expected 400",
              d->name, scan.first.offset);
        CHECK(scan.first.block_id == 2, "%s: block id lost", d->name);
        CHECK(scan.first.addr == (uintptr_t)(buf + 400), "%s: wrong address", d->name);
        CHECK(scan.first.persistent, "%s: single bit error must be persistent", d->name);
        CHECK(scan.first.xor_diff == 0x08u, "%s: xor difference %08x", d->name,
              scan.first.xor_diff);
        CHECK(scan.cells_flaky == 0, "%s: sticky error counted as flaky", d->name);

        /* restore and verify that a clean buffer reports nothing again */
        buf[400] ^= 0x08u;
        mt_scan_reset(&scan);
        CHECK(mt_pattern_apply(&ctx, buf, bytes, false, &scan) == 0,
              "%s: clean buffer still reports errors", d->name);
    }
    free(buf);
}

static void test_error_details_capped(void)
{
    SECTION("error reporting is capped but counted");
    const size_t bytes = 64 * 1024;
    uint8_t *buf = malloc(bytes);
    CHECK(buf != NULL, "out of memory");
    if (!buf) {
        return;
    }
    mt_pattern_ctx_t ctx = { .pattern = MT_PAT_ONES, .sub = 0, .seed = 0, .block_id = 0 };
    (void)mt_pattern_apply(&ctx, buf, bytes, true, NULL);
    for (size_t i = 0; i < bytes; i += 4) {
        buf[i] ^= 0x01u;    /* corrupt 16384 cells */
    }

    mt_scan_t scan;
    mt_scan_reset(&scan);
    uint32_t bad = mt_pattern_apply(&ctx, buf, bytes, false, &scan);
    CHECK(bad == bytes / 4, "expected %zu bad cells, counted %u", bytes / 4, bad);
    CHECK(scan.cells_failed == bytes / 4, "cells_failed %llu",
          (unsigned long long)scan.cells_failed);
    CHECK(scan.detail_count == MT_MAX_DETAIL_ERRORS, "details not capped (%zu)",
          scan.detail_count);
    CHECK(scan.first.offset == 0, "first error offset %u", scan.first.offset);
    free(buf);
}

static void test_flaky_detection(void)
{
    SECTION("transient mismatches are reported separately");
    /* The engine re-reads a mismatching cell.  A device that loses a value
     * between two reads therefore reports it as flaky, which the summary
     * shows separately.  On a host this is simulated by re-writing the
     * correct value from inside the verify loop, so instead we verify that a
     * persistent error is *not* classified as flaky - see the sticky error
     * test above - and that a healthy buffer yields zero flaky cells. */
    const size_t bytes = 4096;
    uint8_t *buf = malloc(bytes);
    CHECK(buf != NULL, "out of memory");
    if (!buf) {
        return;
    }
    mt_pattern_ctx_t ctx = { .pattern = MT_PAT_RANDOM, .sub = 0, .seed = 3, .block_id = 0 };
    (void)mt_pattern_apply(&ctx, buf, bytes, true, NULL);
    mt_scan_t scan;
    mt_scan_reset(&scan);
    (void)mt_pattern_apply(&ctx, buf, bytes, false, &scan);
    CHECK(scan.cells_flaky == 0, "clean memory produced flaky errors");
    free(buf);
}

/* --------------------------------------------------------------------- */

typedef struct {
    size_t   granted;
    size_t   granularity;   /* allocations are truncated to this */
    size_t   refuse_above;  /* refuse anything bigger, 0 = never */
    int      calls;
} fake_heap_t;

static void *fake_alloc(size_t bytes, uint32_t caps, void *user)
{
    fake_heap_t *h = user;
    (void)caps;
    h->calls++;
    if (h->refuse_above && bytes > h->refuse_above) {
        return NULL;
    }
    if (bytes > h->granted) {
        return NULL;
    }
    size_t got = bytes;
    if (h->granularity) {
        got -= got % h->granularity;
    }
    if (got == 0) {
        return NULL;
    }
    h->granted -= got;
    void *p = malloc(got);
    if (p) {
        memset(p, 0x11, got);
    }
    return p;
}

static void fake_free(void *p, void *user)
{
    (void)user;
    free(p);
}

static void test_pool_basic(void)
{
    SECTION("pool acquisition");
    fake_heap_t heap = { .granted = 1024 * 1024, .granularity = 4096 };
    mt_pool_t pool;
    mt_pool_request_t req = {
        .block_bytes = 64 * 1024,
        .target_bytes = 256 * 1024,
        .min_block = 4096,
        .caps = 0,
    };
    CHECK(mt_pool_acquire(&pool, &req, fake_alloc, &heap),
          "pool acquisition failed");
    CHECK(pool.total_bytes == 256 * 1024, "expected 256 KiB, got %zu", pool.total_bytes);
    CHECK(pool.count == 4, "expected 4 blocks, got %zu", pool.count);
    CHECK(pool.largest_block == 64 * 1024, "largest block %zu", pool.largest_block);
    CHECK(pool.highest_addr > pool.lowest_addr, "address range not tracked");

    size_t total = 0;
    for (size_t i = 0; i < pool.count; i++) {
        CHECK(pool.blocks[i].size == 64 * 1024, "block %zu has size %zu", i,
              pool.blocks[i].size);
        CHECK((uintptr_t)pool.blocks[i].addr % 4 == 0, "block %zu is not 4 byte aligned", i);
        total += pool.blocks[i].size;
    }
    CHECK(total == pool.total_bytes, "block sizes do not add up");
    mt_pool_release(&pool, fake_free, NULL);
    CHECK(pool.count == 0 && pool.total_bytes == 0, "release did not reset the pool");
}

static void test_pool_fragmented(void)
{
    SECTION("pool acquisition on a fragmented heap");
    fake_heap_t heap = { .granted = 40 * 1024, .granularity = 4096, .refuse_above = 8 * 1024 };
    mt_pool_t pool;
    mt_pool_request_t req = {
        .block_bytes = 32 * 1024,
        .target_bytes = 0,          /* as much as possible */
        .min_block = 4 * 1024,
        .caps = 0,
    };
    CHECK(mt_pool_acquire(&pool, &req, fake_alloc, &heap),
          "a fragmented heap must still yield a pool");
    CHECK(pool.total_bytes == 40 * 1024, "expected 40 KiB, got %zu", pool.total_bytes);
    for (size_t i = 0; i < pool.count; i++) {
        CHECK(pool.blocks[i].size <= 8 * 1024, "block %zu too big for the fake heap", i);
    }
    mt_pool_release(&pool, fake_free, NULL);
}

static void test_pool_failure(void)
{
    SECTION("pool acquisition reports failure honestly");
    fake_heap_t heap = { .granted = 1024, .granularity = 0 };
    mt_pool_t pool;
    mt_pool_request_t req = {
        .block_bytes = 64 * 1024,
        .target_bytes = 64 * 1024,
        .min_block = 8 * 1024,
        .caps = 0,
    };
    CHECK(!mt_pool_acquire(&pool, &req, fake_alloc, &heap),
          "acquisition must fail when nothing can be allocated");
    CHECK(pool.count == 0, "failed acquisition must leave an empty pool");
}

/* --------------------------------------------------------------------- */

static void test_runner_counts_errors(void)
{
    SECTION("test runner statistics");
    fake_heap_t heap = { .granted = 128 * 1024, .granularity = 4096 };
    mt_pool_t pool;
    mt_pool_request_t req = {
        .block_bytes = 64 * 1024,
        .target_bytes = 128 * 1024,
        .min_block = 4096,
        .caps = 0,
    };
    CHECK(mt_pool_acquire(&pool, &req, fake_alloc, &heap), "acquire failed");

    mt_test_spec_t spec = { .pattern = MT_PAT_ADDRESS, .seed = 1, .passes = 2 };
    mt_result_t res;
    mt_stop_reason_t stop = MT_STOP_NONE;
    fake_clock_us = 0;
    mt_run_test(&pool, &spec, 1, test_clock, test_yield, NULL, NULL, NULL, &res, &stop);

    CHECK(res.cells_failed == 0, "clean memory reported %llu bad cells",
          (unsigned long long)res.cells_failed);
    CHECK(res.cells == 128 * 1024 * 2 / 4, "verified %llu cells, expected %d",
          (unsigned long long)res.cells, 128 * 1024 * 2 / 4);
    CHECK(res.passes == 2, "passes = %u", res.passes);
    CHECK(res.complete, "run must be marked complete");
    CHECK(stop == MT_STOP_FINISHED, "stop reason %d", stop);
    CHECK(res.seconds > 0.0, "no elapsed time measured");
    CHECK(res.mbps > 0.0, "no throughput measured");

    /* Now rerun with a pattern that has many sub-passes to check the timing
     * and pass accounting.  Fault injection is impossible from the outside
     * because the runner's write sweep overwrites whatever we plant in the
     * buffer first, which is exactly why test_error_detection() injects at
     * the pattern level instead. */
    mt_test_spec_t walking = { .pattern = MT_PAT_BIT_ZERO, .seed = 0, .passes = 1 };
    fake_clock_us = 0;
    mt_run_test(&pool, &walking, 0, test_clock, test_yield, NULL, NULL, NULL, &res, &stop);
    CHECK(res.cells_failed == 0, "clean memory reported %llu bad cells",
          (unsigned long long)res.cells_failed);
    CHECK(res.cells == (uint64_t)pool.total_bytes * mt_pattern_desc(MT_PAT_BIT_ZERO)->subpasses / 4,
          "sub-pass accounting wrong: %llu cells", (unsigned long long)res.cells);

    mt_pool_release(&pool, fake_free, NULL);
}

static volatile bool g_abort;

static void aborting_progress(const mt_progress_t *p, void *user)
{
    (void)p;
    (void)user;
    g_abort = true;
}

static void test_runner_abort(void)
{
    SECTION("test runner honours the abort flag");
    fake_heap_t heap = { .granted = 512 * 1024, .granularity = 4096 };
    mt_pool_t pool;
    mt_pool_request_t req = {
        .block_bytes = 256 * 1024,
        .target_bytes = 512 * 1024,
        .min_block = 4096,
        .caps = 0,
    };
    CHECK(mt_pool_acquire(&pool, &req, fake_alloc, &heap), "acquire failed");

    mt_test_spec_t spec = { .pattern = MT_PAT_MODULO_X, .seed = 0, .passes = 0 }; /* infinite */
    mt_result_t res;
    mt_stop_reason_t stop = MT_STOP_NONE;
    g_abort = false;
    fake_clock_us = 0;
    mt_run_test(&pool, &spec, 0, test_clock, test_yield, aborting_progress, NULL,
                &g_abort, &res, &stop);
    CHECK(stop == MT_STOP_USER, "stop reason should be USER, got %d", stop);
    CHECK(!res.complete, "aborted run must not be marked complete");
    CHECK(res.passes_aborted == 1, "abort not recorded");
    mt_pool_release(&pool, fake_free, NULL);
}

/* --------------------------------------------------------------------- */

static void test_testset_parser(void)
{
    SECTION("test set parser");
    mt_testset_spec_t set;

    int n = mt_testset_parse("zero,ones", 1, &set);
    CHECK(n == 2, "expected 2 tests, got %d", n);
    CHECK(set.tests[0].pattern == MT_PAT_ZERO, "wrong first pattern");
    CHECK(set.tests[1].pattern == MT_PAT_ONES, "wrong second pattern");

    n = mt_testset_parse(" quick , ADDRESS ,random ", 3, &set);
    CHECK(n == 5, "expected 5 tests, got %d", n);
    CHECK(set.tests[3].pattern == MT_PAT_ADDRESS, "address test missing");
    CHECK(set.tests[0].seed == 3, "seed not propagated");
    CHECK(set.tests[4].pattern == MT_PAT_RANDOM, "random test missing");

    n = mt_testset_parse("random,random,random", 1, &set);
    CHECK(n == 1, "duplicates must be removed (got %d)", n);

    n = mt_testset_parse("quick,none", 1, &set);
    CHECK(n == 0, "'none' must clear the list (got %d)", n);

    n = mt_testset_parse("quick,none,bitone", 1, &set);
    CHECK(n == 1 && set.tests[0].pattern == MT_PAT_BIT_ONE,
          "'none' must clear only what came before it");

    CHECK(mt_testset_parse("bogus", 1, &set) == -1, "unknown token must be an error");
    CHECK(mt_testset_parse("zero,", 1, &set) == 1, "trailing comma must be tolerated");
    CHECK(mt_testset_parse(NULL, 1, &set) > 0, "NULL spec must fall back to a default");
    CHECK(mt_testset_parse("", 1, &set) > 0, "empty spec must fall back to a default");

    n = mt_testset_parse("extended", 1, &set);
    CHECK(n == (int)MT_PATTERN_COUNT, "'extended' should select every pattern (%d)", n);
    for (size_t i = 0; i < set.count; i++) {
        CHECK(set.tests[i].pattern < MT_PATTERN_COUNT, "invalid pattern id in set");
        CHECK(mt_pattern_desc(set.tests[i].pattern) != NULL, "missing descriptor");
    }
}

static void test_formatting(void)
{
    SECTION("formatting helpers");
    char buf[32];
    CHECK(strcmp(mt_fmt_bytes(buf, sizeof(buf), 0), "0 B") == 0, "got '%s'", buf);
    CHECK(strcmp(mt_fmt_bytes(buf, sizeof(buf), 512), "512 B") == 0, "got '%s'", buf);
    CHECK(strcmp(mt_fmt_bytes(buf, sizeof(buf), 1536), "1.50 KiB") == 0, "got '%s'", buf);
    CHECK(strcmp(mt_fmt_bytes(buf, sizeof(buf), 8u << 20), "8.00 MiB") == 0, "got '%s'", buf);
    CHECK(strcmp(mt_fmt_bytes(buf, sizeof(buf), 2ull << 30), "2.00 GiB") == 0, "got '%s'", buf);

    CHECK(strcmp(mt_fmt_hex(buf, sizeof(buf), 0xDEADBEEFu), "deadbeef") == 0, "got '%s'", buf);
    CHECK(strstr(mt_fmt_location(buf, sizeof(buf), (void *)0x3fc8b200u, 0x1a4c), "0x1a4c") != NULL,
          "location string missing the offset: %s", buf);
}

/* --------------------------------------------------------------------- */

int main(void)
{
    printf("esp32-memtest core unit tests\n");
    printf("===========================\n");

    test_pattern_descriptors();
    test_rng();
    test_patterns_are_clean();
    test_random_is_deterministic();
    test_patterns_differ();
    test_address_pattern_covers_all_bytes();
    test_error_detection();
    test_error_details_capped();
    test_flaky_detection();
    test_pool_basic();
    test_pool_fragmented();
    test_pool_failure();
    test_runner_counts_errors();
    test_runner_abort();
    test_testset_parser();
    test_formatting();

    printf("===========================\n");
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}