## Flashing

Pick your chip, then flash either the merged image or the three separate ones.

**Merged image** — one file, everything at offset `0x0`:

```sh
esptool.py --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x0 ESP32S3.bin
minicom -D /dev/ttyUSB0 -b 115200
```

**Zip** — the separate images, if you prefer explicit offsets:

```sh
unzip ESP32S3.zip
esptool.py --chip esp32s3 -p /dev/ttyUSB0 write_flash \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 ESP32S3.bin
```

Watch the tester at 115200 baud, 8N1. The port may be `/dev/ttyACM*` rather
than `/dev/ttyUSB*`, depending on the USB bridge fitted to your board. If it
cannot connect, hold `BOOT`, tap `RESET`, release `BOOT`.

On the finished screen: `r` runs the suite again, `Enter` prints the full
report, `q` exits. Every zip also contains a `FLA.txt` with these commands.

## Downloads

| Chip | Merged image | Zip | `--chip` |
| --- | --- | --- | --- |
| ESP32 | [ESP32.bin](ESP32.bin) | [ESP32.zip](ESP32.zip) | `esp32` |
| ESP32S3 | [ESP32S3.bin](ESP32S3.bin) | [ESP32S3.zip](ESP32S3.zip) | `esp32s3` |
| ESP32C3 | [ESP32C3.bin](ESP32C3.bin) | [ESP32C3.zip](ESP32C3.zip) | `esp32c3` |
| ESP32P4 | [ESP32P4.bin](ESP32P4.bin) | [ESP32P4.zip](ESP32P4.zip) | `esp32p4` |

Each zip holds `bootloader.bin`, `partition-table.bin`, `<CHIP>.bin` and
`FLA.txt`. The merged `.bin` already contains all three, and takes its flash
mode, frequency and size from the bootloader, so one command is enough.