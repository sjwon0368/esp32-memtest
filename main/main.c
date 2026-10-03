/*
 * esp32-memtest - serial memory tester for Espressif MCUs.
 *
 * SPDX-License-Identifier: MIT
 *
 * Application logic: banner, warnings, menus, the run loop, the final report.
 * Everything test related lives in components/memtest_core, everything chip
 * related in components/memtest_esp and everything screen related in
 * components/memtest_ui.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "sdkconfig.h"

/* Matches the guard in memtest_esp: the chip must support PSRAM *and* this
 * build must enable it, otherwise IDF never compiles esp_psram_init(). */
#if defined(CONFIG_SPIRAM) && defined(SOC_SPIRAM_SUPPORTED)
#include "esp_psram.h"
#define MEMTEST_HAVE_PSRAM_API 1
#endif

/* A Kconfig "bool n" leaves the macro undefined, so every switch the code
 * reads gets a fallback here. */
#ifndef CONFIG_MEMTEST_AUTORUN
#define CONFIG_MEMTEST_AUTORUN 1
#endif
#ifndef CONFIG_MEMTEST_SELFTEST
#define CONFIG_MEMTEST_SELFTEST 1
#endif
#ifndef CONFIG_MEMTEST_KEYS
#define CONFIG_MEMTEST_KEYS 1
#endif
#ifndef CONFIG_MEMTEST_UI_ANSI
#define CONFIG_MEMTEST_UI_ANSI 1
#endif
#ifndef CONFIG_MEMTEST_REBOOT
#define CONFIG_MEMTEST_REBOOT 0
#endif
#ifndef CONFIG_MEMTEST_REBOOT_DELAY_MS
#define CONFIG_MEMTEST_REBOOT_DELAY_MS 5000
#endif
#ifndef CONFIG_MEMTEST_VERBOSE
#define CONFIG_MEMTEST_VERBOSE 0
#endif

#include "memtest_core.h"
#include "memtest_esp.h"
#include "memtest_ui.h"

#ifndef MEMTEST_VERSION
#define MEMTEST_VERSION "0.0.0"
#endif

#define MB   (1024u * 1024u)
#define HISTORY 48          /* result rows kept for scrolling */
#define REGION_NAME_MAX 12

/* ------------------------------------------------------------------------- */
/* Runtime state                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    char          region[REGION_NAME_MAX];
    char          pattern[24];
    mt_result_t   result;
    uint32_t      seq;
    bool          active;
} history_row_t;

typedef struct {
    volatile bool quit;
    volatile bool pause;
    volatile bool skip;
    volatile bool restart;
    bool          help;
} app_control_t;

static mt_esp_region_t   s_regions[MT_ESP_MAX_REGIONS];
static int               s_region_count;
static bool              s_region_selected[MT_ESP_MAX_REGIONS];

static mt_testset_spec_t s_testset;
static uint32_t          s_seed         = CONFIG_MEMTEST_SEED;
static uint32_t          s_passes       = CONFIG_MEMTEST_PASSES;
static app_control_t    s_ctl;

static history_row_t     s_history[HISTORY];
static size_t            s_history_count;
static size_t            s_history_first;   /* index of the oldest row */
static uint32_t          s_seq;

static mt_ui_row_t       s_rows[MT_UI_ROWS];
static mt_ui_region_t    s_ui_regions[MT_ESP_MAX_REGIONS];
static size_t            s_ui_region_count;

static mt_ui_banner_t    s_banner;
static mt_ui_frame_t     s_frame;
static uint64_t          s_boot_ms;

/* Per region: the pool that was tested, kept so that the report can print
 * the block map after the memory itself has already been released. */
static mt_pool_t         g_pools[MT_ESP_MAX_REGIONS];
static size_t            g_tested_bytes[MT_ESP_MAX_REGIONS];
static uint32_t          g_tested_blocks[MT_ESP_MAX_REGIONS];

/* ------------------------------------------------------------------------- */

/** True when the region can be served from internal RAM, i.e. the reserve
 *  policy (keep the runtime alive) has to apply to it. */
static bool region_uses_internal(const mt_esp_region_t *r)
{
    return strcmp(r->key, "internal") == 0 || strcmp(r->key, "dma") == 0 ||
           strcmp(r->key, "any") == 0;
}

/* ------------------------------------------------------------------------- */
/* Configuration helpers                                                     */
/* ------------------------------------------------------------------------- */

static void parse_region_selection(const char *spec)
{
    char buf[MT_MAX_SPEC_LEN];
    char *token;
    char *save = NULL;

    for (int i = 0; i < s_region_count; i++) {
        s_region_selected[i] = false;
    }
    snprintf(buf, sizeof(buf), "%s", spec ? spec : "");
    for (token = strtok_r(buf, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
        while (*token == ' ') {
            token++;
        }
        int idx = mt_esp_region_index(token);
        if (idx >= 0 && idx < s_region_count) {
            s_region_selected[idx] = true;
        } else if (*token) {
            mt_ui_log("unknown region '%s' ignored", token);
        }
    }
}

static bool load_testset(const char *spec)
{
    int n = mt_testset_parse(spec, s_seed, &s_testset);
    if (n <= 0) {
        mt_ui_log("no usable tests in '%s'", spec ? spec : "");
        return false;
    }
    for (int i = 0; i < n; i++) {
        s_testset.tests[i].passes = s_passes;
    }
    return true;
}

static size_t target_bytes_for(const mt_esp_region_t *region)
{
    size_t target = CONFIG_MEMTEST_TEST_SIZE_MB ? (size_t)CONFIG_MEMTEST_TEST_SIZE_MB * MB : 0;

    if (target == 0) {
        target = region->free_bytes;
        if (target == 0) {
            target = region->total;
        }
    }
    if (CONFIG_MEMTEST_MAX_TEST_MB) {
        size_t cap = (size_t)CONFIG_MEMTEST_MAX_TEST_MB * MB;
        if (target > cap) {
            target = cap;
        }
    }
    return target;
}

static size_t block_bytes_for(const mt_esp_region_t *region, size_t target)
{
    size_t block = (size_t)CONFIG_MEMTEST_BLOCK_KB * 1024u;
    if (region->largest_block && region->largest_block < block) {
        block = region->largest_block;
    }
    if (target && target < block) {
        block = target;
    }
    block &= ~(size_t)0x3Fu;   /* keep the size a multiple of 64 bytes */
    return block ? block : 4096u;
}

/* ------------------------------------------------------------------------- */
/* History / result rows                                                     */
/* ------------------------------------------------------------------------- */

static history_row_t *history_push(void)
{
    history_row_t *row;
    if (s_history_count < HISTORY) {
        row = &s_history[s_history_first + s_history_count];
        s_history_count++;
    } else {
        /* Full: overwrite the oldest row and slide the window. s_history_count
         * must stay clamped at HISTORY - letting it grow would make
         * history_at() wrap and hand back the same rows twice. */
        row = &s_history[s_history_first];
        s_history_first = (s_history_first + 1u) % HISTORY;
    }
    memset(row, 0, sizeof(*row));
    row->seq = ++s_seq;
    return row;
}

static const history_row_t *history_at(size_t index)
{
    if (index >= s_history_count) {
        return NULL;
    }
    return &s_history[(s_history_first + index) % HISTORY];
}

/** Fill s_rows from the history, newest last, honouring the scroll offset. */
static void refresh_rows(size_t *scroll)
{
    size_t visible;
    if (s_history_count <= MT_UI_ROWS) {
        visible = s_history_count;
        *scroll = 0;
    } else if (*scroll + MT_UI_ROWS < s_history_count) {
        visible = *scroll + MT_UI_ROWS;
    } else {
        /* Past the end: show the newest rows. The offset is written back
         * through the pointer so the view stays where it was put instead of
         * snapping back on the next redraw. */
        visible = s_history_count;
        *scroll = s_history_count - MT_UI_ROWS;
    }

    for (size_t i = 0; i < MT_UI_ROWS; i++) {
        memset(&s_rows[i], 0, sizeof(s_rows[i]));
        if (i >= visible) {
            continue;
        }
        const history_row_t *h = history_at(*scroll + i);
        if (h == NULL) {
            continue;
        }
        s_rows[i].seq = h->seq;
        s_rows[i].region = h->region;
        s_rows[i].pattern = h->pattern;
        s_rows[i].passes = h->result.passes;
        s_rows[i].passes_target = h->result.passes_target;
        s_rows[i].seconds = h->result.seconds;
        s_rows[i].mbps = h->result.mbps;
        s_rows[i].errors = h->result.cells_failed;
        s_rows[i].flaky = h->result.cells_flaky;
        s_rows[i].ok = h->result.cells_failed == 0;
        s_rows[i].complete = h->result.complete;
        s_rows[i].active = h->active;
    }
}

static void refresh_regions(void)
{
    size_t n = 0;
    for (int i = 0; i < s_region_count; i++) {
        const mt_esp_region_t *r = &s_regions[i];
        size_t tested;
        uint32_t percent;

        if (!s_region_selected[i]) {
            continue;
        }
        tested = g_tested_bytes[i];
        percent = r->total ? (uint32_t)((tested * 100u) / r->total) : 0u;

        s_ui_regions[n].short_name = r->short_name;
        s_ui_regions[n].name = r->name;
        s_ui_regions[n].selected = true;
        s_ui_regions[n].usable = r->mallocable;
        s_ui_regions[n].total = r->total;
        s_ui_regions[n].free_bytes = r->free_bytes;
        s_ui_regions[n].tested_bytes = tested;
        s_ui_regions[n].blocks = g_tested_blocks[i];
        s_ui_regions[n].used_percent = percent > 100u ? 100u : percent;
        n++;
    }
    s_ui_region_count = n;
}

static uint64_t total_errors(void)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < s_history_count; i++) {
        sum += history_at(i)->result.cells_failed;
    }
    return sum;
}

/* ------------------------------------------------------------------------- */
/* Screen composition                                                        */
/* ------------------------------------------------------------------------- */

static void draw(size_t scroll)
{
    mt_ui_state_t state;
    size_t offset = scroll;
    refresh_rows(&offset);
    refresh_regions();

    memset(&state, 0, sizeof(state));
    state.banner = &s_banner;
    state.regions = s_ui_regions;
    state.region_count = s_ui_region_count;
    state.rows = s_rows;
    state.row_count = MT_UI_ROWS;
    state.frame = &s_frame;
    state.show_help = s_ctl.help;
    mt_ui_draw(&state);
}

/* ------------------------------------------------------------------------- */
/* Keyboard                                                                 */
/* ------------------------------------------------------------------------- */

static void handle_key(int key, size_t *scroll)
{
    if (!CONFIG_MEMTEST_KEYS || !mt_ui_interactive()) {
        return;
    }
    switch (key) {
    case 'p':
        s_ctl.pause = !s_ctl.pause;
        mt_ui_log(s_ctl.pause ? "paused" : "resumed");
        break;
    case 's':
    case 'n':
        s_ctl.skip = true;
        mt_ui_log("skipping the current test");
        break;
    case 'q':
        s_ctl.quit = true;
        s_ctl.pause = false;
        s_ctl.skip = true;
        mt_ui_log("stopping after the current step");
        break;
    case 'r':
        s_ctl.restart = true;
        s_ctl.quit = true;
        s_ctl.skip = true;
        s_ctl.pause = false;
        mt_ui_log("restarting the suite");
        break;
    case 'h':
        s_ctl.help = !s_ctl.help;
        break;
    case 'u':
        mt_ui_log("output mode: %s",
                  mt_ui_toggle_mode() == MT_UI_ANSI ? "ANSI screen" : "plain text");
        break;
    case MT_KEY_UP:
        if (*scroll > 0) {
            (*scroll)--;
        }
        break;
    case MT_KEY_DOWN:
        *scroll += 1u;
        break;
    default:
        break;
    }
}

static void poll_keys(size_t *scroll)
{
    int key;
    while ((key = mt_esp_poll_key()) >= 0) {
        handle_key(key, scroll);
    }
}

static bool paused_wait(size_t *scroll)
{
    while (s_ctl.pause) {
        poll_keys(scroll);
        draw(*scroll);
        mt_esp_delay_ms(100);
    }
    return !s_ctl.quit;
}

/* ------------------------------------------------------------------------- */
/* Engine callback                                                          */
/* ------------------------------------------------------------------------- */

static uint64_t s_last_draw_ms;

static bool due_for_draw(void)
{
    uint64_t now = mt_esp_time_ms();
    if (now - s_last_draw_ms < CONFIG_MEMTEST_REFRESH_MS) {
        return false;
    }
    s_last_draw_ms = now;
    return true;
}

static void on_progress(const mt_progress_t *p, void *user)
{
    size_t *scroll = user;

    s_frame.state = s_ctl.pause ? MT_UI_STATE_PAUSED : MT_UI_STATE_RUN;
    s_frame.phase = p->finished ? "done" : p->state;
    s_frame.pass = p->pass;
    s_frame.passes_total = p->passes_total == 0xFFFFFFFFu ? 0u : p->passes_total;
    s_frame.sub = p->sub;
    s_frame.subs_total = p->subs_total;
    s_frame.percent = p->percent;
    s_frame.bytes_done = (size_t)p->bytes_done;
    s_frame.bytes_total = (size_t)p->bytes_total;
    s_frame.free_heap = mt_esp_free_heap();
    s_frame.min_free_heap = mt_esp_min_free_heap();
    s_frame.uptime_ms = mt_esp_time_ms() - s_boot_ms;
    s_frame.total_errors = total_errors();
    s_frame.interactive = CONFIG_MEMTEST_KEYS;

    poll_keys(scroll);
    if (s_ctl.skip) {
        s_ctl.skip = false;
    }
    if (!paused_wait(scroll)) {
        return;
    }
    if (due_for_draw()) {
        draw(*scroll);
    }
}

/* ------------------------------------------------------------------------- */
/* Self test                                                                */
/* ------------------------------------------------------------------------- */

static bool engine_selftest(void)
{
    const size_t size = 64 * 1024;
    mt_test_spec_t spec = { .pattern = MT_PAT_ADDRESS, .seed = s_seed, .passes = 1 };
    mt_result_t result;
    mt_stop_reason_t stop = MT_STOP_NONE;
    mt_pool_t pool;
    mt_pool_request_t req = { .block_bytes = size, .target_bytes = size, .min_block = 4096,
                                 .caps = MALLOC_CAP_DEFAULT };
    size_t reserve = mt_esp_get_reserve();
    bool ok = false;

    mt_esp_set_reserve(0);   /* the self test may use the whole free heap */
    if (!mt_pool_acquire(&pool, &req, mt_esp_alloc, NULL)) {
        mt_ui_log("self test could not allocate 64 KiB - skipping");
        mt_esp_set_reserve(reserve);
        return true;
    }

    mt_run_test(&pool, &spec, s_seed, mt_esp_time_us, NULL, NULL, NULL, NULL, &result, &stop);
    ok = (result.cells_failed == 0) && result.complete;
    if (ok) {
        mt_ui_log("self test passed (%u cells, %.1f MB/s)", (unsigned)result.cells, result.mbps);
    } else {
        mt_ui_log("SELF TEST FAILED: %llu bad cells - the tester or the chip is at fault,"
                  " not the memory module", (unsigned long long)result.cells_failed);
    }
    mt_pool_release(&pool, mt_esp_free, NULL);
    mt_esp_set_reserve(reserve);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Menus                                                                    */
/* ------------------------------------------------------------------------- */

static void menu_wait_key(int *key_out)
{
    *key_out = -1;
    for (;;) {
        *key_out = mt_esp_poll_key();
        if (*key_out >= 0) {
            return;
        }
        mt_esp_delay_ms(50);
    }
}

static void menu_region_select(void)
{
    for (;;) {
        mt_ui_rule();
        mt_ui_raw(" memory regions");
        for (int i = 0; i < s_region_count; i++) {
            char size[24];
            mt_fmt_bytes(size, sizeof(size), s_regions[i].total);
            mt_ui_raw("  [%c] %d %-8s %-18s %10s  %s", s_region_selected[i] ? 'x' : ' ',
                      i + 1, s_regions[i].short_name, s_regions[i].name, size,
                      s_regions[i].note);
        }
        mt_ui_raw("  [a] toggle all      [d] PSRAM only    [i] internal only");
        mt_ui_raw("  [b] back");
        mt_ui_raw("> ");
        int key = '?';
        menu_wait_key(&key);
        if (key == 'b' || key == 'q' || key == 27) {
            return;
        }
        if (key >= '1' && key < '1' + s_region_count) {
            int idx = key - '1';
            s_region_selected[idx] = !s_region_selected[idx];
        } else if (key == 'a') {
            bool all = true;
            for (int i = 0; i < s_region_count; i++) {
                all = all && s_region_selected[i];
            }
            for (int i = 0; i < s_region_count; i++) {
                s_region_selected[i] = !all;
            }
        } else if (key == 'd') {
            int idx = mt_esp_region_index("psram");
            for (int i = 0; i < s_region_count; i++) {
                s_region_selected[i] = (i == idx);
            }
        } else if (key == 'i') {
            int idx = mt_esp_region_index("internal");
            for (int i = 0; i < s_region_count; i++) {
                s_region_selected[i] = (i == idx);
            }
        }
    }
}

static void menu_test_select(void)
{
    for (;;) {
        char names[MT_UI_WIDTH];
        size_t len = 0;
        mt_ui_rule();
        mt_ui_raw(" test patterns (%u selected)", s_testset.count);
        for (int p = 0; p < MT_PATTERN_COUNT; p++) {
            bool selected = false;
            for (size_t i = 0; i < s_testset.count; i++) {
                if (s_testset.tests[i].pattern == (mt_pattern_t)p) {
                    selected = true;
                }
            }
            const mt_pattern_desc_t *d = mt_pattern_desc((mt_pattern_t)p);
            len += (size_t)snprintf(names + len, sizeof(names) - len, "%c%s",
                                    selected ? '+' : ' ', d->key);
        }
        mt_ui_raw("  %s", names);
        mt_ui_raw("  presets: quick, standard, extended, full, random, walking");
        mt_ui_raw("  [s] preset   [0] none   [b] back");
        mt_ui_raw("> ");
        int key = '?';
        menu_wait_key(&key);
        if (key == 'b' || key == 'q' || key == 27) {
            return;
        }
        if (key == '0') {
            s_testset.count = 0;
        } else if (key == 's') {
            mt_ui_raw(" preset? ");
            char line[32];
            int pos = 0;
            line[0] = '\0';
            for (;;) {
                int c = mt_esp_poll_key();
                if (c < 0) {
                    mt_esp_delay_ms(30);
                    continue;
                }
                if (c == '\r' || c == '\n') {
                    break;
                }
                if ((c == 127 || c == 8) && pos > 0) {
                    line[--pos] = '\0';
                } else if (c >= 32 && pos + 1u < sizeof(line)) {
                    line[pos++] = (char)c;
                    line[pos] = '\0';
                }
            }
            if (mt_testset_parse(line, s_seed, &s_testset) <= 0) {
                mt_ui_raw(" unknown preset '%s'", line);
                load_testset("standard");
            } else {
                for (int i = 0; i < s_testset.count; i++) {
                    s_testset.tests[i].passes = s_passes;
                }
            }
        }
    }
}

static bool main_menu(void)
{
    for (;;) {
        char ps[24];
        char sel[64] = "";
        for (int i = 0; i < s_region_count; i++) {
            if (s_region_selected[i]) {
                size_t l = strlen(sel);
                snprintf(sel + l, sizeof(sel) - l, "%s%s", l ? " " : "", s_regions[i].short_name);
            }
        }
        if (sel[0] == '\0') {
            snprintf(sel, sizeof(sel), "%s", "none selected");
        }
        mt_fmt_bytes(ps, sizeof(ps), mt_esp_psram_size());

        mt_ui_rule();
        mt_ui_raw(" esp32-memtest %s - main menu", MEMTEST_VERSION);
        mt_ui_rule();
        mt_ui_raw(" chip       %s rev%d at %d MHz, %d cores", mt_esp_chip_name(),
                  mt_esp_chip_revision(), mt_esp_cpu_mhz(), mt_esp_cores());
        mt_ui_raw(" psram      %s", mt_esp_psram_present() ? ps : "not present");
        mt_ui_raw(" regions    %s", sel);
        mt_ui_raw(" patterns   %u selected, %u pass(es), seed 0x%04lx",
                  (unsigned)s_testset.count, (unsigned)s_passes, (unsigned long)s_seed);
        mt_ui_raw(" output     %s", mt_ui_mode() == MT_UI_ANSI ? "ANSI screen" : "plain text");
        mt_ui_rule();
        mt_ui_raw(" [s] start tests   [r] regions   [t] patterns");
        mt_ui_raw(" [p] passes (%u)    [u] output mode [q] quit", (unsigned)s_passes);
        mt_ui_raw("> ");
        int key = '?';
        menu_wait_key(&key);
        switch (key) {
        case 's':
            return true;
        case 'r':
            menu_region_select();
            break;
        case 't':
            menu_test_select();
            break;
        case 'p':
            s_passes = (s_passes >= 10u) ? 1u : s_passes + 1u;
            for (int i = 0; i < s_testset.count; i++) {
                s_testset.tests[i].passes = s_passes;
            }
            break;
        case 'u':
            mt_ui_toggle_mode();
            break;
        case 'q':
            return false;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Report                                                                   */
/* ------------------------------------------------------------------------- */

static void print_block_map(const mt_pool_t *pool, const mt_esp_region_t *region)
{
    for (size_t i = 0; i < pool->count; i++) {
        char size[24];
        mt_fmt_bytes(size, sizeof(size), pool->blocks[i].size);
        mt_ui_raw("     block %-2u  0x%08lx - 0x%08lx  %10s", (unsigned)i,
                  (unsigned long)(uintptr_t)pool->blocks[i].addr,
                  (unsigned long)(uintptr_t)(pool->blocks[i].addr + pool->blocks[i].size - 1u),
                  size);
    }
    (void)region;
}

static void print_errors(const mt_result_t *result, const mt_pool_t *pool)
{
    mt_ui_raw("     first failing cells:");
    mt_ui_raw("       %-3s %-10s %-10s %-10s %-10s %-5s %-5s %-12s", "#", "address", "offset",
              "expected", "actual", "xor", "bits", "block");
    for (size_t i = 0; i < result->error_count; i++) {
        const mt_error_t *e = &result->errors[i];
        const mt_block_t *blk = (e->block_id < pool->count) ? &pool->blocks[e->block_id] : NULL;
        char hex_e[16], hex_a[16], hex_x[16], block[24], block_id[12];
        unsigned xor_bits = 0;
        for (uint32_t v = e->xor_diff; v != 0; v >>= 1) {
            xor_bits += (unsigned)(v & 1u);
        }
        mt_fmt_hex(hex_e, sizeof(hex_e), e->expected);
        mt_fmt_hex(hex_a, sizeof(hex_a), e->actual);
        mt_fmt_hex(hex_x, sizeof(hex_x), e->xor_diff);
        if (blk) {
            mt_fmt_bytes(block, sizeof(block), blk->size);
            snprintf(block_id, sizeof(block_id), "%u", (unsigned)e->block_id);
        } else {
            snprintf(block, sizeof(block), "%s", "-");
            snprintf(block_id, sizeof(block_id), "%s", "-");
        }
        mt_ui_raw("       %-3u 0x%08lx +0x%06lx %s %s %s %-5u %-5u %-4s%-10s%s",
                  (unsigned)(i + 1u),
                  (unsigned long)e->addr, (unsigned long)e->offset, hex_e, hex_a, hex_x,
                  xor_bits, (unsigned)e->bits_wrong, block_id, block,
                  e->persistent ? "" : " transient");
    }
}

static void print_hint(const mt_result_t *result)
{
    bool edge_only = true;
    bool transient = false;

    for (size_t i = 0; i < result->error_count; i++) {
        edge_only = edge_only && result->errors[i].on_block_edge;
        transient = transient || !result->errors[i].persistent;
    }
    if (result->error_count == 0) {
        return;
    }
    if (transient) {
        mt_ui_raw("     hint: some cells failed once and read back correctly afterwards.");
        mt_ui_raw("           That points at a marginal cell - power, temperature or timing -");
        mt_ui_raw("           not at a hard failure. Repeat the run when the board is cold.");
    }
    if (edge_only) {
        mt_ui_raw("     hint: every failure sits at the very start or end of a block.");
        mt_ui_raw("           Check that the heap did not hand out memory which something else");
        mt_ui_raw("           still owns, and lower CONFIG_MEMTEST_RESERVE_KB if needed.");
    }
}

/* Roll up every history row that belongs to a selected region. Shared by the
 * one-line verdict under the banner and by the full report, so the two can
 * never disagree about whether the run passed. */
static void collect_totals(unsigned *tests, uint64_t *cells, uint64_t *errors, double *seconds,
                           bool *passed)
{
    *tests = 0;
    *cells = 0;
    *errors = 0;
    *seconds = 0.0;
    *passed = true;

    for (int i = 0; i < s_region_count; i++) {
        if (!s_region_selected[i]) {
            continue;
        }
        for (size_t index = 0; index < s_history_count; index++) {
            const history_row_t *h = history_at(index);
            if (strcmp(h->region, s_regions[i].short_name) != 0) {
                continue;
            }
            *cells += h->result.cells;
            *errors += h->result.cells_failed;
            *seconds += h->result.seconds;
            (*tests)++;
            *passed = *passed && (h->result.cells_failed == 0);
        }
    }
}

static void print_verdict_line(void)
{
    unsigned tests;
    uint64_t cells, errors;
    double seconds;
    bool passed;
    char verified[24];

    collect_totals(&tests, &cells, &errors, &seconds, &passed);
    mt_fmt_bytes(verified, sizeof(verified), (size_t)(cells * 4u));
    mt_ui_raw(" RESULT     %s - %u test(s), %s verified in %.2f s, %llu error(s)",
              passed ? "PASS" : "FAIL", tests, verified, seconds,
              (unsigned long long)errors);
}

static void print_report(void)
{
    uint64_t cells = 0;
    uint64_t errors = 0;
    double seconds = 0.0;
    size_t tests = 0;
    bool passed = true;

    mt_ui_rule();
    mt_ui_raw(" esp32-memtest %s - report", MEMTEST_VERSION);
    mt_ui_rule();
    mt_ui_raw(" chip        %s rev%d at %d MHz, %d cores, ESP-IDF %s", mt_esp_chip_name(),
              mt_esp_chip_revision(), mt_esp_cpu_mhz(), mt_esp_cores(), mt_esp_idf_version());

    for (int i = 0; i < s_region_count; i++) {
        const mt_esp_region_t *r = &s_regions[i];
        char total[24], tested[24];
        if (!s_region_selected[i]) {
            continue;
        }
        mt_fmt_bytes(total, sizeof(total), r->total);
        mt_fmt_bytes(tested, sizeof(tested), g_tested_bytes[i]);
        /* Coverage is what makes a memory test report trustworthy: a tester
         * that only walked 200 KiB of an 8 MiB part can pass trivially. */
        unsigned pct = r->total ? (unsigned)((g_tested_bytes[i] * 100u) / r->total) : 0u;
        if (pct > 100u) {
            pct = 100u;
        }
        mt_ui_raw(" region      %-18s %10s total, %s tested in %u block(s), %u%% covered",
                  r->name, total, tested, (unsigned)g_tested_blocks[i], pct);
        if (r->note[0]) {
            mt_ui_raw("             %s", r->note);
        }
        print_block_map(&g_pools[i], r);

        mt_ui_raw("     %-18s %-6s %-9s %-11s %-9s %s", "PATTERN", "PASS", "TIME",
                  "THROUGHPUT", "ERRORS", "RESULT");
        /* The history cursor must restart for every region. It used to be
         * declared once outside this loop, so the first region's scan consumed
         * every row in the history - including the rows belonging to the other
         * regions - and every later region printed an empty table. */
        for (size_t index = 0; index < s_history_count; index++) {
            const history_row_t *h = history_at(index);
            if (strcmp(h->region, r->short_name) != 0) {
                continue;
            }
            char rate[16];
            snprintf(rate, sizeof(rate), "%.1fMB/s", h->result.mbps);
            mt_ui_raw("     %-18s %u/%-4u %8.2fs %-11s %-9llu %s", h->pattern,
                      (unsigned)h->result.passes, (unsigned)h->result.passes_target,
                      h->result.seconds, rate,
                      (unsigned long long)h->result.cells_failed,
                      h->result.cells_failed ? "FAIL" : (h->result.complete ? "PASS" : "STOPPED"));
            if (h->result.cells_failed) {
                print_errors(&h->result, &g_pools[i]);
                print_hint(&h->result);
            }
            cells += h->result.cells;
            errors += h->result.cells_failed;
            seconds += h->result.seconds;
            tests++;
            passed = passed && (h->result.cells_failed == 0);
        }
        mt_ui_rule();
    }

    {
        char verified[24];
        mt_fmt_bytes(verified, sizeof(verified), cells * 4u);
        mt_ui_raw(" totals      %u test(s), %s verified in %.2f s, %llu error(s)", (unsigned)tests,
                  verified, seconds, (unsigned long long)errors);
        mt_ui_raw(" RESULT     %s", passed ? "PASS - no memory faults detected"
                                           : "FAIL - memory faults detected");
        mt_ui_rule();
    }
}

/* ------------------------------------------------------------------------- */
/* Run loop                                                                 */
/* ------------------------------------------------------------------------- */

static void run_region(int idx, size_t *scroll)
{
    const mt_esp_region_t *r = &s_regions[idx];
    mt_pool_t *pool = &g_pools[idx];
    mt_pool_request_t req;
    size_t target = target_bytes_for(r);
    size_t block = block_bytes_for(r, target);
    char tested[24], free[24], largest[24];

    if (!r->present) {
        mt_ui_log("%s: not present on this board - skipped", r->name);
        return;
    }
    if (!r->mallocable) {
        mt_ui_log("%s: %s - skipped", r->name, r->note);
        return;
    }

    memset(pool, 0, sizeof(*pool));
    req.block_bytes = block;
    req.target_bytes = target;
    req.min_block = 4096;
    req.caps = r->caps;

    mt_esp_set_reserve(region_uses_internal(r)
                           ? (size_t)CONFIG_MEMTEST_RESERVE_KB * 1024u
                           : 0);

    if (!mt_pool_acquire(pool, &req, mt_esp_alloc, NULL)) {
        mt_ui_log("%s: could not allocate any test memory", r->name);
        history_row_t *row = history_push();
        snprintf(row->region, sizeof(row->region), "%s", r->short_name);
        snprintf(row->pattern, sizeof(row->pattern), "%s", "allocation failed");
        row->result.cells_failed = 1;
        row->result.complete = false;
        return;
    }

    mt_fmt_bytes(tested, sizeof(tested), pool->total_bytes);
    mt_fmt_bytes(free, sizeof(free), r->free_bytes);
    mt_fmt_bytes(largest, sizeof(largest), pool->largest_block);
    mt_ui_log("%s: testing %s (free %s, largest block %s)", r->short_name, tested, free, largest);
    g_tested_bytes[idx] = pool->total_bytes;
    g_tested_blocks[idx] = (uint32_t)pool->count;

    for (size_t t = 0; t < s_testset.count; t++) {
        history_row_t *row;
        const mt_pattern_desc_t *desc = mt_pattern_desc(s_testset.tests[t].pattern);
        mt_stop_reason_t stop = MT_STOP_NONE;

        if (s_ctl.quit) {
            break;
        }
        row = history_push();
        snprintf(row->region, sizeof(row->region), "%s", r->short_name);
        snprintf(row->pattern, sizeof(row->pattern), "%s", desc ? desc->name : "?");
        row->active = true;
        s_frame.test_name = row->pattern;
        s_frame.title = r->short_name;
        s_frame.state = MT_UI_STATE_RUN;
        draw(*scroll);

        s_testset.tests[t].passes = s_passes;
        mt_run_test(pool, &s_testset.tests[t], s_seed, mt_esp_time_us, mt_esp_task_yield,
                    on_progress, scroll, &s_ctl.skip, &row->result, &stop);
        s_ctl.skip = false;
        row->active = false;

        if (row->result.cells_failed) {
            mt_ui_log("%s %s: %llu bad cells", r->short_name, row->pattern,
                      (unsigned long long)row->result.cells_failed);
        }
        s_frame.state = MT_UI_STATE_RUN;
        draw(*scroll);
    }

    g_pools[idx] = *pool;   /* block addresses are kept for the report */
    mt_pool_release(pool, mt_esp_free, NULL);
    /* g_tested_bytes[] / g_tested_blocks[] deliberately stay set: the report
     * is printed after the memory has been released, and those two counters are
     * the only record of what was actually covered. They are cleared by
     * run_suite() when a fresh sweep starts, never here. */
    mt_esp_set_reserve((size_t)CONFIG_MEMTEST_RESERVE_KB * 1024u);
    mt_ui_log("%s: released %s", r->short_name, tested);
}

static void run_suite(void)
{
    size_t scroll = 0;
    s_frame.state = MT_UI_STATE_RUN;
    s_frame.title = NULL;

    while (!s_ctl.quit) {
        for (int i = 0; i < s_region_count; i++) {
            if (s_region_selected[i]) {
                run_region(i, &scroll);
            }
            if (s_ctl.quit) {
                break;
            }
        }
        if (s_ctl.restart) {
            mt_ui_log("restarting");
            s_ctl.restart = false;
            s_ctl.quit = false;
            s_history_count = 0;
            s_history_first = 0;
            for (int i = 0; i < MT_ESP_MAX_REGIONS; i++) {
                g_tested_bytes[i] = 0;
                g_tested_blocks[i] = 0;
            }
        } else {
            break;
        }
    }
    s_frame.state = MT_UI_STATE_DONE;
    draw(0);
}

/* ------------------------------------------------------------------------- */
/* Startup                                                                  */
/* ------------------------------------------------------------------------- */

static void banner(void)
{
    char ps[24], dram[24], flash[24];

    mt_fmt_bytes(ps, sizeof(ps), mt_esp_psram_size());
    mt_fmt_bytes(dram, sizeof(dram), mt_esp_internal_total());
    mt_fmt_bytes(flash, sizeof(flash), mt_esp_flash_size());

    memset(&s_banner, 0, sizeof(s_banner));
    s_banner.version = MEMTEST_VERSION;
    s_banner.chip = mt_esp_chip_name();
    s_banner.revision = mt_esp_chip_revision();
    s_banner.cpu_mhz = mt_esp_cpu_mhz();
    s_banner.cores = mt_esp_cores();
    s_banner.idf_version = mt_esp_idf_version();
    s_banner.flash_bytes = mt_esp_flash_size();
    s_banner.psram_present = mt_esp_psram_present();
    s_banner.psram_mallocable = mt_esp_psram_mallocable();
    s_banner.psram_size = mt_esp_psram_size();
    s_banner.psram_mode = mt_esp_psram_mode();
    s_banner.internal_total = mt_esp_internal_total();

    mt_ui_rule();
    mt_ui_raw("esp32-memtest %s", MEMTEST_VERSION);
    mt_ui_raw("  chip      %s rev%d at %d MHz, %d cores", mt_esp_chip_name(),
              mt_esp_chip_revision(), mt_esp_cpu_mhz(), mt_esp_cores());
    mt_ui_raw("  flash     %s", flash);
    mt_ui_raw("  psram     %s", mt_esp_psram_present() ? ps : "none detected");
    mt_ui_raw("  internal  %s total", dram);
    {
        char freeh[24], minh[24];
        mt_fmt_bytes(freeh, sizeof(freeh), mt_esp_free_heap());
        mt_fmt_bytes(minh, sizeof(minh), mt_esp_min_free_heap());
        mt_ui_raw("  heap      %s free, %s minimum", freeh, minh);
    }
    mt_ui_raw("  console   %u baud, %s keys", (unsigned)CONFIG_MEMTEST_BAUD,
              CONFIG_MEMTEST_KEYS ? "interactive" : "read-only");
    {
        const esp_app_desc_t *desc = esp_app_get_description();
        char id[9];
        for (int b = 0; b < 4; b++) {
            snprintf(id + (b * 2), 3, "%02x", desc->app_elf_sha256[b]);
        }
        mt_ui_raw("  firmware  ELF SHA256 %s (matches the panic dump)", id);
    }
    mt_ui_raw("  build     %s %s", __DATE__, __TIME__);
    mt_ui_rule();

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    if (!mt_esp_psram_present()) {
        mt_ui_raw("*** PSRAM was compiled in but the chip could not be initialised.");
        mt_ui_raw("*** Rebuild for the module you have:");
        mt_ui_raw("***   quad SPI PSRAM  (N8R2 and similar)  -> nothing to change");
        mt_ui_raw("***   octal SPI PSRAM (N8R8 / R8 modules)  -> CONFIG_SPIRAM_MODE_OCT=y");
        mt_ui_raw("*** The internal DRAM tests below still run and are valid.");
        mt_ui_rule();
    }
#endif
    if (mt_esp_psram_present() && !mt_esp_psram_mallocable()) {
        mt_ui_raw("*** PSRAM is present but the heap cannot hand it out.");
        mt_ui_raw("*** Enable CONFIG_SPIRAM_USE_MALLOC and flash again, or PSRAM");
        mt_ui_raw("*** cannot be tested.");
        mt_ui_rule();
    }
    if (!CONFIG_MEMTEST_KEYS) {
        mt_ui_raw("note: key handling is disabled (CONFIG_MEMTEST_KEYS=n)");
    }
    if (mt_esp_psram_present() && strcmp(mt_esp_psram_mode(), "hex") != 0) {
        char note[96];
        snprintf(note, sizeof(note), "note: PSRAM is in %s mode", mt_esp_psram_mode());
        mt_ui_raw("%s", note);
    }
}

static void load_regions(void)
{
    s_region_count = mt_esp_regions(s_regions, MT_ESP_MAX_REGIONS);
    for (int i = 0; i < s_region_count; i++) {
        if (CONFIG_MEMTEST_VERBOSE) {
            char size[24];
            mt_fmt_bytes(size, sizeof(size), s_regions[i].total);
            mt_ui_log("region %-8s %-18s %10s  caps 0x%05lx", s_regions[i].short_name,
                      s_regions[i].name, size, (unsigned long)s_regions[i].caps);
        }
    }
    parse_region_selection(CONFIG_MEMTEST_REGIONS);
}

void app_main(void)
{
    bool start = true;

    s_boot_ms = mt_esp_time_ms();
    mt_esp_init(CONFIG_MEMTEST_BAUD);
    mt_ui_set_interactive(CONFIG_MEMTEST_KEYS);
    mt_ui_begin(MT_UI_AUTO);
    mt_esp_set_reserve((size_t)CONFIG_MEMTEST_RESERVE_KB * 1024u);

#ifdef MEMTEST_HAVE_PSRAM_API
    /* The ESP-IDF startup initialises PSRAM before app_main() runs. If that
     * attempt came up empty, make one more before giving up: a die that is
     * merely marginal often answers on a retry, and without PSRAM there is
     * nothing worth testing. */
    if (!esp_psram_is_initialized()) {
        esp_err_t psram_err = esp_psram_init();
        if (esp_psram_is_initialized()) {
            mt_ui_log("PSRAM was down at boot, esp_psram_init() recovered it (%s)",
                esp_err_to_name(psram_err));
        }
    }
#endif

    load_regions();
    banner();

    if (!load_testset(CONFIG_MEMTEST_TESTS)) {
        load_testset("standard");
    }

    if (CONFIG_MEMTEST_SELFTEST) {
        if (!engine_selftest()) {
            mt_ui_log("continuing anyway - treat every failure with suspicion");
        }
    }

    if (!CONFIG_MEMTEST_AUTORUN) {
        start = main_menu();
    }

    if (start) {
        /* Stay on the banner screen with a single verdict line under it. The
         * full report is deliberately not printed here: it scrolls away the
         * summary the operator actually wants to read, and it used to end the
         * session. It is available on demand. */
        for (;;) {
            int action = 0;
            int key;

            memset(&s_frame, 0, sizeof(s_frame));
            s_frame.state = MT_UI_STATE_IDLE;
            s_ctl.quit = false;
            s_ctl.pause = false;
            s_ctl.skip = false;
            s_history_count = 0;
            s_history_first = 0;
            memset(g_tested_bytes, 0, sizeof(g_tested_bytes));
            memset(g_tested_blocks, 0, sizeof(g_tested_blocks));

            run_suite();

            s_frame.state = MT_UI_STATE_DONE;
            draw(0);
            print_verdict_line();
            if (CONFIG_MEMTEST_KEYS && mt_ui_interactive()) {
                mt_ui_raw(" keys:  r run again    Enter print report    q exit");
            }
            mt_ui_rule();

            /* Wait for a key so the screen stays readable. Nothing is cleared
             * and nothing is printed twice. */
            for (;;) {
                mt_esp_delay_ms(200);
                while ((key = mt_esp_poll_key()) >= 0) {
                    if (key == 'q' || key == 'Q') {
                        action = 'q';
                    } else if (key == 'r' || key == 'R') {
                        action = 'r';
                    } else if (key == '\r' || key == '\n' || key == MT_KEY_SPACE) {
                        action = 'p';
                    }
                    if (action != 0) {
                        break;
                    }
                }
                if (action != 0) {
                    break;
                }
            }

            if (action == 'q') {
                break;
            }
            if (action == 'p') {
                mt_ui_end();
                print_report();
                break;
            }
            /* action == 'r': start over */
        }
    } else {
        mt_ui_end();
        mt_ui_rule();
        mt_ui_raw("esp32-memtest: nothing was tested.");
    }

    if (CONFIG_MEMTEST_REBOOT) {
        mt_ui_rule();
        mt_ui_raw("rebooting in %d ms", CONFIG_MEMTEST_REBOOT_DELAY_MS);
        mt_esp_delay_ms(CONFIG_MEMTEST_REBOOT_DELAY_MS);
        esp_restart();
    }
}