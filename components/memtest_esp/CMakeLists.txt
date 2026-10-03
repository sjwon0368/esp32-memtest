idf_component_register(
    SRCS "mt_board.c"
    INCLUDE_DIRS "include"
    REQUIRES memtest_core esp_psram esp_hw_support esp_timer driver esp_driver_uart spi_flash
)