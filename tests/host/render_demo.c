/*
 * Screen replay harness for esp32-memtest.
 *
 * The firmware cannot be tested on the build machine, but the ANSI output it
 * produces can be: this program feeds realistic states through the real
 * renderer, captures the byte stream and replays it through a tiny terminal
 * emulator.  That catches the bugs a text unit test cannot see - lines wider
 * than 80 columns, frames taller than the screen, and leftovers from a longer
 * previous frame that no clear-to-end-of-line hides.
 *
 *   make render        check the layout
 *   make render-keep   also write build/screen.ansi for inspection
 */

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memtest_ui.h"

#define COLS 80
#define ROWS 24

static int g_failures;

static char g_detail[64];

static void check(bool ok, const char *what, const char *detail)
{
    if (!ok) {
        g_failures++;
        printf("  FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    }
}

/** Same, but appends the offending number to the message. */
static void check_int(bool ok, const char *what, int value)
{
    snprintf(g_detail, sizeof(g_detail), "(%d)", value);
    check(ok, what, g_detail);
}

/* ------------------------------------------------------------------------- */
/* terminal emulator                                                          */

typedef struct {
    char cell[ROWS][COLS + 1];
    int  row;
    int  col;
    int  used_rows;
    bool overflow;   /**< a character was written past column 80 */
    bool scroll;     /**< the frame is taller than the terminal */
} term_t;

static void term_reset(term_t *t)
{
    memset(t, 0, sizeof(*t));
    for (int r = 0; r < ROWS; r++) {
        memset(t->cell[r], ' ', COLS);
        t->cell[r][COLS] = '\0';
    }
}

static void term_frame_start(term_t *t)
{
    t->row = 0;
    t->col = 0;
    t->used_rows = 0;
    t->overflow = false;
    t->scroll = false;
}

static void term_putc(term_t *t, char c)
{
    if (c == '\r') {
        t->col = 0;
        return;
    }
    if (c == '\n') {
        t->col = 0;
        if (++t->row >= ROWS) {
            t->scroll = true;
            t->row = ROWS - 1;
        }
        return;
    }
    if (t->col >= COLS) {
        t->overflow = true;
        t->col = 0;
    }
    t->cell[t->row][t->col] = c;
    if (t->row + 1 > t->used_rows) {
        t->used_rows = t->row + 1;   /* rows that actually carry text */
    }
    t->col++;
}

/** Copy row @p r out of the emulator, trailing spaces removed. */
static void term_row(const term_t *t, int r, char *out, size_t size)
{
    size_t len = COLS;
    while (len > 0 && t->cell[r][len - 1] == ' ') {
        len--;
    }
    if (len >= size) {
        len = size - 1;
    }
    memcpy(out, t->cell[r], len);
    out[len] = '\0';
}

/* ------------------------------------------------------------------------- */
/* realistic state                                                            */

static const mt_ui_banner_t banner = {
    .version        = "1.0.0",
    .chip           = "ESP32-S3",
    .revision       = 0,
    .cpu_mhz        = 240,
    .cores          = 2,
    .idf_version    = "v5.5.5",
    .flash_bytes    = 16u * 1024u * 1024u,
    .psram_present  = true,
    .psram_mallocable = true,
    .psram_size     = 8u * 1024u * 1024u,
    .psram_mode     = "octal",
    .internal_total = 400u * 1024u,
    .seed           = 4919,
    .tests_ok       = true,
};

static mt_ui_region_t regions[] = {
    { .short_name = "psram", .name = "PSRAM (SPI RAM)", .selected = true, .usable = true,
      .total = 8u * 1024u * 1024u, .free_bytes = 7u * 1024u * 1024u, .tested_bytes = 0,
      .blocks = 0, .used_percent = 0 },
    { .short_name = "int", .name = "internal DRAM", .selected = true, .usable = true,
      .total = 400u * 1024u, .free_bytes = 200u * 1024u, .tested_bytes = 0,
      .blocks = 0, .used_percent = 0 },
    { .short_name = "dma", .name = "DMA capable DRAM", .selected = false, .usable = true,
      .total = 300u * 1024u, .free_bytes = 290u * 1024u, .tested_bytes = 0,
      .blocks = 0, .used_percent = 0 },
};

static mt_ui_row_t rows[MT_UI_ROWS];

static mt_ui_frame_t frame = {
    .state = MT_UI_STATE_IDLE,
    .free_heap = 180u * 1024u,
    .min_free_heap = 150u * 1024u,
    .uptime_ms = 4200,
    .interactive = true,
};

static void reset_rows(void)
{
    memset(rows, 0, sizeof(rows));
    memset(regions, 0, sizeof(regions));
    regions[0] = (mt_ui_region_t){ .short_name = "psram", .name = "PSRAM (SPI RAM)",
                                   .selected = true, .usable = true,
                                   .total = 8u * 1024u * 1024u,
                                   .free_bytes = 7u * 1024u * 1024u };
    regions[1] = (mt_ui_region_t){ .short_name = "int", .name = "internal DRAM",
                                   .selected = true, .usable = true,
                                   .total = 400u * 1024u, .free_bytes = 200u * 1024u };
    regions[2] = (mt_ui_region_t){ .short_name = "dma", .name = "DMA capable DRAM",
                                   .usable = true, .total = 300u * 1024u,
                                   .free_bytes = 290u * 1024u };
}

static void row_set(size_t i, uint32_t seq, const char *region, const char *pattern,
                    uint32_t pass, uint32_t total, double seconds, double mbps,
                    uint64_t errors, bool ok, bool complete, bool active)
{
    rows[i] = (mt_ui_row_t){ .seq = seq, .region = region, .pattern = pattern,
                             .passes = pass, .passes_target = total, .seconds = seconds,
                             .mbps = mbps, .errors = errors, .ok = ok,
                             .complete = complete, .active = active };
}

static void emit_states(void)
{
    mt_ui_state_t st = { .banner = &banner, .regions = regions,
                         .region_count = sizeof(regions) / sizeof(regions[0]),
                         .rows = rows, .row_count = MT_UI_ROWS, .frame = &frame };

    /* 1. fresh start: nothing tested yet */
    reset_rows();
    frame.state = MT_UI_STATE_IDLE;
    frame.title = NULL;
    mt_ui_draw(&st);

    /* 2. PSRAM pool allocated, first pattern running */
    regions[0].blocks = 30;
    regions[0].used_percent = 94;
    frame.state = MT_UI_STATE_RUN;
    frame.title = "PSRAM (SPI RAM)";
    frame.test_name = "moving inversions";
    frame.phase = "verify";
    frame.pass = 1;
    frame.passes_total = 0xFFFFFFFFu;
    frame.sub = 2;
    frame.subs_total = 8;
    frame.percent = 4;
    frame.bytes_done = 320u * 1024u;
    frame.bytes_total = 7u * 1024u * 1024u;
    frame.uptime_ms = 91000;
    row_set(0, 1, "psram", "moving inversions", 1, 0xFFFFFFFFu, 128.4, 55.7, 0,
            false, false, true);
    mt_ui_draw(&st);

    /* 3. failures found, second region running */
    row_set(0, 2, "psram", "moving inversions", 1, 0xFFFFFFFFu, 130.1, 55.7, 12,
            false, false, false);
    row_set(1, 3, "psram", "checker", 1, 0xFFFFFFFFu, 4.9, 1440.2, 0, false, false, true);
    regions[0].blocks = 30;
    regions[0].used_percent = 94;
    regions[0].tested_bytes = 7u * 1024u * 1024u;
    regions[1].blocks = 1;
    regions[1].used_percent = 48;
    frame.title = "internal DRAM";
    frame.test_name = "checker (upper/lower 8 bit)";
    frame.phase = "write";
    frame.sub = 1;
    frame.subs_total = 4;
    frame.percent = 12;
    frame.bytes_done = 96u * 1024u;
    frame.bytes_total = 190u * 1024u;
    frame.total_errors = 12;
    frame.uptime_ms = 140000;
    mt_ui_draw(&st);

    /* 4. paused with the help overlay open */
    frame.state = MT_UI_STATE_PAUSED;
    st.show_help = true;
    mt_ui_draw(&st);

    /* 5. finished */
    st.show_help = false;
    frame.state = MT_UI_STATE_DONE;
    frame.test_name = NULL;
    frame.phase = "done";
    frame.percent = 100;
    row_set(1, 4, "psram", "checker", 1, 1, 4.9, 1440.2, 0, true, true, false);
    row_set(2, 5, "int", "address", 1, 1, 0.8, 231.0, 0, true, true, false);
    mt_ui_draw(&st);
}

/* ------------------------------------------------------------------------- */

static void replay(const char *path, bool print_frames)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  FAIL cannot read %s\n", path);
        g_failures++;
        return;
    }

    term_t term;
    char prev[ROWS][COLS + 1];
    int frame_no = 0;
    bool had_frame = false;

    term_reset(&term);
    term_frame_start(&term);
    memset(prev, 0, sizeof(prev));

    int c;
    int esc = 0;            /* 0 = text, 1 = just after ESC, 2 = inside a CSI */
    char csi[16];
    size_t csi_len = 0;
    bool screen_done = false;

    while ((c = fgetc(f)) != EOF && !screen_done) {
        if (esc == 1) {
            esc = (c == '[') ? 2 : 0;
            csi_len = 0;
            continue;
        }
        if (esc == 2) {
            if (c >= 0x30 && c <= 0x3f) {      /* parameter byte */
                if (csi_len + 1u < sizeof(csi)) {
                    csi[csi_len++] = (char)c;
                }
                continue;
            }
            if (c >= 0x40 && c <= 0x7e) {      /* final byte of the sequence */
                esc = 0;
                if (c == 'l' && csi_len == 5u && memcmp(csi, "?1049", 5) == 0) {
                    screen_done = true;        /* left the alternate screen */
                    break;
                }
                if (c == 'H') {
                    if (had_frame && term.used_rows > 0) {
                        if (print_frames) {
                            printf("--- frame %d ---\n", frame_no - 1);
                            for (int r = 0; r < term.used_rows; r++) {
                                char line[COLS + 1];
                                term_row(&term, r, line, sizeof(line));
                                printf("|%s|\n", line);
                            }
                        }
                        check(!term.overflow, "line longer than 80 columns", NULL);
                        check(!term.scroll, "frame scrolls the terminal", NULL);
                        check_int(term.used_rows == ROWS - 1,
                                  "frame does not fill the screen", term.used_rows);
                        for (int r = 0; r < term.used_rows; r++) {
                            char line[COLS + 1];
                            term_row(&term, r, line, sizeof(line));
                            if (had_frame && strcmp(prev[r], line) != 0 &&
                                strncmp(prev[r], line, strlen(line)) == 0) {
                                printf("  WARN row %d shrank, stale text may remain"
                                       " ('%s' -> '%s')\n", r + 1, prev[r], line);
                            }
                            snprintf(prev[r], COLS + 1, "%s", line);
                        }
                    }
                    frame_no++;
                    had_frame = true;
                    term_frame_start(&term);
                }
            }
            continue;
        }
        if (c == 0x1b) {
            esc = 1;
            continue;
        }
        term_putc(&term, (char)c);
    }
    /* the last frame is only complete at end of stream */
    if (had_frame && term.used_rows > 0) {
        check(!term.overflow, "line longer than 80 columns", NULL);
        check(!term.scroll, "frame taller than the terminal", NULL);
        check_int(term.used_rows == ROWS - 1, "frame does not fill the screen",
                  term.used_rows);
        if (print_frames) {
            printf("--- frame %d ---\n", frame_no - 1);
            for (int r = 0; r < term.used_rows; r++) {
                char line[COLS + 1];
                term_row(&term, r, line, sizeof(line));
                printf("|%s|\n", line);
            }
        }
    }
    fclose(f);

    check(frame_no >= 5, "expected at least five frames", NULL);
    printf("  %d frames replayed, %d problem(s)\n", frame_no, g_failures);
}

int main(int argc, char **argv)
{
    bool print_frames = (argc > 1 && strcmp(argv[1], "--print") == 0);
    const char *path = "build/screen.ansi";

    /* The renderer writes to stdout; borrow the real one so that the report
     * below does not end up in the capture file. */
    int saved_stdout = dup(fileno(stdout));
    if (!freopen(path, "w", stdout)) {
        printf("  FAIL cannot write %s\n", path);
        return 1;
    }
    mt_ui_set_interactive(true);
    mt_ui_begin(MT_UI_ANSI);
    emit_states();
    mt_ui_end();                       /* leaves the alternate screen first */
    mt_ui_rule();
    mt_ui_raw("report text that is not part of the screen");
    mt_ui_rule();
    fflush(stdout);
    dup2(saved_stdout, fileno(stdout));
    close(saved_stdout);

    replay(path, print_frames);

    if (g_failures) {
        printf("render: %d problem(s)\n", g_failures);
        return 1;
    }
    printf("render: ok\n");
    return 0;
}