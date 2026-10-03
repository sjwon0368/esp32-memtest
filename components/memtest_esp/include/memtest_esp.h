/*
 * esp32-memtest - ESP-IDF platform layer.
 *
 * SPDX-License-Identifier: MIT
 *
 * Everything that knows about the actual chip lives here: memory discovery,
 * allocation with a reserve policy, timing and keyboard input.  The test
 * engine itself never sees an ESP-IDF header.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "memtest_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of memory regions memtest_esp knows how to describe. */
#define MT_ESP_MAX_REGIONS 4

typedef struct {
    const char *key;         /**< "psram", "internal", "dma", "any"        */
    const char *name;        /**< long name shown in the UI                 */
    const char *short_name;  /**< column heading                            */
    uint32_t caps;           /**< heap_caps mask used for allocation         */
    size_t total;            /**< physical size of the region               */
    size_t free_bytes;       /**< currently free                            */
    size_t largest_block;    /**< biggest single allocation possible        */
    size_t min_free;         /**< smallest free size seen so far            */
    bool present;            /**< the hardware region exists                */
    bool mallocable;         /**< the heap can hand it out                  */
    char note[96];           /**< human readable hint, may be empty         */
} mt_esp_region_t;

/** Bring up console input and any peripheral the tester needs. */
void mt_esp_init(uint32_t baud);

/* --- chip information ------------------------------------------------ */

const char *mt_esp_chip_name(void);
int         mt_esp_chip_revision(void);
int         mt_esp_cpu_mhz(void);
int         mt_esp_cores(void);
uint32_t    mt_esp_flash_size(void);
const char *mt_esp_idf_version(void);
const char *mt_esp_build_date(void);

/* --- memory information ---------------------------------------------- */

/**
 * Fill @p out with up to @p max region descriptors.
 * @return number of descriptors written.
 */
int mt_esp_regions(mt_esp_region_t *out, int max);

/** @return index of the region with @p key, or -1. */
int mt_esp_region_index(const char *key);

size_t mt_esp_psram_size(void);
bool   mt_esp_psram_present(void);
bool   mt_esp_psram_mallocable(void);
const char *mt_esp_psram_mode(void);
size_t mt_esp_internal_total(void);
size_t mt_esp_free_heap(void);
size_t mt_esp_min_free_heap(void);

/* --- allocation ------------------------------------------------------ */

/**
 * Amount of memory that must stay free in the heap the request targets.
 * Requesting a pool that would eat the IDF's own working memory fails
 * instead of pulling the rug from under the console.
 */
void   mt_esp_set_reserve(size_t bytes);
size_t mt_esp_get_reserve(void);
void  *mt_esp_alloc(size_t bytes, uint32_t caps, void *user);
void   mt_esp_free(void *ptr, void *user);

/* --- timing / scheduling --------------------------------------------- */

uint64_t mt_esp_time_us(void);
uint64_t mt_esp_time_ms(void);
void     mt_esp_delay_ms(uint32_t ms);
void     mt_esp_task_yield(void);

/* --- keyboard -------------------------------------------------------- */

/** @return next key press, or -1 when nothing is pending. */
int mt_esp_poll_key(void);
/** @return short name of @p key for the help line, "?" when unknown. */
const char *mt_esp_key_name(int key);

/** Key ids used by the UI (ASCII for letters, + 256 for special keys). */
#define MT_KEY_UP     0x1000
#define MT_KEY_DOWN   0x1001
#define MT_KEY_SPACE  ' '

#ifdef __cplusplus
}
#endif