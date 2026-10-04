# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

First release.

### Bug fixes

- release download links and shorten the release body ([`5e272fc`](https://github.com/sjwon0368/esp32-memtest/commit/5e272fc))
- release packaging: one shell, quiet install, validate merged images ([`2e3bcc5`](https://github.com/sjwon0368/esp32-memtest/commit/2e3bcc5))
- CI: espressif/idf action does not exist, install ESP-IDF directly ([`c480530`](https://github.com/sjwon0368/esp32-memtest/commit/c480530))

### Features

- issue templates and document how to report a problem ([`5ed523f`](https://github.com/sjwon0368/esp32-memtest/commit/5ed523f))
- release notes generator and fix zip output path ([`b6a55e2`](https://github.com/sjwon0368/esp32-memtest/commit/b6a55e2))

### Housekeeping

- Make changelog generation idempotent and drop a duplicated section ([`227ed20`](https://github.com/sjwon0368/esp32-memtest/commit/227ed20))
- Document CI by trigger, not by how to run it ([`63f06fc`](https://github.com/sjwon0368/esp32-memtest/commit/63f06fc))
- Generate release notes from git history and fix zip output path ([`93198ec`](https://github.com/sjwon0368/esp32-memtest/commit/93198ec))
- Make release default to a draft and validate its inputs ([`e853837`](https://github.com/sjwon0368/esp32-memtest/commit/e853837))
- Split CI by cost and add a release workflow ([`28d63cf`](https://github.com/sjwon0368/esp32-memtest/commit/28d63cf))
- Reframe README board profile as target defaults; run CI on demand only ([`aeb0cba`](https://github.com/sjwon0368/esp32-memtest/commit/aeb0cba))
- Install ESP-IDF with an explicit IDF_PATH instead of runner env files ([`8151b34`](https://github.com/sjwon0368/esp32-memtest/commit/8151b34))
- Export ESP-IDF environment to later CI steps ([`8ba4935`](https://github.com/sjwon0368/esp32-memtest/commit/8ba4935))
- Run CI on master instead of main ([`a443cae`](https://github.com/sjwon0368/esp32-memtest/commit/a443cae))
- Ignore captured results.md ([`ef491a6`](https://github.com/sjwon0368/esp32-memtest/commit/ef491a6))
- Set copyright holder and expand setup instructions ([`9f4164b`](https://github.com/sjwon0368/esp32-memtest/commit/9f4164b))
- esp32-memtest 1.0.0: serial memory tester for Espressif MCUs ([`92a88b6`](https://github.com/sjwon0368/esp32-memtest/commit/92a88b6))

### Documentation

- changelog for v1.0.0 ([`ade1e13`](https://github.com/sjwon0368/esp32-memtest/commit/ade1e13))

[Full diff: all commits](https://github.com/sjwon0368/esp32-memtest/compare/initial...v1.0.0)

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

### Continuous integration

- Split into three workflows: `ci.yml` (host tests, runs on every push),
  `firmware.yml` (builds all four targets, manual) and `build-release.yml`
  (builds and publishes a GitHub release, manual). Only the fast host tests run
  automatically; each firmware job installs a complete ESP-IDF toolchain.
- `build-release.yml` publishes a merged image and a zip per target, named
  `ESP32`, `ESP32S3`, `ESP32C3` and `ESP32P4`, with per-target flashing
  instructions generated from `packaging/FLA.in.txt`.
- The merged images take their flash mode, frequency and size from each
  bootloader's own header instead of a hardcoded table, because they differ per
  target (the `esp32` build is 2 MB at 40 MHz, the `esp32s3` build is 16 MB).
