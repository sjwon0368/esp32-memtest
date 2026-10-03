/*
 * esp32-memtest - portable, platform independent memory test engine.
 *
 * SPDX-License-Identifier: MIT
 *
 * Nothing in this component may include an ESP-IDF header: the whole point
 * of keeping the engine separate is that it can be compiled and unit tested
 * on a host PC (see tests/host).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */

/** Maximum number of separate allocations a test pool may hold. */
#define MT_MAX_BLOCKS          32u
/** Maximum number of individual error reports kept per test run. */
#define MT_MAX_DETAIL_ERRORS   8u
/** Maximum number of sub-passes a single pattern may be split into. */
#define MT_MAX_SUBPASSES       64u
/** Longest test-set/region specification string accepted. */
#define MT_MAX_SPEC_LEN        256u

/* -------------------------------------------------------------------------
 * Patterns
 * ------------------------------------------------------------------------- */

typedef enum {
    MT_PAT_ZERO = 0,          /**< all 0x00                                */
    MT_PAT_ONES,              /**< all 0xFF                                */
    MT_PAT_AA55,              /**< 0xAAAA5555... byte-lane alternation     */
    MT_PAT_ADDRESS,           /**< address-derived (fmix32 of the address) */
    MT_PAT_MOVING_INV,        /**< 0xAAAAAAAA rotated by one bit per cell  */
    MT_PAT_CHECKER,           /**< single walking one bit per 32-bit cell   */
    MT_PAT_BIT_ONE,           /**< every bit set everywhere, bit by bit     */
    MT_PAT_BIT_ZERO,          /**< every bit clear everywhere, bit by bit   */
    MT_PAT_MODULO_X,          /**< offset/XOR sweep, 64 sub-passes          */
    MT_PAT_RANDOM,            /**< deterministic pseudo-random data         */
    MT_PATTERN_COUNT
} mt_pattern_t;

typedef struct {
    const char *key;      /**< short identifier used on the command line */
    const char *name;     /**< name shown in the UI                      */
    const char *desc;     /**< one line description                      */
    uint8_t subpasses;    /**< number of sub-passes the pattern needs   */
    uint32_t est_mbs;     /**< rough traffic volume, used for ordering   */
} mt_pattern_desc_t;

/** Descriptor table indexed by mt_pattern_t. */
extern const mt_pattern_desc_t mt_pattern_table[MT_PATTERN_COUNT];

/** @return descriptor for @p pat, or NULL when out of range. */
const mt_pattern_desc_t *mt_pattern_desc(mt_pattern_t pat);

/** @return pattern id for @p name (case insensitive), -1 if unknown. */
int mt_pattern_from_name(const char *name);

/* -------------------------------------------------------------------------
 * Deterministic PRNG (xoshiro128**), used by the random pattern.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint64_t s[2];
} mt_rng_t;

void mt_rng_seed(mt_rng_t *rng, uint64_t seed);
uint32_t mt_rng_next(mt_rng_t *rng);

/* -------------------------------------------------------------------------
 * Pattern application
 * ------------------------------------------------------------------------- */

/** Everything needed to reproduce a pattern for one sub-pass. */
typedef struct {
    mt_pattern_t pattern;
    uint32_t sub;      /**< sub-pass index, < descriptor->subpasses  */
    uint32_t seed;     /**< random pattern seed                      */
    uint32_t block_id; /**< makes each pool block use its own stream  */
} mt_pattern_ctx_t;

/** A single mismatching 32-bit cell. */
typedef struct {
    uint32_t block_id;
    uint32_t offset;      /**< byte offset inside the block */
    uintptr_t addr;       /**< absolute address of the cell  */
    uint32_t expected;
    uint32_t actual;
    uint32_t xor_diff;
    uint8_t  bits_wrong;
    bool     persistent;  /**< re-read mismatch -> real fault            */
    bool     on_block_edge; /**< first/last 32 bytes of the block        */
} mt_error_t;

/** Result of verifying one block. */
typedef struct {
    uint64_t cells_checked;
    uint64_t cells_failed;
    uint64_t bits_wrong;
    uint64_t cells_flaky;   /**< mismatch that vanished on immediate re-read */
    bool     has_first;
    mt_error_t first;
    size_t   detail_count;
    mt_error_t details[MT_MAX_DETAIL_ERRORS];
} mt_scan_t;

void mt_scan_reset(mt_scan_t *scan);

/**
 * Write or verify one pattern over one block.
 *
 * @param ctx    pattern description
 * @param block  memory to write/verify, 4-byte aligned, at least 4 bytes
 * @param bytes  size of @p block, must be a multiple of 4
 * @param write  true to fill the block, false to verify it
 * @param scan   scan accumulator, may be NULL when @p write is true
 * @return number of mismatching cells found (0 when writing)
 */
uint32_t mt_pattern_apply(const mt_pattern_ctx_t *ctx, void *block, size_t bytes,
                          bool write, mt_scan_t *scan);

/* -------------------------------------------------------------------------
 * Test pool
 * ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *addr;
    size_t   size;
} mt_block_t;

typedef struct {
    mt_block_t blocks[MT_MAX_BLOCKS];
    size_t     count;
    size_t     total_bytes;
    size_t     largest_block;
    uintptr_t  lowest_addr;
    uintptr_t  highest_addr;
} mt_pool_t;

typedef struct {
    size_t   block_bytes;   /**< preferred size of a single allocation */
    size_t   target_bytes;  /**< total size to obtain, 0 = grab as much as possible */
    size_t   min_block;     /**< stop when allocations get smaller than this */
    uint32_t caps;          /**< heap capabilities requested (informational) */
} mt_pool_request_t;

typedef void *(*mt_alloc_fn)(size_t bytes, uint32_t caps, void *user);
typedef void  (*mt_free_fn)(void *ptr, void *user);

/**
 * Try to obtain a pool of memory totalling close to @p req->target_bytes.
 * Requests are shrunk automatically until allocations succeed, so a
 * fragmented heap still yields a (smaller) usable pool.
 *
 * @return true when at least one block was obtained.
 */
bool mt_pool_acquire(mt_pool_t *pool, const mt_pool_request_t *req,
                     mt_alloc_fn alloc, void *alloc_user);

/** Release every block of @p pool and reset it. */
void mt_pool_release(mt_pool_t *pool, mt_free_fn release, void *free_user);

/* -------------------------------------------------------------------------
 * Test runner
 * ------------------------------------------------------------------------- */

typedef struct {
    mt_pattern_t pattern;
    uint32_t     seed;      /**< 0 = use the global seed */
    uint32_t     passes;    /**< 0 = repeat until aborted */
} mt_test_spec_t;

typedef struct {
    const char *state;      /**< "run"/"verify"/"pass 2" - free text for the UI */
    uint32_t    pass;
    uint32_t    passes_total;
    uint32_t    sub;
    uint32_t    subs_total;
    uint64_t    bytes_done;
    uint64_t    bytes_total;
    uint32_t    percent;    /**< 0..100 */
    bool        finished;
} mt_progress_t;

typedef void (*mt_progress_cb)(const mt_progress_t *progress, void *user);

/** Monotonic microsecond clock, supplied by the platform layer. */
typedef uint64_t (*mt_clock_fn)(void);
/** Called periodically so the RTOS keeps running while a test is busy. */
typedef void (*mt_yield_fn)(void);

typedef struct {
    const char *name;
    uint64_t    bytes;          /**< bytes covered by the pool        */
    uint64_t    cells;          /**< 32-bit cells verified             */
    uint64_t    cells_failed;
    uint64_t    bits_wrong;
    uint64_t    cells_flaky;
    uint32_t    passes;         /**< completed passes, 0xFFFFFFFF = stopped */
    uint32_t    passes_target;
    double      seconds;
    double      mbps;
    uint64_t    passes_aborted; /**< 1 when the user aborted early     */
    size_t      error_count;
    mt_error_t  errors[MT_MAX_DETAIL_ERRORS];
    bool        complete;       /**< false when stopped early           */
} mt_result_t;

/** Reasons a test run stopped. */
typedef enum {
    MT_STOP_NONE = 0,
    MT_STOP_FINISHED,
    MT_STOP_USER,
    MT_STOP_KEYBOARD,
} mt_stop_reason_t;

/**
 * Run one pattern over the whole pool.
 *
 * @param aborted   set to true (by @p progress_cb) to stop as soon as
 *                  possible; the result is then flagged as incomplete.
 */
void mt_run_test(mt_pool_t *pool, const mt_test_spec_t *spec, uint32_t default_seed,
                 mt_clock_fn clock, mt_yield_fn yield_fn,
                 mt_progress_cb progress, void *progress_user,
                 volatile bool *aborted,
                 mt_result_t *result, mt_stop_reason_t *stop_reason);

/* -------------------------------------------------------------------------
 * Test set / region specification strings
 * ------------------------------------------------------------------------- */

typedef enum {
    MT_SET_NONE = 0,
    MT_SET_QUICK,      /**< zero, ones, aa55, address, random                */
    MT_SET_STANDARD,   /**< quick + moving inversions + checker              */
    MT_SET_EXTENDED,   /**< standard + walking bit ones/zeros + modulo X     */
    MT_SET_FULL,       /**< every pattern                                   */
    MT_SET_RANDOM,     /**< random only - good for long soak tests           */
    MT_SET_WALKING,    /**< address + walking bit patterns                   */
    MT_SET_COUNT
} mt_testset_t;

typedef struct {
    mt_test_spec_t tests[MT_PATTERN_COUNT];
    size_t count;
} mt_testset_spec_t;

const char *mt_testset_name(mt_testset_t set);

/**
 * Parse a comma separated list of test names / set names.
 *
 * Accepts: quick, standard, extended, full, random, walking, single,
 * all, none and any individual pattern key.
 *
 * @return number of tests in @p out, or -1 on a parse error.
 */
int mt_testset_parse(const char *spec, uint32_t seed, mt_testset_spec_t *out);

/* -------------------------------------------------------------------------
 * Formatting helpers (pure, no libc locale dependencies)
 * ------------------------------------------------------------------------- */

/** @return human readable size, e.g. "7.50 MiB". Always NUL terminated. */
const char *mt_fmt_bytes(char *buf, size_t buf_size, uint64_t bytes);

/** @return "0x3fc8b200+0x1a4c" style location string. */
const char *mt_fmt_location(char *buf, size_t buf_size, const void *base, uint32_t offset);

/** @return lowercase hex, always NUL terminated, truncates if needed. */
const char *mt_fmt_hex(char *buf, size_t buf_size, uint32_t value);

#ifdef __cplusplus
}
#endif