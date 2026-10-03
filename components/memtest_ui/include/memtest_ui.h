/*
 * esp32-memtest - serial terminal user interface.
 *
 * SPDX-License-Identifier: MIT
 *
 * Two rendering modes share one state model:
 *
 *   ANSI   full screen repaint through the alternate screen buffer, with a
 *          progress bar, live tables and a small event log.  Requires a
 *          terminal that understands ANSI escape sequences (screen, PuTTY,
 *          minicom, idf.py monitor - all of them do).
 *
 *   PLAIN  one line per event, for dumb terminals, log files and CI.
 *
 * The mode can be switched at runtime with the 'u' key, so a user who ends up
 * in a terminal that cannot cope never has to reflash.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memtest_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Total line width including both borders. The default of 78 renders an
 * 80-column screen, which is what a stock 80x24 terminal expects. Widen it
 * with CONFIG_MEMTEST_UI_WIDTH (or -DMT_UI_WIDTH=) *and* give the terminal at
 * least that many columns, otherwise every row wraps and the box is ruined. */
#ifndef MT_UI_WIDTH
#define MT_UI_WIDTH  78
#endif
/* Result rows kept on screen. The live layout spends 13 rows on the banner,
 * memory table, headers and footer, so 10 rows plus those still fits the 24
 * lines of a stock terminal. */
#define MT_UI_ROWS   10

typedef enum {
    MT_UI_AUTO = 0,   /**< follow CONFIG_MEMTEST_UI_ANSI */
    MT_UI_ANSI,
    MT_UI_PLAIN,
} mt_ui_mode_t;

/* ------------------------------------------------------------------------- */

typedef struct {
    const char *version;
    const char *chip;
    int         revision;
    int         cpu_mhz;
    int         cores;
    const char *idf_version;
    size_t      flash_bytes;
    bool        psram_present;
    bool        psram_mallocable;
    size_t      psram_size;
    const char *psram_mode;
    size_t      internal_total;
    uint32_t    seed;
    bool        tests_ok;      /**< engine self test passed */
} mt_ui_banner_t;

typedef struct {
    const char *short_name;
    const char *name;
    bool        selected;
    bool        usable;
    size_t      total;
    size_t      free_bytes;
    size_t      tested_bytes;
    uint32_t    blocks;
    uint32_t    used_percent;  /**< fraction of the region covered by the pool */
} mt_ui_region_t;

typedef struct {
    uint32_t    seq;          /**< bump whenever the row changes, so plain
                                   mode knows what it already printed */
    const char *region;
    const char *pattern;
    uint32_t    passes;
    uint32_t    passes_target;
    double      seconds;
    double      mbps;
    uint64_t    errors;
    uint64_t    flaky;
    bool        ok;
    bool        complete;
    bool        active;
} mt_ui_row_t;

typedef enum {
    MT_UI_STATE_IDLE = 0,
    MT_UI_STATE_RUN,
    MT_UI_STATE_PAUSED,
    MT_UI_STATE_DONE,
} mt_ui_run_state_t;

typedef struct {
    const char   *title;          /**< region currently under test */
    const char   *test_name;
    const char   *phase;          /**< "write", "verify", ... */
    mt_ui_run_state_t state;
    uint32_t      pass;
    uint32_t      passes_total;
    uint32_t      sub;
    uint32_t      subs_total;
    uint32_t      percent;
    size_t        bytes_done;
    size_t        bytes_total;
    size_t        free_heap;
    size_t        min_free_heap;
    uint64_t      uptime_ms;
    uint64_t      total_errors;
    bool          interactive;    /**< keyboard handling enabled */
} mt_ui_frame_t;

/** Everything the screen shows; owned by the caller. */
typedef struct {
    const mt_ui_banner_t *banner;
    const mt_ui_region_t *regions;
    size_t                region_count;
    const mt_ui_row_t    *rows;
    size_t                row_count;
    const mt_ui_frame_t  *frame;
    bool                  show_help;
} mt_ui_state_t;

/* ------------------------------------------------------------------------- */

void        mt_ui_begin(mt_ui_mode_t mode);
void        mt_ui_end(void);
mt_ui_mode_t mt_ui_mode(void);
mt_ui_mode_t mt_ui_toggle_mode(void);
bool        mt_ui_interactive(void);
void        mt_ui_set_interactive(bool on);

/** Repaint the screen (ANSI) or emit any new event (plain). */
void mt_ui_draw(const mt_ui_state_t *state);

/** One-off message. In ANSI mode it lands in the event log. */
void mt_ui_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Free-form text output, bypassing the screen (used for reports). */
void mt_ui_raw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Horizontal rule, for plain mode reports. */
void mt_ui_rule(void);

/** Draw the help overlay state; call mt_ui_draw() afterwards. */
void mt_ui_help_visible(bool visible);

#ifdef __cplusplus
}
#endif