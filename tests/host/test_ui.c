/*
 * esp32-memtest - host tests for the terminal renderer.
 *
 * SPDX-License-Identifier: MIT
 *
 * A serial screen renderer has one failure mode that is invisible in code
 * review and obvious on the device: a row that grows past 80 columns wraps
 * and destroys the frame.  These tests render representative screens and
 * check every emitted line, with the ANSI escape sequences stripped, exactly
 * like a terminal would see it.
 *
 *   make -C tests/host ui
 */

#define MT_UI_FORCE_ANSI 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memtest_ui.h"

static FILE *g_report;   /* real stdout, stdout itself is captured */
static int g_failures;
static int g_checks;
static char g_screen[16384];
static size_t g_screen_len;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(g_report ? g_report : stdout, "  FAIL %s:%d: ", __func__,  \
                    __LINE__);                                                \
            fprintf(g_report ? g_report : stdout, __VA_ARGS__);                \
            fprintf(g_report ? g_report : stdout, "\n");                       \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------------- */

static void build_state(mt_ui_state_t *st, mt_ui_banner_t *banner,
                        mt_ui_region_t regions[2], mt_ui_row_t rows[MT_UI_ROWS],
                        mt_ui_frame_t *frame)
{
    memset(st, 0, sizeof(*st));

    memset(banner, 0, sizeof(*banner));
    banner->version = "1.0.0";
    banner->chip = "ESP32-S3";
    banner->revision = 0;
    banner->cpu_mhz = 240;
    banner->cores = 2;
    banner->idf_version = "v5.5.5";
    banner->flash_bytes = 16u << 20;
    banner->psram_present = true;
    banner->psram_mallocable = true;
    banner->psram_size = 8u << 20;
    banner->psram_mode = "hex";
    banner->internal_total = 512u * 1024u;

    memset(regions, 0, sizeof(mt_ui_region_t) * 2);
    regions[0].short_name = "PSRAM";
    regions[0].selected = true;
    regions[0].usable = true;
    regions[0].total = 8u << 20;
    regions[0].free_bytes = (7u << 20) + 512u * 1024u;
    regions[0].tested_bytes = (7u << 20) + 512u * 1024u;
    regions[0].blocks = 2;
    regions[0].used_percent = 94;
    regions[1].short_name = "DRAM";
    regions[1].selected = true;
    regions[1].usable = true;
    regions[1].total = 512u * 1024u;
    regions[1].free_bytes = 402u * 1024u;
    regions[1].tested_bytes = 256u * 1024u;
    regions[1].blocks = 1;
    regions[1].used_percent = 55;

    memset(rows, 0, sizeof(mt_ui_row_t) * MT_UI_ROWS);
    for (size_t i = 0; i < MT_UI_ROWS; i++) {
        rows[i].region = "PSRAM";
        rows[i].pattern = "Address test";
        rows[i].passes = 1;
        rows[i].passes_target = 1;
        rows[i].seconds = 1.25;
        rows[i].mbps = 121.5;
        rows[i].ok = true;
        rows[i].complete = true;
        rows[i].seq = (uint32_t)i + 1u;
    }
    rows[2].pattern = "Modulo X";
    rows[2].ok = false;
    rows[2].errors = 4;
    rows[2].flaky = 2;
    rows[3].pattern = "Pseudo random";
    rows[3].active = true;
    rows[3].passes = 0;
    rows[3].passes_target = 0;      /* infinite */
    rows[4].complete = false;
    rows[5].passes = 12345;
    rows[5].passes_target = 0;
    rows[6].pattern = "A very long pattern name that should be truncated cleanly";
    rows[7].seconds = 99999.99;
    rows[7].mbps = 12345.6;

    memset(frame, 0, sizeof(*frame));
    frame->title = "PSRAM";
    frame->test_name = "Address test";
    frame->phase = "verify";
    frame->state = MT_UI_STATE_RUN;
    frame->pass = 2;
    frame->passes_total = 3;
    frame->sub = 7;
    frame->subs_total = 64;
    frame->percent = 67;
    frame->bytes_done = 5u << 20;
    frame->bytes_total = 7u << 20;
    frame->free_heap = 402u * 1024u;
    frame->min_free_heap = 380u * 1024u;
    frame->uptime_ms = 123456;
    frame->total_errors = 4;
    frame->interactive = true;

    st->banner = banner;
    st->regions = regions;
    st->region_count = 2;
    st->rows = rows;
    st->row_count = MT_UI_ROWS;
    st->frame = frame;
    st->show_help = false;
}

/** Strip ANSI sequences so the length check matches what the terminal shows. */
static size_t visible_len(const char *line)
{
    size_t len = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '\x1b') {
            while (*p && *p != 'm') {
                p++;
            }
            if (!*p) {
                break;
            }
            continue;
        }
        if (*p == '\r' || *p == '\n') {
            continue;
        }
        len++;
    }
    return len;
}

static void check_lines(const char *label)
{
    const char *p = g_screen;
    int lines = 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        char line[512];
        size_t len;
        if (eol == NULL) {
            break;   /* trailing fragment (teardown escape codes), not a row */
        }
        len = (size_t)(eol - p);
        if (len >= sizeof(line)) {
            CHECK(false, "line %d is absurdly long", lines);
            return;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        CHECK(visible_len(line) <= MT_UI_WIDTH + 2, "%s line %d is %zu columns wide: %s", label,
              lines, visible_len(line), line);
        CHECK(visible_len(line) == MT_UI_WIDTH + 2, "%s line %d is %zu columns, expected %d: %s",
              label, lines, visible_len(line), MT_UI_WIDTH + 2, line);
        lines++;
        p = eol + 1;
    }
    CHECK(lines > 20, "%s: suspiciously short screen (%d lines)", label, lines);
    CHECK(lines <= 32, "%s: screen is taller than a standard terminal (%d lines)", label, lines);
}

/* ------------------------------------------------------------------------- */

/* The renderer writes to stdout, so redirect it into a file and read the
 * result back.  The real stdout is kept on its own stream, because
 * /dev/stdout would resolve to the capture file itself. */
#define CAPTURE_PATH "/tmp/esp32-memtest-ui.txt"

static void reset_capture(void)
{
    g_screen_len = 0;
    g_screen[0] = '\0';
}

static void start_capture(void)
{
    fflush(stdout);
    if (g_report == NULL) {
        int fd = dup(STDOUT_FILENO);
        if (fd >= 0) {
            g_report = fdopen(fd, "w");
        }
        if (g_report == NULL) {
            g_report = stderr;
        }
    }
    reset_capture();
    if (freopen(CAPTURE_PATH, "w", stdout) == NULL) {
        fprintf(stderr, "cannot redirect stdout\n");
        exit(1);
    }
}

static void stop_capture(void)
{
    FILE *f;
    fflush(stdout);
    f = fopen(CAPTURE_PATH, "rb");
    if (f) {
        g_screen_len = fread(g_screen, 1, sizeof(g_screen) - 1, f);
        fclose(f);
    }
    g_screen[g_screen_len] = '\0';
}

static void show_capture(void)
{
    if (g_report) {
        fwrite(g_screen, 1, g_screen_len, g_report);
        fflush(g_report);
    }
}

int main(void)
{
    mt_ui_banner_t banner;
    mt_ui_region_t regions[2];
    mt_ui_row_t rows[MT_UI_ROWS];
    mt_ui_frame_t frame;
    mt_ui_state_t st;

    printf("esp32-memtest UI layout tests\n");
    printf("============================\n");
    CHECK(MT_UI_WIDTH + 2 == 80, "the screen must be exactly 80 columns wide, is %d",
          MT_UI_WIDTH + 2);

    build_state(&st, &banner, regions, rows, &frame);

    /* --- frame without the help overlay ------------------------------- */
    start_capture();
    mt_ui_begin(MT_UI_ANSI);
    mt_ui_log("allocated 7.50 MiB of PSRAM in 2 blocks");
    mt_ui_draw(&st);
    stop_capture();
    show_capture();
    check_lines("frame");
    CHECK(mt_ui_mode() == MT_UI_ANSI, "ANSI mode not selected");

    /* --- frame with the help overlay ----------------------------------- */
    reset_capture();
    start_capture();
    mt_ui_help_visible(true);
    st.show_help = true;
    mt_ui_draw(&st);
    stop_capture();
    show_capture();
    check_lines("help");

    mt_ui_end();

    fprintf(g_report ? g_report : stdout, "%d checks, %d failures\n", g_checks, g_failures);
    fflush(g_report ? g_report : stdout);
    return g_failures == 0 ? 0 : 1;
}