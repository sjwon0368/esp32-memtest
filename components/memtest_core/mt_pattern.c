/*
 * esp32-memtest - pattern engine.
 *
 * SPDX-License-Identifier: MIT
 *
 * Every pattern is a pure function of (pattern, sub-pass, seed, block id)
 * plus the cell index, so a fill pass and a later verify pass produce bit
 * identical results without having to remember the expected data anywhere.
 * That is what makes it possible to test tens of megabytes of RAM with a
 * few kilobytes of bookkeeping state.
 */

#include "memtest_core.h"

#include <string.h>

/* ------------------------------------------------------------------------- */

const mt_pattern_desc_t mt_pattern_table[MT_PATTERN_COUNT] = {
    [MT_PAT_ZERO]       = { "zero",       "Zero fill",        "0x00 everywhere",
                            1, 1 },
    [MT_PAT_ONES]       = { "ones",       "Ones fill",        "0xFF everywhere",
                            1, 1 },
    [MT_PAT_AA55]       = { "aa55",       "Byte alternation", "0xAAAA5555 walking byte lanes",
                            1, 1 },
    [MT_PAT_ADDRESS]    = { "address",    "Address test",     "value derived from the cell address",
                            1, 1 },
    [MT_PAT_MOVING_INV] = { "movinginv",  "Moving inversions", "0xAAAAAAAA rotating one bit per cell",
                            1, 1 },
    [MT_PAT_CHECKER]    = { "checker",    "Walking bit",      "one bit walks through a 32-bit cell",
                            1, 1 },
    [MT_PAT_BIT_ONE]    = { "bitone",     "Walking bit ones", "every bit driven high, one at a time",
                            32, 32 },
    [MT_PAT_BIT_ZERO]   = { "bitzero",    "Walking bit zeros", "every bit driven low, one at a time",
                            32, 32 },
    [MT_PAT_MODULO_X]   = { "modulox",    "Modulo X",         "64 offset/XOR sweeps, catches aliasing",
                            64, 64 },
    [MT_PAT_RANDOM]     = { "random",     "Pseudo random",    "xoshiro128** stream, fixed seed",
                            1, 1 },
};

const mt_pattern_desc_t *mt_pattern_desc(mt_pattern_t pat)
{
    if ((unsigned)pat >= MT_PATTERN_COUNT) {
        return NULL;
    }
    return &mt_pattern_table[pat];
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower(*a) != lower(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int mt_pattern_from_name(const char *name)
{
    if (name == NULL || *name == '\0') {
        return -1;
    }
    for (unsigned i = 0; i < MT_PATTERN_COUNT; i++) {
        if (ieq(mt_pattern_table[i].key, name)) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */

void mt_rng_seed(mt_rng_t *rng, uint64_t seed)
{
    /* SplitMix64 expansion, so that even seed 0 yields a healthy state. */
    uint64_t z = seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    rng->s[0] = z;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    rng->s[1] = (z ^ (z >> 31));
    if ((rng->s[0] | rng->s[1]) == 0) {
        rng->s[1] = 0x2545F4914F6CDD1DULL;
    }
    for (int i = 0; i < 8; i++) {
        (void)mt_rng_next(rng);
    }
}

uint32_t mt_rng_next(mt_rng_t *rng)
{
    uint64_t s0 = rng->s[0];
    uint64_t s1 = rng->s[1];
    uint64_t result = s0 + s1;
    s1 ^= s0;
    rng->s[0] = ((s0 << 55) | (s0 >> 9)) ^ s1 ^ (s1 << 14);
    rng->s[1] = (s1 << 36) | (s1 >> 28);
    return (uint32_t)((result >> 32) ^ result);
}

/* ------------------------------------------------------------------------- */

static inline uint32_t rotl32(uint32_t v, unsigned n)
{
    n &= 31u;
    return (v << n) | (v >> ((32u - n) & 31u));
}

/* Murmur3 finaliser: cheap, strong avalanche - ideal for address patterns. */
static inline uint32_t fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

/** Stream seed for the random pattern; both fill and verify use it. */
static inline uint64_t random_seed(uint32_t seed, uint32_t block_id)
{
    return ((uint64_t)seed << 32) ^ ((uint64_t)block_id * 0x9E3779B97F4A7C15ULL) ^ 0xD1B54A32D192ED03ULL;
}

static inline uint8_t popcount32(uint32_t v)
{
    uint8_t n = 0;
    while (v) {
        v &= v - 1u;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------- */

void mt_scan_reset(mt_scan_t *scan)
{
    if (scan) {
        memset(scan, 0, sizeof(*scan));
    }
}

static void record_error(mt_scan_t *scan, const mt_error_t *err)
{
    scan->cells_failed++;
    scan->bits_wrong += err->bits_wrong;
    if (!err->persistent) {
        scan->cells_flaky++;
    }
    if (!scan->has_first) {
        scan->has_first = true;
        scan->first = *err;
    }
    if (scan->detail_count < MT_MAX_DETAIL_ERRORS) {
        scan->details[scan->detail_count++] = *err;
    }
}

/* -------------------------------------------------------------------------
 * Verify helpers
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t block_id;
    const uint32_t *cells;
    size_t n;
    size_t i;
    mt_scan_t *scan;
    uint32_t failures;
} verify_state_t;

static inline void verify_cell(verify_state_t *v, uint32_t expected)
{
    uint32_t actual = v->cells[v->i];
    if (actual == expected) {
        return;
    }
    mt_error_t err;
    err.block_id = v->block_id;
    err.offset = (uint32_t)(v->i * 4u);
    err.addr = (uintptr_t)&v->cells[v->i];
    err.expected = expected;
    err.actual = actual;
    err.xor_diff = actual ^ expected;
    err.bits_wrong = popcount32(err.xor_diff);
    /* Re-read straight away: a value that comes back correct the second
     * time is a marginal (timing / cache / refresh) problem, not a stuck
     * bit, and it is worth reporting separately. */
    err.persistent = (v->cells[v->i] != expected);
    err.on_block_edge = (v->i < 8u) || (v->i + 8u >= v->n);
    v->failures++;
    if (v->scan) {
        record_error(v->scan, &err);
    }
}

/* ------------------------------------------------------------------------- */

uint32_t mt_pattern_apply(const mt_pattern_ctx_t *ctx, void *block, size_t bytes,
                          bool write, mt_scan_t *scan)
{
    uint32_t *cells = (uint32_t *)block;
    const size_t n = bytes / 4u;

    if (write) {
        switch (ctx->pattern) {
        case MT_PAT_ZERO:
            memset(block, 0x00, bytes);
            break;
        case MT_PAT_ONES:
            memset(block, 0xFF, bytes);
            break;
        case MT_PAT_BIT_ONE:
        case MT_PAT_BIT_ZERO: {
            uint32_t v = (ctx->pattern == MT_PAT_BIT_ONE) ? (1u << (ctx->sub & 31u))
                                                          : ~(1u << (ctx->sub & 31u));
            /* memset() is only correct when all four bytes of the cell are
             * identical, i.e. for the all-zero and all-one cells. */
            if (v == 0x00000000u || v == 0xFFFFFFFFu) {
                memset(block, (int)(v & 0xFFu), bytes);
            } else {
                for (size_t i = 0; i < n; i++) {
                    cells[i] = v;
                }
            }
            break;
        }
        case MT_PAT_AA55: {
            /* 8 words (32 bytes) per copy keeps the loop tight. */
            uint32_t chunk[8];
            for (unsigned k = 0; k < 8; k++) {
                chunk[k] = (k & 1u) ? 0xAAAAAAAAu : 0x55555555u;
            }
            for (size_t off = 0; off < bytes; off += sizeof(chunk)) {
                memcpy((uint8_t *)block + off, chunk, sizeof(chunk));
            }
            break;
        }
        case MT_PAT_MOVING_INV: {
            uint32_t chunk[32];
            for (unsigned k = 0; k < 32; k++) {
                chunk[k] = rotl32(0xAAAAAAAAu, k);
            }
            for (size_t off = 0; off < bytes; off += sizeof(chunk)) {
                memcpy((uint8_t *)block + off, chunk, sizeof(chunk));
            }
            break;
        }
        case MT_PAT_CHECKER: {
            uint32_t chunk[32];
            for (unsigned k = 0; k < 32; k++) {
                chunk[k] = 1u << k;
            }
            for (size_t off = 0; off < bytes; off += sizeof(chunk)) {
                memcpy((uint8_t *)block + off, chunk, sizeof(chunk));
            }
            break;
        }
        case MT_PAT_MODULO_X: {
            uint32_t salt = (ctx->sub + 1u) ^ 0x5A5A5A5Au;
            for (size_t i = 0; i < n; i++) {
                cells[i] = ~(uint32_t)i ^ salt;
            }
            break;
        }
        case MT_PAT_ADDRESS: {
            const uint32_t base = ctx->block_id * 0x9E3779B9u;
            const uint32_t salt = ctx->seed ^ 0x51ED2701u;
            for (size_t i = 0; i < n; i++) {
                /* Derive the value from the absolute address of the cell so
                 * that every byte lane carries independent information. */
                cells[i] = fmix32((base + (uint32_t)i) ^ salt);
            }
            break;
        }
        case MT_PAT_RANDOM: {
            mt_rng_t rng;
            mt_rng_seed(&rng, random_seed(ctx->seed, ctx->block_id));
            for (size_t i = 0; i < n; i++) {
                cells[i] = mt_rng_next(&rng);
            }
            break;
        }
        default:
            return 0;
        }
        return 0;
    }

    if (scan) {
        scan->cells_checked += n;
    }
    verify_state_t v = {
        .block_id = ctx->block_id,
        .cells = cells,
        .n = n,
        .i = 0,
        .scan = scan,
        .failures = 0,
    };

    switch (ctx->pattern) {
    case MT_PAT_ZERO:
        for (v.i = 0; v.i < n; v.i++) {
            verify_cell(&v, 0x00000000u);
        }
        break;
    case MT_PAT_ONES:
        for (v.i = 0; v.i < n; v.i++) {
            verify_cell(&v, 0xFFFFFFFFu);
        }
        break;
    case MT_PAT_BIT_ONE:
    case MT_PAT_BIT_ZERO: {
        uint32_t expected = (ctx->pattern == MT_PAT_BIT_ONE) ? (1u << (ctx->sub & 31u))
                                                            : ~(1u << (ctx->sub & 31u));
        for (v.i = 0; v.i < n; v.i++) {
            verify_cell(&v, expected);
        }
        break;
    }
    case MT_PAT_AA55:
        for (v.i = 0; v.i < n; v.i++) {
            verify_cell(&v, (v.i & 1u) ? 0xAAAAAAAAu : 0x55555555u);
        }
        break;
    case MT_PAT_MOVING_INV:
        for (v.i = 0; v.i < n; v.i++) {
            verify_cell(&v, rotl32(0xAAAAAAAAu, (unsigned)(v.i & 31u)));
        }
        break;
    case MT_PAT_CHECKER:
        for (v.i = 0; v.i < n; v.i++) {
            verify_cell(&v, 1u << (v.i & 31u));
        }
        break;
    case MT_PAT_MODULO_X: {
        const uint32_t salt = (ctx->sub + 1u) ^ 0x5A5A5A5Au;
        for (v.i = 0; v.i < n; v.i++) {
            verify_cell(&v, ~(uint32_t)v.i ^ salt);
        }
        break;
    }
    case MT_PAT_ADDRESS: {
        const uint32_t base = ctx->block_id * 0x9E3779B9u;
        const uint32_t salt = ctx->seed ^ 0x51ED2701u;
        for (v.i = 0; v.i < n; v.i++) {
            verify_cell(&v, fmix32((base + (uint32_t)v.i) ^ salt));
        }
        break;
    }
    case MT_PAT_RANDOM: {
        mt_rng_t rng;
        mt_rng_seed(&rng, random_seed(ctx->seed, ctx->block_id));
        for (v.i = 0; v.i < n; v.i++) {
            verify_cell(&v, mt_rng_next(&rng));
        }
        break;
    }
    default:
        break;
    }
    return v.failures;
}