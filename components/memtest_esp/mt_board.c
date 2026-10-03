/*
 * esp32-memtest - chip, memory and timing abstraction.
 *
 * SPDX-License-Identifier: MIT
 */

#include "memtest_esp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "soc/soc_caps.h"
#include "esp_heap_caps.h"
/* Both conditions are required. SOC_SPIRAM_SUPPORTED says the chip has PSRAM;
 * CONFIG_SPIRAM says this build enables it. IDF only compiles the code that
 * defines esp_psram_get_size() / esp_psram_is_initialized() when CONFIG_SPIRAM
 * is set (esp_psram/CMakeLists.txt), so on e.g. esp32p4 the chip supports PSRAM
 * but the symbols are absent and the link fails. */
#if defined(CONFIG_SPIRAM) && defined(SOC_SPIRAM_SUPPORTED)
#include "esp_psram.h"
#define MEMTEST_ESP_HAS_PSRAM_API 1
#endif
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef CONFIG_ESP_COREDUMP_ENABLE_NONE
#endif

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) || defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#include "driver/usb_serial_jtag.h"
#define MT_ESP_HAS_USB_CONSOLE 1
#endif

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#define MT_ESP_HAS_UART_CONSOLE 1
#endif

#if CONFIG_ESP_CONSOLE_UART_DEFAULT
#define MT_ESP_CONSOLE_PORT ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#endif

/* ------------------------------------------------------------------------- */

static size_t s_reserve_bytes;

void mt_esp_set_reserve(size_t bytes)
{
    s_reserve_bytes = bytes;
}

size_t mt_esp_get_reserve(void)
{
    return s_reserve_bytes;
}

void *mt_esp_alloc(size_t bytes, uint32_t caps, void *user)
{
    (void)user;
    if (bytes == 0) {
        return NULL;
    }
    if (s_reserve_bytes != 0) {
        size_t free_now = heap_caps_get_free_size(caps);
        if (free_now < bytes + s_reserve_bytes) {
            return NULL;   /* would starve the runtime */
        }
    }
    return heap_caps_malloc(bytes, caps);
}

void mt_esp_free(void *ptr, void *user)
{
    (void)user;
    heap_caps_free(ptr);
}

/* ------------------------------------------------------------------------- */

const char *mt_esp_chip_name(void)
{
    static char name[24];
    esp_chip_info_t info;
    esp_chip_info(&info);
    switch (info.model) {
    case CHIP_ESP32:     snprintf(name, sizeof(name), "ESP32");      break;
    case CHIP_ESP32S2:   snprintf(name, sizeof(name), "ESP32-S2");   break;
    case CHIP_ESP32S3:   snprintf(name, sizeof(name), "ESP32-S3");   break;
    case CHIP_ESP32C2:   snprintf(name, sizeof(name), "ESP32-C2");   break;
    case CHIP_ESP32C3:   snprintf(name, sizeof(name), "ESP32-C3");   break;
    case CHIP_ESP32C5:   snprintf(name, sizeof(name), "ESP32-C5");   break;
    case CHIP_ESP32C6:   snprintf(name, sizeof(name), "ESP32-C6");   break;
    case CHIP_ESP32C61:  snprintf(name, sizeof(name), "ESP32-C61");  break;
    case CHIP_ESP32H2:   snprintf(name, sizeof(name), "ESP32-H2");   break;
    case CHIP_ESP32H21:  snprintf(name, sizeof(name), "ESP32-H21");  break;
    case CHIP_ESP32H4:   snprintf(name, sizeof(name), "ESP32-H4");   break;
    case CHIP_ESP32P4:   snprintf(name, sizeof(name), "ESP32-P4");   break;
    default:             snprintf(name, sizeof(name), "ESP32-?%d", (int)info.model); break;
    }
    return name;
}

int mt_esp_chip_revision(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    return info.revision;
}

int mt_esp_cores(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    return info.cores;
}

int mt_esp_cpu_mhz(void)
{
    return esp_clk_cpu_freq() / 1000000;
}

uint32_t mt_esp_flash_size(void)
{
    uint32_t size = 0;
    if (esp_flash_get_size(NULL, &size) != ESP_OK) {
        return 0;
    }
    return size;
}

const char *mt_esp_idf_version(void)
{
    return IDF_VER;
}

const char *mt_esp_build_date(void)
{
    return __DATE__ " " __TIME__;
}

/* ------------------------------------------------------------------------- */

size_t mt_esp_psram_size(void)
{
#ifdef MEMTEST_ESP_HAS_PSRAM_API
    return esp_psram_get_size();
#else
    return 0;               /* ESP32-C3, C2 and friends have no PSRAM */
#endif
}

bool mt_esp_psram_present(void)
{
#ifdef MEMTEST_ESP_HAS_PSRAM_API
    return esp_psram_is_initialized();
#else
    return false;
#endif
}

bool mt_esp_psram_mallocable(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

const char *mt_esp_psram_mode(void)
{
#if defined(CONFIG_SPIRAM_MODE_HEX)
    return "hex";
#elif defined(CONFIG_SPIRAM_MODE_OCT)
    return "octal";
#else
    return "unknown";
#endif
}

size_t mt_esp_internal_total(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
}

size_t mt_esp_free_heap(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}

size_t mt_esp_min_free_heap(void)
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
}

/* ------------------------------------------------------------------------- */

static const struct {
    const char *key;
    const char *name;
    const char *short_name;
    uint32_t caps;
} s_region_defs[MT_ESP_MAX_REGIONS] = {
    { "psram",    "PSRAM (SPI)",      "PSRAM",    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT },
    { "internal", "Internal DRAM",    "DRAM",     MALLOC_CAP_INTERNAL },
    { "dma",      "DMA capable DRAM", "DMA",      MALLOC_CAP_DMA },
    { "any",      "Default heap",     "ANY",      MALLOC_CAP_DEFAULT },
};

static void describe_region(mt_esp_region_t *r)
{
    memset(r->note, 0, sizeof(r->note));
    r->total = heap_caps_get_total_size(r->caps);
    r->free_bytes = heap_caps_get_free_size(r->caps);
    r->largest_block = heap_caps_get_largest_free_block(r->caps);
    r->min_free = heap_caps_get_minimum_free_size(r->caps);
    r->present = r->total > 0;
    r->mallocable = r->present;

    if (strcmp(r->key, "psram") == 0) {
        size_t chip_size = mt_esp_psram_size();
        if (chip_size == 0) {
            r->present = false;
            r->mallocable = false;
            snprintf(r->note, sizeof(r->note),
                     "no PSRAM detected - check the module and the PSRAM pins");
        } else {
            /* The PSRAM heap only exists when the allocator is allowed to use
             * PSRAM; otherwise heap_caps_get_total_size() reports nothing. */
            r->total = chip_size;
            if (!r->mallocable) {
                snprintf(r->note, sizeof(r->note),
                         "PSRAM present but not in the heap - enable "
                         "CONFIG_SPIRAM_USE_MALLOC");
            }
        }
    } else if (!r->present && strcmp(r->key, "dma") == 0) {
        snprintf(r->note, sizeof(r->note), "no DMA capable heap on this chip");
    } else if (strcmp(r->key, "any") == 0) {
        snprintf(r->note, sizeof(r->note), "internal RAM plus PSRAM, allocator decides");
    } else if (r->present && strcmp(r->key, "dma") == 0) {
        snprintf(r->note, sizeof(r->note), "subset of internal DRAM");
    }
}

int mt_esp_regions(mt_esp_region_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = 0;
    for (unsigned i = 0; i < MT_ESP_MAX_REGIONS && n < max; i++) {
        out[n].key = s_region_defs[i].key;
        out[n].name = s_region_defs[i].name;
        out[n].short_name = s_region_defs[i].short_name;
        out[n].caps = s_region_defs[i].caps;
        describe_region(&out[n]);
        n++;
    }
    return n;
}

int mt_esp_region_index(const char *key)
{
    if (key == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < MT_ESP_MAX_REGIONS; i++) {
        if (strcasecmp(s_region_defs[i].key, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */

uint64_t mt_esp_time_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

uint64_t mt_esp_time_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

void mt_esp_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* The engine calls this between chunks of work.
 *
 * This must BLOCK, not just yield. The ESP-IDF task watchdog is fed from the
 * idle task, and the idle task only runs when every higher-priority task is
 * blocked. taskYIELD() merely rotates between equal-priority ready tasks, so on
 * this single-task workload it returns immediately, the idle task never runs,
 * and the task watchdog fires mid-test. vTaskDelay(1) really does hand the CPU
 * to idle. MT_YIELD_BYTES in the engine is tuned so this happens rarely enough
 * that the sleep is not what dominates the runtime. */
void mt_esp_task_yield(void)
{
    vTaskDelay(1);
}

/* ------------------------------------------------------------------------- */
/* Keyboard input                                                          */
/* ------------------------------------------------------------------------- */

#if defined(MT_ESP_HAS_USB_CONSOLE)
static bool s_usb_jtag_ready;
#endif
#if defined(MT_ESP_HAS_UART_CONSOLE)
static bool s_uart_driver_ready;
#endif

static int decode_key(const unsigned char *buf, size_t len, size_t *consumed)
{
    if (len == 0) {
        return -1;
    }
    /* Arrow keys arrive as ESC [ A .. D */
    if (buf[0] == 0x1B && len >= 3 && buf[1] == '[') {
        *consumed = 3;
        switch (buf[2]) {
        case 'A': return MT_KEY_UP;
        case 'B': return MT_KEY_DOWN;
        default:  return -1;
        }
    }
    *consumed = 1;
    if (buf[0] == 0x1B) {
        return -1;   /* lone escape: ignore */
    }
    if (buf[0] == '\r' || buf[0] == '\n' || buf[0] == '\t') {
        return buf[0];
    }
    return (int)buf[0];
}

int mt_esp_poll_key(void)
{
#if defined(MT_ESP_HAS_UART_CONSOLE) && defined(MT_ESP_CONSOLE_PORT)
    if (s_uart_driver_ready) {
        unsigned char buf[16];
        size_t buffered = 0;
        if (uart_get_buffered_data_len(MT_ESP_CONSOLE_PORT, &buffered) == ESP_OK &&
            buffered > 0) {
            int n = uart_read_bytes(MT_ESP_CONSOLE_PORT, buf, sizeof(buf), 0);
            if (n > 0) {
                size_t consumed = 0;
                return decode_key(buf, (size_t)n, &consumed);
            }
        }
    }
#endif
#if defined(MT_ESP_HAS_USB_CONSOLE)
    if (s_usb_jtag_ready) {
        unsigned char buf[16];
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
        if (n > 0) {
            size_t consumed = 0;
            return decode_key(buf, (size_t)n, &consumed);
        }
    }
#endif
    return -1;
}

const char *mt_esp_key_name(int key)
{
    static char one[2] = { 0, 0 };
    if (key < 0) {
        return "?";
    }
    if (key == MT_KEY_UP) {
        return "UP";
    }
    if (key == MT_KEY_DOWN) {
        return "DOWN";
    }
    if (key >= 0x20 && key < 0x7F) {
        one[0] = (char)key;
        one[1] = '\0';
        return one;
    }
    return "?";
}

/* ------------------------------------------------------------------------- */

void mt_esp_init(uint32_t baud)
{
    (void)baud;   /* the baud rate belongs to whoever configured the console */

#if defined(MT_ESP_HAS_UART_CONSOLE) && defined(MT_ESP_CONSOLE_PORT)
    /* The console VFS may already own the driver; installing it a second time
     * is harmless as long as the failure is not mistaken for a fatal error. */
    esp_err_t err = uart_driver_install(MT_ESP_CONSOLE_PORT, 1024, 0, 0, NULL, 0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_uart_driver_ready = true;
    }
#endif
#if defined(MT_ESP_HAS_USB_CONSOLE)
    /* Only poll the driver if it is genuinely installed. usb_serial_jtag_read_bytes()
     * dereferences the driver handle without checking, so calling it when IDF did not
     * install one is an immediate NULL dereference. */
    s_usb_jtag_ready = usb_serial_jtag_is_driver_installed();
#endif
}