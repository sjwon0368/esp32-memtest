idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES memtest_core memtest_esp memtest_ui esp_app_format
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MEMTEST_VERSION="${PROJECT_VER}"
)
