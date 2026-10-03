/*
 * esp32-memtest - terminal screen renderer.
 *
 * SPDX-License-Identifier: MIT
 *
 * The screen is 80 columns wide and rows are laid out from an exact field
 * budget so that nothing ever wraps - a wrapped row would destroy the frame
 * and flicker the whole display.
 */

#include "memtest_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_MEMTEST_UI_ANSI) || defined(MT_UI_FORCE_ANSI)
#define MT_UI_ANSI_DEFAULT 1
#else
#define MT_UI_ANSI_DEFAULT 0
#endif

/* ------------------------------------------------------------------------- */
/* Layout budget                                                            */
/* ------------------------------------------------------------------------- */

/** Inner width of every screen row (the row is "| " + inner + " |"). */
#define MT_UI_INNER (MT_UI_WIDTH - 2)

/* field widths, per row type - every row sums to MT_UI_INNER */
#define W_TITLE       24
#define W_SUBTITLE    26
#define W_TRAILER     (MT_UI_INNER - W_TITLE - W_SUBTITLE)
#define W_NOTE1       34
#define W_NOTE2       22
#define W_NOTE3       (MT_UI_INNER - W_NOTE1 - W_NOTE2)
#define W_MEM_NAME    10
#define W_MEM_TOTAL   11
#define W_MEM_FREE    11
#define W_MEM_TESTED  11
#define W_MEM_BLOCKS  5
#define W_MEM_GAP     1
#define W_MEM_BAR     (MT_UI_INNER - W_MEM_NAME - W_MEM_TOTAL - W_MEM_FREE - \
                       W_MEM_TESTED - W_MEM_BLOCKS - W_MEM_GAP)
#define W_ROW_REGION   8
#define W_ROW_NAME    19
#define W_ROW_PASS     7
#define W_ROW_TIME    10
#define W_ROW_RATE    12
#define W_ROW_ERRORS  11
#define W_ROW_VERDICT (MT_UI_INNER - W_ROW_REGION - W_ROW_NAME - W_ROW_PASS - \
                       W_ROW_TIME - W_ROW_RATE - W_ROW_ERRORS)
#define W_PRG_BAR     20
#define W_PRG_PERCENT  5
#define W_PRG_NAME    17
#define W_PRG_PHASE    7
#define W_PRG_PASS     9
#define W_PRG_BYTES    12
#define W_PRG_STATE    6
#define W_ST_HEAP     25
#define W_ST_UP       11
#define W_ST_ERRORS   10
#define W_ST_KEYS     (MT_UI_INNER - W_ST_HEAP - W_ST_UP - W_ST_ERRORS)

/* ------------------------------------------------------------------------- */
/* Output primitives                                                        */
/* ------------------------------------------------------------------------- */

#define ESC              "\x1b"
#define ANSI_ALT_ON      ESC "[?1049h"
#define ANSI_ALT_OFF     ESC "[?1049l"
#define ANSI_CURSOR_HIDE ESC "[?25l"
#define ANSI_CURSOR_SHOW ESC "[?25h"
#define ANSI_CLEAR       ESC "[2J"
#define ANSI_HOME        ESC "[H"
#define ANSI_RESET       ESC "[0m"
#define ANSI_BOLD        ESC "[1m"
#define ANSI_RED         ESC "[31m"
#define ANSI_GREEN       ESC "[32m"
#define ANSI_YELLOW      ESC "[33m"
#define ANSI_CYAN        ESC "[36m"

static mt_ui_mode_t s_mode = MT_UI_PLAIN;
static bool         s_started;
static bool         s_interactive = true;
static bool         s_help_visible;

/* Rows the layout is fitted to.  One less than a 24 line terminal: every row
 * ends with a newline, so writing a 24th line would scroll the screen and
 * push the top border off. */
#define MT_UI_TERM_ROWS 23

#define LOG_LINES 3
static char s_log[LOG_LINES][MT_UI_INNER + 1];
static int   s_log_count;

static char   s_out[6144];
static size_t s_out_len;

static uint32_t s_row_seq_seen[MT_UI_ROWS];

/* ------------------------------------------------------------------------- */

static void out_printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (s_out_len + 1u >= sizeof(s_out)) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(s_out + s_out_len, sizeof(s_out) - s_out_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s_out_len += (size_t)n;
        if (s_out_len + 1u >= sizeof(s_out)) {
            s_out_len = sizeof(s_out) - 2u;   /* truncated but still valid */
        }
    }
}

static void out_flush(void)
{
    if (s_out_len > 0) {
        fwrite(s_out, 1, s_out_len, stdout);
        fflush(stdout);
        s_out_len = 0;
    }
}

static size_t str_len(const char *s)
{
    return s ? strlen(s) : 0;
}

/** Emit @p text in exactly @p width columns, truncating when too long. */
static void pad(const char *text, size_t width)
{
    size_t len = str_len(text);
    if (len > width) {
        len = width;
    }
    for (size_t i = 0; i < len; i++) {
        out_printf("%c", text[i]);
    }
    for (size_t i = len; i < width; i++) {
        out_printf(" ");
    }
}

static void row_begin(void)
{
    out_printf("| ");
}

static void row_end(void)
{
    out_printf(" |\n");   /* '| ' + MT_UI_INNER + ' |' == MT_UI_WIDTH + 2 */
}

static void row(const char *text)
{
    row_begin();
    pad(text, MT_UI_INNER);
    row_end();
}

static void rule(char left, char fill, char right)
{
    out_printf("%c", left);
    /* MT_UI_WIDTH + 2 columns in total, matching the '| ' + MT_UI_INNER + ' |'
     * content rows emitted by row_end(), so the box lines up. */
    for (int i = 0; i < MT_UI_WIDTH; i++) {
        out_printf("%c", fill);
    }
    out_printf("%c\n", right);
}

static void bytes_str(char *buf, size_t size, uint64_t v)
{
    mt_fmt_bytes(buf, size, v);
}

/** Compact "5.0/7.0Mi" progress pair; never longer than 12 characters. */
static void bytes_pair(char *buf, size_t size, uint64_t done, uint64_t total)
{
    if (total >= 1024u * 1024u) {
        snprintf(buf, size, "%.0f/%.0fMi", (double)done / 1048576.0, (double)total / 1048576.0);
    } else {
        snprintf(buf, size, "%.0f/%.0fKi", (double)done / 1024.0, (double)total / 1024.0);
    }
}

static void bar(char *buf, unsigned percent, unsigned width)
{
    unsigned filled = (percent * width) / 100u;
    if (filled > width) {
        filled = width;
    }
    unsigned i = 0;
    for (; i < filled; i++) {
        buf[i] = '#';
    }
    for (; i < width; i++) {
        buf[i] = '.';
    }
    buf[width] = '\0';
}

static const char *state_word(mt_ui_run_state_t state)
{
    switch (state) {
    case MT_UI_STATE_RUN:    return "RUN";
    case MT_UI_STATE_PAUSED: return "PAUSE";
    case MT_UI_STATE_DONE:   return "DONE";
    default:                 return "IDLE";
    }
}

static const char *state_color(mt_ui_run_state_t state)
{
    switch (state) {
    case MT_UI_STATE_RUN:    return ANSI_GREEN;
    case MT_UI_STATE_PAUSED: return ANSI_YELLOW;
    case MT_UI_STATE_DONE:   return ANSI_CYAN;
    default:                 return "";
    }
}

/* ------------------------------------------------------------------------- */
/* Screen sections                                                          */
/* ------------------------------------------------------------------------- */

static size_t draw_header(const mt_ui_banner_t *b)
{
    char buf[64];

    rule('+', '-', '+');
    if (b == NULL) {
        row("esp32-memtest");
        rule('+', '-', '+');
        return 4;
    }

    row_begin();
    out_printf("%s", ANSI_BOLD);
    snprintf(buf, sizeof(buf), "esp32-memtest %s", b->version ? b->version : "?");
    pad(buf, W_TITLE);
    out_printf("%s", ANSI_RESET);
    snprintf(buf, sizeof(buf), "%s rev%d  %dMHz  %dc", b->chip ? b->chip : "?", b->revision,
             b->cpu_mhz, b->cores);
    pad(buf, W_SUBTITLE);
    snprintf(buf, sizeof(buf), "ESP-IDF %s", b->idf_version ? b->idf_version : "?");
    pad(buf, W_TRAILER);
    row_end();

    {
        char ps[24], il[24], fl[24];
        bytes_str(ps, sizeof(ps), b->psram_size);
        bytes_str(il, sizeof(il), b->internal_total);
        bytes_str(fl, sizeof(fl), b->flash_bytes);

        row_begin();
        if (b->psram_present) {
            if (b->psram_mallocable) {
                snprintf(buf, sizeof(buf), "PSRAM %s %s (in heap)", ps,
                         b->psram_mode ? b->psram_mode : "");
            } else {
                out_printf("%s", ANSI_RED);
                snprintf(buf, sizeof(buf), "PSRAM %s NOT in heap", ps);
            }
        } else {
            snprintf(buf, sizeof(buf), "no PSRAM - internal DRAM only");
        }
        pad(buf, W_NOTE1);
        if (!b->psram_present || !b->psram_mallocable) {
            out_printf("%s", ANSI_RESET);
        }
        snprintf(buf, sizeof(buf), "internal DRAM %s", il);
        pad(buf, W_NOTE2);
        snprintf(buf, sizeof(buf), "flash %s", fl);
        pad(buf, W_NOTE3);
        row_end();
    }
    rule('+', '-', '+');
    return 4;
}

static size_t draw_memory(const mt_ui_state_t *st, size_t max_rows)
{
    char total[24], freem[24], tested[24];
    char coverage[64];
    char buf[64];
    size_t shown = 0;
    size_t skipped = 0;

    row_begin();
    pad("MEMORY", W_MEM_NAME);
    pad("TOTAL", W_MEM_TOTAL);
    pad("FREE", W_MEM_FREE);
    pad("TESTED", W_MEM_TESTED);
    pad("BLOCKS", W_MEM_BLOCKS);
    pad("", W_MEM_GAP);
    pad("COVERAGE", W_MEM_BAR);
    row_end();

    for (size_t i = 0; i < st->region_count; i++) {
        const mt_ui_region_t *r = &st->regions[i];
        if (!r->selected) {
            continue;
        }
        if (shown >= max_rows) {
            skipped++;
            continue;
        }
        bytes_str(total, sizeof(total), r->total);
        bytes_str(freem, sizeof(freem), r->free_bytes);
        bytes_str(tested, sizeof(tested), r->tested_bytes);
        bar(coverage, r->used_percent, 20);

        row_begin();
        if (!r->usable) {
            out_printf("%s", ANSI_RED);
        }
        {
            char name[32];
            snprintf(name, sizeof(name), "%c%s", r->usable ? ' ' : '!',
                     r->short_name ? r->short_name : "?");
            pad(name, W_MEM_NAME);
        }
        pad(total, W_MEM_TOTAL);
        pad(freem, W_MEM_FREE);
        pad(tested, W_MEM_TESTED);
        snprintf(buf, sizeof(buf), "%u", (unsigned)r->blocks);
        pad(buf, W_MEM_BLOCKS);
        pad("", W_MEM_GAP);
        snprintf(buf, sizeof(buf), "%s %3u%%", coverage, (unsigned)r->used_percent);
        pad(buf, W_MEM_BAR);
        out_printf("%s", ANSI_RESET);
        row_end();
        shown++;
    }
    if (skipped > 0) {
        char note[32];
        snprintf(note, sizeof(note), "+%u more region(s)", (unsigned)skipped);
        row(note);
        shown++;
    }
    rule('+', '-', '+');
    return shown + 2u;
}

static size_t draw_results(const mt_ui_state_t *st)
{
    row_begin();
    pad("REGION", W_ROW_REGION);
    pad("TEST", W_ROW_NAME);
    pad("PASS", W_ROW_PASS);
    pad("TIME", W_ROW_TIME);
    pad("THROUGHPUT", W_ROW_RATE);
    pad("ERRORS", W_ROW_ERRORS);
    pad("RESULT", W_ROW_VERDICT);
    row_end();

    for (size_t i = 0; i < MT_UI_ROWS; i++) {
        if (i >= st->row_count || st->rows[i].region == NULL) {
            row("");           /* spare slot: no history entry yet */
            continue;
        }
        const mt_ui_row_t *r = &st->rows[i];
        char pass[16], time_s[16], rate[16], errors[48];
        const char *verdict;
        const char *color;

        if (r->passes_target == 0 || r->passes_target == 0xFFFFFFFFu) {
            snprintf(pass, sizeof(pass), "%u/*", (unsigned)r->passes);
        } else {
            snprintf(pass, sizeof(pass), "%u/%u", (unsigned)r->passes, (unsigned)r->passes_target);
        }
        snprintf(time_s, sizeof(time_s), "%.2fs", r->seconds);
        snprintf(rate, sizeof(rate), "%.1fMB/s", r->mbps);
        if (r->flaky) {
            snprintf(errors, sizeof(errors), "%llu+%llu", (unsigned long long)r->errors,
                     (unsigned long long)r->flaky);
        } else {
            snprintf(errors, sizeof(errors), "%llu", (unsigned long long)r->errors);
        }

        if (r->active) {
            verdict = "...";
            color = ANSI_YELLOW;
        } else if (!r->ok) {
            verdict = "FAIL";
            color = ANSI_RED;
        } else if (!r->complete) {
            verdict = "STOP";
            color = ANSI_YELLOW;
        } else {
            verdict = "PASS";
            color = ANSI_GREEN;
        }

        row_begin();
        out_printf("%s", color);
        pad(r->region ? r->region : "", W_ROW_REGION);
        pad(r->pattern ? r->pattern : "", W_ROW_NAME);
        pad(pass, W_ROW_PASS);
        pad(time_s, W_ROW_TIME);
        pad(rate, W_ROW_RATE);
        pad(errors, W_ROW_ERRORS);
        pad(verdict, W_ROW_VERDICT);
        out_printf("%s", ANSI_RESET);
        row_end();
    }
    rule('+', '-', '+');
    return MT_UI_ROWS + 2u;
}

static size_t draw_progress(const mt_ui_state_t *st)
{
    const mt_ui_frame_t *f = st->frame;
    char progress[64];
    char buf[48];

    if (f == NULL) {
        row("");
        return 1;
    }
    bar(progress, f->percent, W_PRG_BAR - 2);

    row_begin();
    out_printf("%s[", state_color(f->state));
    out_printf("%s", progress);
    out_printf("]%s", ANSI_RESET);
    {
        char pct[8];
        snprintf(pct, sizeof(pct), "%3u%%", (unsigned)(f->percent > 100u ? 100u : f->percent));
        pad(pct, W_PRG_PERCENT);
    }
    /* one blank column keeps a long test name from running into the phase */
    pad(f->test_name ? f->test_name : "-", W_PRG_NAME - 1u);
    pad("", 1);
    pad(f->phase ? f->phase : "", W_PRG_PHASE);
    if (f->passes_total == 0u || f->passes_total == 0xFFFFFFFFu) {
        snprintf(buf, sizeof(buf), "p%u/*", (unsigned)f->pass);
    } else {
        snprintf(buf, sizeof(buf), "p%u/%u", (unsigned)f->pass, (unsigned)f->passes_total);
    }
    pad(buf, W_PRG_PASS);
    bytes_pair(buf, sizeof(buf), f->bytes_done, f->bytes_total);
    pad(buf, W_PRG_BYTES);
    out_printf("%s", state_color(f->state));
    pad(state_word(f->state), W_PRG_STATE);
    out_printf("%s", ANSI_RESET);
    row_end();
    return 1;
}

static size_t draw_status(const mt_ui_state_t *st)
{
    const mt_ui_frame_t *f = st->frame;
    char freeh[24], minf[24];
    char buf[64];

    if (f == NULL) {
        row("");
        return 1;
    }
    bytes_str(freeh, sizeof(freeh), f->free_heap);
    bytes_str(minf, sizeof(minf), f->min_free_heap);

    row_begin();
    snprintf(buf, sizeof(buf), "heap %.9s min %.9s", freeh, minf);
    pad(buf, W_ST_HEAP);
    snprintf(buf, sizeof(buf), "up %llu.%02llus", (unsigned long long)(f->uptime_ms / 1000u),
             (unsigned long long)((f->uptime_ms % 1000u) / 10u));
    pad(buf, W_ST_UP);
    snprintf(buf, sizeof(buf), "errors %llu", (unsigned long long)f->total_errors);
    pad(buf, W_ST_ERRORS);
    /* The available keys depend on what the tester is doing. Once the run is
     * over the pause/skip keys are meaningless, so say what actually works:
     * run it again, show the report, or leave. */
    if (!f->interactive) {
        pad("[keys disabled]", W_ST_KEYS);
    } else if (f->state == MT_UI_STATE_DONE) {
        pad("[r]un [Enter] report [q]uit", W_ST_KEYS);
    } else {
        pad("[h]elp [p]ause [s]kip [q]uit", W_ST_KEYS);
    }
    row_end();
    return 1;
}

/** The event log; empty when nothing was logged, so it costs no rows. */
static size_t draw_log(void)
{
    if (s_log_count <= 0) {
        return 0;
    }
    for (int i = 0; i < LOG_LINES; i++) {
        int idx = s_log_count - LOG_LINES + i;
        if (idx < 0) {
            row("");
        } else {
            row(s_log[idx]);
        }
    }
    return (size_t)LOG_LINES;
}

static void draw_help(void)
{
    rule('+', '=', '+');
    row("KEYS");
    row("  p  pause / resume          s  skip the running test");
    row("  n  next test now           q  stop and print the summary");
    row("  r  run the suite again     u  switch ANSI / plain output");
    row("  h  toggle this help        UP/DOWN scroll the test list");
    rule('+', '=', '+');
}

/** Rows the fixed parts of the layout use, before regions and log lines. */
#define LAYOUT_FIXED_ROWS 11u   /* header 4 + mem 2 + tests 2 + progress, status, rule */

static size_t draw_screen(const mt_ui_state_t *st)
{
    size_t used = 0;
    size_t max_regions;

    out_printf("%s%s", ANSI_HOME, ANSI_RESET);

    if (st->show_help) {
        /* The overlay owns the whole screen: anything else would scroll the
         * terminal and push the layout around. */
        draw_help();                     /* 7 rows */
        used = 7u;
    } else {
        used += draw_header(st->banner);
        /* rows that may go to the region table, keeping room for the log */
        max_regions = (MT_UI_TERM_ROWS > LAYOUT_FIXED_ROWS + (size_t)LOG_LINES)
                          ? (size_t)(MT_UI_TERM_ROWS - LAYOUT_FIXED_ROWS - (size_t)LOG_LINES)
                          : 1u;
        used += draw_memory(st, max_regions);
        used += draw_results(st);
        used += draw_progress(st);
        used += draw_status(st);
        used += draw_log();
    }

    /* Pad to a full screen: the repaint has no erase, so a shorter frame
     * would leave fragments of the previous one behind. */
    while (used < MT_UI_TERM_ROWS - 1u) {
        row("");
        used++;
    }
    rule('+', '-', '+');
    out_flush();
    return used + 1u;
}


/* ------------------------------------------------------------------------- */
/* Public API                                                               */
/* ------------------------------------------------------------------------- */

void mt_ui_log(const char *fmt, ...)
{
    char msg[MT_UI_INNER + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (s_mode == MT_UI_PLAIN) {
        printf("%s\r\n", msg);
        fflush(stdout);
        return;
    }
    if (s_log_count < LOG_LINES) {
        snprintf(s_log[s_log_count], sizeof(s_log[0]), "%s", msg);
    } else {
        memmove(s_log[0], s_log[1], (LOG_LINES - 1) * sizeof(s_log[0]));
        snprintf(s_log[LOG_LINES - 1], sizeof(s_log[LOG_LINES - 1]), "%s", msg);
    }
    s_log_count++;
}

void mt_ui_raw(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    /* Erase to end of line, but only when we are driving a real ANSI screen.
     * In plain mode the output has to stay free of escape sequences so it can
     * be pasted into a bug report. */
    if (s_mode == MT_UI_ANSI) {
        printf("%s\x1b[K\r\n", msg);
    } else {
        printf("%s\r\n", msg);
    }
    fflush(stdout);
}

void mt_ui_rule(void)
{
    char buf[MT_UI_WIDTH + 4];
    size_t i;
    buf[0] = '+';
    for (i = 1; i <= MT_UI_WIDTH; i++) {
        buf[i] = '-';
    }
    buf[MT_UI_WIDTH + 1] = '+';
    buf[MT_UI_WIDTH + 2] = '\0';
    mt_ui_raw("%s", buf);
}

void mt_ui_help_visible(bool visible)
{
    s_help_visible = visible;
}

void mt_ui_set_interactive(bool on)
{
    s_interactive = on;
}

bool mt_ui_interactive(void)
{
    return s_interactive;
}

mt_ui_mode_t mt_ui_mode(void)
{
    return s_mode;
}

mt_ui_mode_t mt_ui_toggle_mode(void)
{
    mt_ui_mode_t previous = s_mode;
    mt_ui_end();
    s_mode = (previous == MT_UI_ANSI) ? MT_UI_PLAIN : MT_UI_ANSI;
    mt_ui_begin(s_mode);
    return s_mode;
}

void mt_ui_begin(mt_ui_mode_t mode)
{
    s_mode = (mode == MT_UI_AUTO) ? (MT_UI_ANSI_DEFAULT ? MT_UI_ANSI : MT_UI_PLAIN) : mode;
    s_log_count = 0;
    memset(s_log, 0, sizeof(s_log));
    memset(s_row_seq_seen, 0, sizeof(s_row_seq_seen));
    s_started = true;

    if (s_mode == MT_UI_ANSI) {
        /* The alternate screen buffer keeps the terminal scrollback clean, so
         * that the final report is the only thing left on screen. */
        fputs(ANSI_ALT_ON ANSI_CURSOR_HIDE ANSI_CLEAR ANSI_HOME ANSI_RESET, stdout);
        fflush(stdout);
    }
}

void mt_ui_end(void)
{
    if (s_started && s_mode == MT_UI_ANSI) {
        fputs(ANSI_RESET ANSI_CURSOR_SHOW ANSI_ALT_OFF, stdout);
        fflush(stdout);
    }
    s_started = false;
}

void mt_ui_draw(const mt_ui_state_t *state)
{
    if (state == NULL) {
        return;
    }
    if (s_mode == MT_UI_ANSI) {
        draw_screen(state);
        return;
    }

    /* Plain mode: emit a line whenever a result row has actually changed. */
    for (size_t i = 0; i < state->row_count && i < MT_UI_ROWS; i++) {
        const mt_ui_row_t *r = &state->rows[i];
        if (r->seq == s_row_seq_seen[i]) {
            continue;
        }
        s_row_seq_seen[i] = r->seq;

        char rate[16];
        snprintf(rate, sizeof(rate), "%.1f MB/s", r->mbps);
        if (r->active) {
            mt_ui_raw("%-6s %-16s running", r->region ? r->region : "",
                      r->pattern ? r->pattern : "");
        } else if (!r->ok) {
            mt_ui_raw("%-6s %-16s FAIL  %llu errors (%llu flaky)  %.2f s  %s",
                      r->region ? r->region : "", r->pattern ? r->pattern : "",
                      (unsigned long long)r->errors, (unsigned long long)r->flaky, r->seconds,
                      rate);
        } else if (!r->complete) {
            mt_ui_raw("%-6s %-16s STOP  %u/%u pass  %.2f s", r->region ? r->region : "",
                      r->pattern ? r->pattern : "", (unsigned)r->passes,
                      (unsigned)r->passes_target, r->seconds);
        } else {
            mt_ui_raw("%-6s %-16s PASS  %u/%u pass  %.2f s  %s", r->region ? r->region : "",
                      r->pattern ? r->pattern : "", (unsigned)r->passes,
                      (unsigned)r->passes_target, r->seconds, rate);
        }
    }
}