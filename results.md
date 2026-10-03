I (668) heap_init: Initializing. RAM available for dynamic allocation:
I (674) heap_init: At 3FC9D788 len 0004BF88 (303 KiB): RAM
I (680) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM
I (685) heap_init: At 3FCF0000 len 00008000 (32 KiB): DRAM
I (690) heap_init: At 600FE000 len 00001FE8 (7 KiB): RTCRAM
I (695) esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator
I (703) spi_flash: detected chip: generic
I (705) spi_flash: flash io: dio
I (708) sleep_gpio: Configure to isolate all GPIO pins in sleep state
I (715) sleep_gpio: Enable automatic switching of GPIO sleep configuration
I (722) main_task: Started on CPU0
I (725) esp_psram: Reserving pool of 96K of internal memory for DMA/internal allocations
I (733) main_task: Calling app_main()
+------------------------------------------------------------------------------+
 esp32-memtest 1.0.0 - report
+------------------------------------------------------------------------------+
 chip        ESP32-S3 rev2 at 160 MHz, 2 cores, ESP-IDF v5.5.5
 region      PSRAM (SPI)          8.00 MiB total, 7.87 MiB tested in 32 block(s), 98% covered
     PATTERN            PASS   TIME      THROUGHPUT  ERRORS    RESULT
     Zero fill          1/1        1.85s 8.5MB/s     0         PASS
     Ones fill          1/1        1.87s 8.4MB/s     0         PASS
     Byte alternation   1/1        2.52s 6.3MB/s     0         PASS
     Address test       1/1        3.51s 4.5MB/s     0         PASS
     Pseudo random      1/1        3.91s 4.0MB/s     0         PASS
     Moving inversions  1/1        2.45s 6.4MB/s     0         PASS
     Walking bit        1/1        2.53s 6.2MB/s     0         PASS
     Walking bit ones   1/1       79.20s 6.4MB/s     0         PASS
     Walking bit zeros  1/1       79.41s 6.3MB/s     0         PASS
     Modulo X           1/1      161.74s 6.2MB/s     0         PASS
+------------------------------------------------------------------------------+
 region      Internal DRAM         462 KiB total, 241 KiB tested in 3 block(s), 52% covered
     PATTERN            PASS   TIME      THROUGHPUT  ERRORS    RESULT
+------------------------------------------------------------------------------+
 totals      10 test(s), 1.04 GiB verified in 338.98 s, 0 error(s)
 RESULT     PASS - no memory faults detected
+------------------------------------------------------------------------------+
I (383938) main_task: Returned from app_main()
