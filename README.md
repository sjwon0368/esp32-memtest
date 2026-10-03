# esp32-memtest

A MemTest86-style memory tester for the ESP32 family, driven from a serial
terminal. It exercises PSRAM and internal DRAM with a set of classic memory
test patterns, shows live progress on an 80-column screen, and prints a
detailed report when it is done.

The primary target is the **ESP32-S3**, but the project builds for every
ESP32 variant that ESP-IDF v5.5 supports (verified: `esp32`, `esp32s3`,
`esp32c3`, `esp32p4`). No display, no Wi-Fi, no SD card, no keyboard: a UART
(or the built-in USB serial/JTAG) at 115200 baud is the whole interface.

```
+------------------------------------------------------------------------------+
| esp32-memtest 1.0.0     ESP32-S3 rev0  240MHz  2c ESP-IDF v5.5.5             |
| PSRAM 8.00 MiB octal (in heap)    internal DRAM 400 KiB flash 16.0 MiB       |
+------------------------------------------------------------------------------+
| MEMORY    TOTAL      FREE       TESTED     BLOCK COVERAGE                    |
|  psram    8.00 MiB   7.00 MiB   7.00 MiB   30    ##################..  94%   |
|  int      400 KiB    200 KiB    190 KiB    1     #########...........  48%   |
+------------------------------------------------------------------------------+
| REGION  TEST               PASS   TIME      THROUGHPUT  ERRORS     RESULT    |
| psram   moving inversions  1/1    130.10s   55.7MB/s    0          PASS      |
| psram   checker            1/1    4.90s     1440.2MB/s  0          ...       |
+------------------------------------------------------------------------------+
| [##..................] 12% checker            write  p1/1     96/190Ki    RUN |
| heap 180 KiB min 150 KiB up 140.00s errors 0  [h]elp [p]ause [s]kip [q]uit |
+------------------------------------------------------------------------------+
```

## Why

A board with a soldered PSRAM module is a lottery ticket until you have tested
it. `esp32-memtest` is a small, dependency-free diagnostic application: flash
it, type one key, and watch whether every cell of your PSRAM survives the
moving-inversions and address patterns. It also tells you *which* cells fail,
which is usually enough to spot a marginal module, a bad solder joint or a
wrong pin assignment before you hunt for firmware bugs.

It does not assume a particular PSRAM size, and it does not reserve a fixed
amount of memory for itself: it allocates what is available, in blocks, and
keeps enough internal RAM free for the console and the FreeRTOS runtime.

## Features

* PSRAM and internal DRAM are discovered at run time through
  `esp_psram_get_size()` and `heap_caps_get_*()`; a board without PSRAM simply
  reports it and tests what it has.
* Memory is claimed in blocks, so even a heavily fragmented heap is tested
  almost completely instead of failing on one oversized allocation.
* 13 patterns: zero, ones, `AA55`, address-derived, moving inversions, walking
  bits, checkerboard, bit-one, bit-zero, modulo-X and a seeded random pattern,
  grouped into presets (`quick`, `standard`, `extended`, `full`, `random`,
  `walking`).
* A self test over a small internal buffer runs first, so a failure of the
  tester itself is not mistaken for a failure of the module.
* Write and verify sweeps are separate passes, each block is verified twice
  (persistent and transient errors are distinguished), and a stuck or drifting
  cell is reported with its address, offset, expected value, actual value,
  XOR difference and the block it lives in.
* An 80-column ANSI screen with a live progress bar, or plain text with one
  line per event for dumb terminals, log files and CI. `u` switches at run
  time.
* Everything configurable through `menuconfig`; no rebuild needed for the
  common cases (regions, test set, pass count, seed, test size).

## Building

Requires `ESP-IDF v5.5` or newer, and:

- at least 2 GB of free storage
- room for a full stack install, which can grow to about 10 GB

>[!NOTE]
>The setup commands below are for Linux. On Windows, use the ESP-IDF PowerShell
>installer and then run the same `idf.py` commands from an ESP-IDF terminal.
>
>Windows-specific instructions would be welcome as a pull request.

```sh
git clone https://github.com/sjwon0368/esp32-memtest
cd esp32-memtest

. $IDF_PATH/export.sh          # or: . /opt/esp-idf/export.sh
idf.py set-target esp32s3      # esp32, esp32c3, esp32p4, ...

idf.py -p /dev/ttyUSB0 flash monitor
```

Replace `/dev/ttyUSB0` with your actual port; it can also be `/dev/ttyACM*`
depending on the USB bridge on your board.

Set the terminal to 115200 baud, 8N1. Press `Ctrl-]` to leave the monitor if you
used `idf.py flash monitor`.

Always select the chip explicitly with `set-target` (or `-DIDF_TARGET=...`). With
no `sdkconfig` present, ESP-IDF 5.5 falls back to plain `esp32` and silently
builds an image for the wrong chip.

### Target defaults and hardware profiles

The firmware is the same for every supported chip. It never assumes a PSRAM
size, a flash size or a pinout: at run time it asks the heap what exists and
reports that. The only per-board work is telling ESP-IDF how your module is
wired, and that is the job of these files:

| File | Applied when building for |
| --- | --- |
| `sdkconfig.defaults` | every target - console, tester options |
| `sdkconfig.defaults.esp32` | `esp32` (original, quad PSRAM) |
| `sdkconfig.defaults.esp32s3` | `esp32s3` (octal PSRAM, 16 MB flash) |
| - | `esp32c3`, `esp32p4` use `sdkconfig.defaults` only |

ESP-IDF appends `sdkconfig.defaults.<target>` automatically once the target is
selected, so `idf.py set-target esp32s3` picks up the S3 profile with no extra
step.

#### Which PSRAM and flash does my module have?

Module names encode both: `N16R8` = 16 MB flash (`N16`) and 8 MB PSRAM (`R8`).

| Module | Flash | PSRAM | Line mode | Flash size setting |
| --- | --- | --- | --- | --- |
| `N16R8` | 16 MB quad | 8 MB | **octal** | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` |
| `N8R2` | 8 MB quad | 2 MB | quad (default) | `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y` |
| `N16R8V` | 16 MB | 8 MB | octal, **1.8 V** | as above |

This is the single most common reason a board appears to have no PSRAM. Quad
PSRAM is the ESP-IDF default for the S3, so an octal module must ask for it
explicitly. Get it wrong and the chip boot-loops before the tester starts:

```
E (165) quad_psram: PSRAM chip is not connected, or wrong PSRAM line mode
E cpu_start: Failed to init external RAM!
abort() was called at PC 0x42001adf on core 0
```

If you have a different module, add a `sdkconfig.defaults.<target>` line (or
change the existing one) and rebuild. No source change is needed.

`idf.py menuconfig` -> **Memory tester (esp32-memtest)** holds every option:

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_MEMTEST_AUTORUN` | `y` | start testing on boot instead of showing a menu |
| `CONFIG_MEMTEST_REGIONS` | `psram,internal` | comma separated: `psram`, `internal`, `dma`, `any` |
| `CONFIG_MEMTEST_TESTS` | `full` | presets and/or individual pattern keys |
| `CONFIG_MEMTEST_PASSES` | `1` | passes over the test set, `0` = until a key is pressed |
| `CONFIG_MEMTEST_SEED` | `4919` | seed for the random pattern |
| `CONFIG_MEMTEST_TEST_SIZE_MB` | `0` | fixed size per region, `0` = use everything available |
| `CONFIG_MEMTEST_MAX_TEST_MB` | `0` | upper limit for automatic sizing |
| `CONFIG_MEMTEST_BLOCK_KB` | `256` | block size, smaller covers a more fragmented heap |
| `CONFIG_MEMTEST_RESERVE_KB` | `96` | internal RAM kept free so the runtime survives |
| `CONFIG_MEMTEST_SELFTEST` | `y` | verify the tester before blaming the memory |
| `CONFIG_MEMTEST_UI_ANSI` | `y` | full screen UI (`n` = plain text) |
| `CONFIG_MEMTEST_UI_WIDTH` | `78` | screen width, borders included; widen it *and* the terminal together |
| `CONFIG_MEMTEST_REFRESH_MS` | `200` | screen refresh interval |
| `CONFIG_MEMTEST_KEYS` | `y` | read keys from the console |
| `CONFIG_MEMTEST_BAUD` | `115200` | console baud rate |
| `CONFIG_MEMTEST_REBOOT` | `n` | reboot the board when the run finishes |
| `CONFIG_MEMTEST_REBOOT_DELAY_MS` | `5000` | delay before that reboot |
| `CONFIG_MEMTEST_VERBOSE` | `n` | log every allocation decision |

Patterns can be listed individually, for example
`CONFIG_MEMTEST_TESTS="quick,modulox"` or `"address,random"`.

### PSRAM has to be in the heap

`CONFIG_SPIRAM_USE_MALLOC` is enabled in `sdkconfig.defaults`. Without it the
ESP-IDF allocator refuses to hand out PSRAM and there is nothing to test; the
application prints a loud warning at boot when it detects this case.

## Flashing a single file

The bootloader, partition table and application can be merged into one image and
flashed at offset `0x0`:

```sh
esptool.py --chip esp32s3 merge_bin -o esp32-memtest-merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/esp32-memtest.bin

esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 esp32-memtest-merged.bin
```

`./flash.sh` does the port detection, flashing and monitoring. A prebuilt image
is in `dist/`; `--flash_size` must match the board (16 MB for `N16R8`), a too
small header caps the flash the bootloader maps.

### Prebuilt images

Releases attach a merged image per target, each named after the chip and
flashed at `0x0`:

| File | Chip |
| --- | --- |
| `ESP32.bin` | `esp32` |
| `ESP32S3.bin` | `esp32s3` |
| `ESP32C3.bin` | `esp32c3` |
| `ESP32P4.bin` | `esp32p4` |

Each target also has a `.zip` containing the three separate images plus a
`FLA.txt` with the same instructions.

```sh
# 1. flash the merged image at 0x0
esptool.py --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x0 ESP32S3.bin

# 2. watch it, 115200 baud 8N1
minicom -D /dev/ttyUSB0 -b 115200
```

If it cannot connect, hold `BOOT`, tap `RESET`, release `BOOT`. The port may be
`/dev/ttyACM*` rather than `/dev/ttyUSB*`, depending on the USB bridge on the
board.

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| Boot loop: `quad_psram: PSRAM chip is not connected, or wrong PSRAM line mode` then `abort() was called` | PSRAM line mode does not match the module. `R8`/`N8R8` PSRAM is octal: rebuild with `CONFIG_SPIRAM_MODE_OCT=y`. With `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` the build boots anyway and says so on the startup screen. |
| No PSRAM row on the startup screen | Either the line mode is wrong, or `CONFIG_SPIRAM_USE_MALLOC` is off. The banner names which. |
| Boot log says `SPI Flash Size : 2MB` on a 16 MB board | Image built for the wrong flash size: set `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` and merge with `--flash_size 16MB`. |
| Nothing on screen | Wrong port or baud rate; the console is 115200 8N1. |
| Native USB serial disappears while the board reboots | ChromeOS drops the port when the device re-enumerates; share it with Linux again. |

## Keys

| Key | Action |
| --- | --- |
| `p` | pause / resume |
| `s` or `n` | skip the running test and continue with the next one |
| `q` | stop after the current step and print the report |
| `r` | run the whole suite again (during a run) |
| `h` | toggle the help screen |
| `u` | switch between the ANSI screen and plain text output |
| up / down | scroll the result list |

When a run finishes the banner screen stays up with a single verdict line
under it, for example:

```
 RESULT     PASS - 10 test(s), 79.2 MiB verified in 12.44 s, 0 error(s)
```

Nothing is cleared and nothing is printed again at that point. Press `r`,
space or enter to print the full report, or `q` to leave the tester on the
banner.

With `CONFIG_MEMTEST_AUTORUN=n` a small text menu is shown after boot where
the regions (`r`), the pattern set (`t`) and the pass count (`p`) can be
chosen. Any key enters the current selection (`s`), `q` exits.

## Reading the report

The report is printed when you ask for it after a run, and stays in the
terminal scrollback:

```
+------------------------------------------------------------------------------+
| esp32-memtest 1.0.0 - report                                                  |
+------------------------------------------------------------------------------+
| chip        ESP32-S3 rev0 at 240 MHz, 2 cores, ESP-IDF v5.5.5                |
| region      PSRAM (SPI)          8.00 MiB total, 7.00 MiB tested in 30 block(s), 87% covered |
|     PATTERN              PASS   TIME      THROUGHPUT  ERRORS     RESULT         |
|     moving inversions    1/1   130.10s   55.7MB/s     0          PASS           |
|     checker              1/1    4.90s    1440.2MB/s   0          PASS           |
|     first failing cells:                                                    |
|       #   address    offset    expected  actual    xor   bits  block          |
|       1   0x3fc8b120 0x0012a0 0x12345678 0x12345600 0x00000078 3   4/256KiB transient |
| totals      9 test(s), 60.0 MiB verified in 148.21 s, 3 error(s)              |
| RESULT      FAIL - memory faults detected                                    |
+------------------------------------------------------------------------------+
```

* `xor` is the difference between expected and actual, `bits` the number of
  cells whose bits differ. `transient` means the cell failed once but read
  correctly on the confirming pass, `persistent` means it failed again.
* The block map above each table shows which parts of the region were
  actually claimed (`#` tested, `.` not available).
* The coverage percentage is the number to look at first: a part can pass
  trivially if only a small corner of it was ever written. A single pass of
  `full` over an 8 MiB PSRAM covers about 87%, because the tester keeps a
  reserve of internal RAM for the console and the allocator.
* `STOPPED` instead of `PASS`/`FAIL` means the test was cut short by a key or
  by the pass count, not that it passed.

## Continuous integration

| Workflow | Runs on | What it does |
| --- | --- | --- |
| `ci.yml` | every push and pull request | host tests, takes seconds |
| `firmware.yml` | manually | builds all four targets |
| `build-release.yml` | manually | builds all four and attaches the images to a GitHub release |

Only the host tests run on your commits. The firmware jobs each install a
complete ESP-IDF toolchain and compile it, which takes several minutes per
target, so a maintainer starts them deliberately rather than letting them run
on every push. Contributors do not need to do anything here: opening a pull
request runs the host tests automatically, and building the firmware and
publishing releases is left to a maintainer.

Release notes are generated rather than written by hand.
`packaging/release_notes.py` finds the highest existing `v*` tag, takes the
commits between it and `HEAD`, groups them by type, and inserts a new section
into `CHANGELOG.md`. It is also useful on its own for previewing what a release
would say:

```sh
python3 packaging/release_notes.py --version v1.1.0 --out notes.md
```

Commits are classified from their prefixes, so `feat:`, `fix:` and `docs:` land
in the right group, and plain sentences such as `Fix the PSRAM detection` are
recognised too.

## Testing without a board

The engine and the terminal renderer are plain C11 with no ESP-IDF
dependency, so they are tested on the build machine:

```sh
make -C tests/host all     # engine + screen layout + ANSI replay
make -C tests/host asan    # engine under ASan/UBSan
make -C tests/host render-keep   # keep the captured ANSI stream for inspection
```

* `test_core.c` covers the patterns, the pool allocation against simulated
  fragmented heaps, the test runner's statistics and abort handling, and the
  test-set parser.
* `test_ui.c` renders the screen and checks that every line is exactly 80
  columns.
* `render_demo.c` feeds realistic states through the real renderer, replays
  the resulting escape sequences through a small terminal emulator and
  verifies that no frame overflows 80 columns, no frame scrolls the terminal
  and every frame fills exactly the visible screen.

CI builds the firmware for `esp32`, `esp32s3`, `esp32c3` and `esp32p4` and
runs the host tests.

## Project layout

```
main/                     application: menus, run loop, report, menuconfig
components/memtest_core/  portable engine: patterns, pool, runner, presets
components/memtest_esp/   chip, heap, timer and console abstraction
components/memtest_ui/    80-column ANSI screen and plain text renderer
tests/host/               host test suites and the screen replay harness
```

`memtest_core` knows nothing about ESP-IDF and is compiled for the host
directly; `memtest_esp` is the only component that includes ESP-IDF headers.

## Limitations

* Only the memory the heap can hand out is tested. Memory reserved for Wi-Fi,
  the PSRAM DMA threshold or the IDF runtime is not reachable and is not
  tested - the "FREE" and "TESTED" columns show what that costs you.
* One pass of `full` does not cover 100% of a region. The tester holds back
  `CONFIG_MEMTEST_RESERVE_KB` of internal RAM so the console keeps working,
  and the heap allocator will not hand out its last fragment. The report
  states the coverage percentage per region; read it before trusting a PASS.
* Testing is destructive: the tested memory is overwritten with patterns. Do
  not run this in an application that keeps data in RAM.
* A test that "passes" proves the cells held a value for the duration of the
  run. Marginally bad modules often need heat, voltage or thousands of
  passes; use `CONFIG_MEMTEST_PASSES=0` and let it run for hours.
* No display, touch, GPIO, Wi-Fi or SD support, by design. If you want a
  graphical UI, this firmware is not it.

## License

MIT, see [LICENSE](LICENSE).

