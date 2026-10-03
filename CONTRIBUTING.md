# Contributing

Thanks for helping to make the tester better at finding broken memory.

## Getting set up

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
make -C tests/host all
```

`idf.py monitor` on a real board is the only way to validate the console
behaviour; the host tests cover the engine and the screen layout, not the
serial driver.

## Ground rules for the code

* **C11, no dynamic allocation in the engine.** `memtest_core` compiles for
  both the host and the firmware, so it must not include ESP-IDF headers and
  must not depend on the FreeRTOS heap. The ESP-IDF heap is only reached
  through the allocator callback that `mt_run_test()` receives.
* **The tester must never take the system down.** Every allocation has to
  respect the reserve configured with `CONFIG_MEMTEST_RESERVE_KB`, and a
  failed allocation has to be reported, not retried in a loop.
* **Do not assume a PSRAM size, a block size or a chip.** Everything is
  discovered at run time through `mt_esp_*`.
* **The screen is 80 columns and 23 rows.** `make -C tests/host render-keep`
  replays real ANSI output through a terminal emulator and catches lines that
  are too long, frames that scroll the terminal and frames that leave stale
  text behind. Run it after touching `components/memtest_ui`.
* `printf` format arguments that are `uint32_t` need an explicit
  `(unsigned)` cast: on xtensa `uint32_t` is `unsigned long`, and ESP-IDF
  builds with `-Werror=all`.

## Style

* Four spaces, no tabs, 100 column lines.
* Braces on their own line for functions, on the same line for `if`/`for`.
* Types are `snake_case_t`, functions `snake_case()`, constants
  `UPPER_SNAKE_CASE`.
* Comments explain *why*, not *what*. The code already says what.
* SPDX header on every file.

## Tests

* Engine changes need new cases in `tests/host/test_core.c`, including the
  ones you would want if the change broke: a fragmented heap, an allocation
  that fails halfway, a pattern that verifies a single wrong bit.
* Screen changes need a case in `tests/host/test_ui.c` or
  `tests/host/render_demo.c`.
* `make -C tests/host asan` must stay clean.

## Commit messages

Short imperative subject, one logical change per commit, and mention the
target board if the change is chip specific. If a change fixes a bug that
only shows up on real hardware, say how it was reproduced - that is the part
nobody else can guess.

## Pull requests

Build for at least `esp32s3` and `esp32`, run the host tests, and describe
what changed and why. Screenshots of the serial output help a lot for UI
changes.