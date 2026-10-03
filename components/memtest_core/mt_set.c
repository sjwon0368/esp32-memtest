/*
 * esp32-memtest - test set parsing and formatting helpers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "memtest_core.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */

const char *mt_testset_name(mt_testset_t set)
{
    switch (set) {
    case MT_SET_QUICK:    return "quick";
    case MT_SET_STANDARD: return "standard";
    case MT_SET_EXTENDED: return "extended";
    case MT_SET_FULL:     return "full";
    case MT_SET_RANDOM:   return "random";
    case MT_SET_WALKING:  return "walking";
    case MT_SET_NONE:     return "none";
    default:              return "?";
    }
}

static bool ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void add(mt_testset_spec_t *set, mt_pattern_t pat, uint32_t seed)
{
    for (size_t i = 0; i < set->count; i++) {
        if (set->tests[i].pattern == pat) {
            return;
        }
    }
    if (set->count >= MT_PATTERN_COUNT) {
        return;
    }
    set->tests[set->count].pattern = pat;
    set->tests[set->count].seed = seed;
    set->tests[set->count].passes = 0;
    set->count++;
}

/* MT_PATTERN_COUNT is used as the list terminator below; it is not a valid
 * pattern id, so every array has to spell out all remaining slots. */
#define END MT_PATTERN_COUNT

static const struct {
    const char *name;
    const uint8_t patterns[MT_PATTERN_COUNT + 1];
} mt_presets[] = {
    { "quick",    { MT_PAT_ZERO, MT_PAT_ONES, MT_PAT_AA55, MT_PAT_ADDRESS, MT_PAT_RANDOM, END } },
    { "standard", { MT_PAT_ZERO, MT_PAT_ONES, MT_PAT_AA55, MT_PAT_ADDRESS, MT_PAT_RANDOM,
                    MT_PAT_MOVING_INV, MT_PAT_CHECKER, END } },
    { "extended", { MT_PAT_ZERO, MT_PAT_ONES, MT_PAT_AA55, MT_PAT_ADDRESS, MT_PAT_RANDOM,
                    MT_PAT_MOVING_INV, MT_PAT_CHECKER, MT_PAT_BIT_ONE, MT_PAT_BIT_ZERO,
                    MT_PAT_MODULO_X, END } },
    { "full",     { MT_PAT_ZERO, MT_PAT_ONES, MT_PAT_AA55, MT_PAT_ADDRESS, MT_PAT_RANDOM,
                    MT_PAT_MOVING_INV, MT_PAT_CHECKER, MT_PAT_BIT_ONE, MT_PAT_BIT_ZERO,
                    MT_PAT_MODULO_X, END } },
    { "all",      { MT_PAT_ZERO, MT_PAT_ONES, MT_PAT_AA55, MT_PAT_ADDRESS, MT_PAT_RANDOM,
                    MT_PAT_MOVING_INV, MT_PAT_CHECKER, MT_PAT_BIT_ONE, MT_PAT_BIT_ZERO,
                    MT_PAT_MODULO_X, END } },
    { "random",   { MT_PAT_RANDOM, END } },
    { "walking",  { MT_PAT_ADDRESS, MT_PAT_CHECKER, MT_PAT_BIT_ONE, MT_PAT_BIT_ZERO, END } },
};

#undef END

static int add_named(mt_testset_spec_t *set, const char *token, size_t len, uint32_t seed)
{
    char buf[32];
    if (len == 0 || len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, token, len);
    buf[len] = '\0';

    for (unsigned i = 0; i < sizeof(mt_presets) / sizeof(mt_presets[0]); i++) {
        if (ieq(mt_presets[i].name, buf)) {
            unsigned k = 0;
            while (mt_presets[i].patterns[k] < MT_PATTERN_COUNT) {
                add(set, (mt_pattern_t)mt_presets[i].patterns[k], seed);
                k++;
            }
            return (int)k;
        }
    }

    int pat = mt_pattern_from_name(buf);
    if (pat >= 0) {
        add(set, (mt_pattern_t)pat, seed);
        return 1;
    }
    return -1;
}

int mt_testset_parse(const char *spec, uint32_t seed, mt_testset_spec_t *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (spec == NULL || *spec == '\0') {
        return add_named(out, "standard", 8, seed);
    }

    const char *p = spec;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len == 0) {
            continue;
        }
        if (len == 4 && memcmp(start, "none", 4) == 0) {
            out->count = 0;   /* clears anything selected before it */
            continue;
        }
        if (add_named(out, start, len, seed) < 0) {
            return -1;
        }
    }
    return (int)out->count;
}

/* ------------------------------------------------------------------------- */

const char *mt_fmt_bytes(char *buf, size_t buf_size, uint64_t bytes)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    if (buf == NULL || buf_size == 0) {
        return "";
    }
    double v = (double)bytes;
    unsigned u = 0;
    while (v >= 1024.0 && u < (sizeof(units) / sizeof(units[0])) - 1u) {
        v /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(buf, buf_size, "%llu B", (unsigned long long)bytes);
    } else if (v < 10.0) {
        snprintf(buf, buf_size, "%.2f %s", v, units[u]);
    } else if (v < 100.0) {
        snprintf(buf, buf_size, "%.1f %s", v, units[u]);
    } else {
        snprintf(buf, buf_size, "%.0f %s", v, units[u]);
    }
    return buf;
}

const char *mt_fmt_location(char *buf, size_t buf_size, const void *base, uint32_t offset)
{
    if (buf == NULL || buf_size == 0) {
        return "";
    }
    snprintf(buf, buf_size, "%p+0x%04lx", base, (unsigned long)offset);
    return buf;
}

const char *mt_fmt_hex(char *buf, size_t buf_size, uint32_t value)
{
    if (buf == NULL || buf_size == 0) {
        return "";
    }
    snprintf(buf, buf_size, "%08lx", (unsigned long)value);
    return buf;
}