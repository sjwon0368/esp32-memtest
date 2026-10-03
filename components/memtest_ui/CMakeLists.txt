idf_component_register(
    SRCS "mt_screen.c"
    INCLUDE_DIRS "include"
    REQUIRES memtest_core
)

if(CONFIG_MEMTEST_UI_WIDTH)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC MT_UI_WIDTH=${CONFIG_MEMTEST_UI_WIDTH})
endif()
