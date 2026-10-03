# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0]

### Added

- Portable memory test engine with ten patterns: `zero`, `ones`, `aa55`,
  `address`, `random`, `movinginv`, `checker`, `bitone`, `bitzero`, `modulox`.
- Named test sets: `quick`, `standard`, `extended`, `full`, `random`, `walking`.
- Hardware discovery through `heap_caps` for PSRAM, internal DRAM, DMA-capable
  DRAM and the default heap.
- Live ANSI screen with a plain-text fallback for dumb terminals, usable over
  any serial console at 115200 baud.
- Runtime interactive menu (`CONFIG_MEMTEST_AUTORUN=n`) to pick regions, test
  set and pass count without reflashing.
- Per-cell error reporting with the address, the expected and the actual value.
- Host test suite for the engine, the screen composition and the ANSI replay
  harness, including an ASan/UBSan configuration.
- `flash.sh` helper and a single-file merged image (`esptool merge_bin`).
- Support for `esp32`, `esp32s3`, `esp32c3` and `esp32p4`.

### Fixed

- Only the first region ever printed its results. The cursor into the result
  history was declared once outside the region loop, so the first region's scan
  consumed every row in the history - including the rows belonging to the other
  regions - and each later region printed an empty table. The DRAM rows existed
  all along, they were simply never read.
- The result history counter grew past the size of its ring buffer once more
  than 48 rows had been recorded, which made the report list the same rows
  twice.
- PSRAM was reported as absent on every board. The compile-time guard used
  `SOC_PSRAM_SUPPORTED`, which no ESP-IDF target defines; the correct macro is
  `SOC_SPIRAM_SUPPORTED`. Because an undefined macro in `#if` silently evaluates
  to zero, the tester always took the "no PSRAM" branch and skipped the region
  even when several MiB of PSRAM were sitting in the heap.
- The console crashed with `LoadProhibited` on boards whose USB is a bridge
  chip. The tester assumed the USB Serial/JTAG driver was installed and called
  `usb_serial_jtag_read_bytes()` without checking; it now verifies the driver is
  present, and the secondary console defaults to none because ESP-IDF turns it
  on by default for every SoC that has the peripheral.
- The report claimed `0 B tested in 0 block(s)` while tests were running over
  the whole pool: the per-region coverage counters were cleared immediately
  before the report was generated. The report now also states the percentage of
  each region that was actually covered.
- Throughput was roughly 3 MB/s because the engine yielded with `vTaskDelay(1)`
  every 512 KiB, which rounds up to a whole FreeRTOS tick (1-10 ms). It now uses
  `taskYIELD()`, so other tasks still get scheduled without burning a tick.

### Changed

- The default test set is `full`, which runs all ten patterns instead of the
  seven in `standard`.
- The banner screen stays on the console after a run with a single verdict
  line. The long report is printed only when a key is pressed.
- The finished screen names its own keys, and `r` runs the suite again instead
  of printing the report.
- The result list holds ten rows instead of eight, and the scroll offset
  survives a redraw, so a full run can be reviewed without it snapping back.
- The screen width is `CONFIG_MEMTEST_UI_WIDTH` (default 78, an 80-column
  display) so a wider terminal can be used without editing the source.