#!/usr/bin/env bash
#
# Flash esp32-memtest and watch it, on a board whose USB port ChromeOS
# forwards into the Linux container (Crostini).  No ESP-IDF install needed on
# the laptop: the container already has the toolchain.
#
#   ./flash.sh                 find the port, flash, then start the monitor
#   ./flash.sh /dev/ttyUSB0    same, with an explicit port
#   ./flash.sh --flash-only    skip the monitor
#
# The merged image contains bootloader, partition table and application, so
# everything goes to offset 0x0.
set -euo pipefail

cd "$(dirname "$0")"

PORT=""
FLASH_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --flash-only) FLASH_ONLY=1 ;;
        /dev/*)       PORT="$arg" ;;
        *)            echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [ ! -f dist/esp32-memtest-merged.bin ]; then
    echo "dist/esp32-memtest-merged.bin is missing - build it first:" >&2
    echo "  . \$IDF_PATH/export.sh && idf.py -DIDF_TARGET=esp32s3 build" >&2
    echo "  esptool.py --chip esp32s3 merge_bin -o dist/esp32-memtest-merged.bin \\" >&2
    echo "      --flash_mode dio --flash_freq 80m --flash_size 16MB \\" >&2
    echo "      0x0 build/bootloader/bootloader.bin \\" >&2
    echo "      0x8000 build/partition_table/partition-table.bin \\" >&2
    echo "      0x10000 build/esp32-memtest.bin" >&2
    exit 1
fi

# The board renumbers itself after a reset, so do not trust a remembered name.
if [ -z "$PORT" ]; then
    for candidate in /dev/ttyACM* /dev/ttyUSB*; do
        if [ -e "$candidate" ]; then
            PORT="$candidate"
            break
        fi
    done
fi
if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
    echo "no serial port found." >&2
    echo "In ChromeOS: connect the board, then use the USB device in the" >&2
    echo "Crostini tray/panel to share it with Linux, and try again." >&2
    exit 1
fi

# shellcheck disable=SC1091
. "${IDF_PATH:-/opt/esp-idf}/export.sh" >/dev/null

echo "==> port     $PORT"
echo "==> image    dist/esp32-memtest-merged.bin ($(stat -c %s dist/esp32-memtest-merged.bin) bytes)"
echo "==> flashing at 0x0"
esptool.py --chip esp32s3 -p "$PORT" write_flash 0x0 dist/esp32-memtest-merged.bin

if [ "$FLASH_ONLY" -eq 1 ]; then
    exit 0
fi

# A board on its native USB port re-enumerates after the reset that flashing
# triggers, and it may come back as ttyACM1 instead of ttyACM0.
MON_PORT="$PORT"
for _ in $(seq 1 20); do
    for candidate in /dev/ttyACM* /dev/ttyUSB*; do
        if [ -e "$candidate" ]; then
            MON_PORT="$candidate"
            break
        fi
    done
    [ -n "$MON_PORT" ] && break
    sleep 0.5
done
PORT="$MON_PORT"

echo "==> monitoring on $PORT at 115200 (Ctrl-] to leave idf.py, Ctrl-A X in minicom)"
echo "    if the screen stays empty: press RESET on the board once."
if command -v minicom >/dev/null 2>&1; then
    minicom -D "$PORT" -b 115200
else
    idf.py -p "$PORT" monitor
fi