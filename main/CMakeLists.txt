set(LYRIC_FONT "${CMAKE_CURRENT_BINARY_DIR}/lyric_font.bin")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(LYRIC_FONT_INPUTS
        "${CMAKE_CURRENT_LIST_DIR}/../tools/make_lyric_font.py"
        "${ARDUINO_GFX_PATH}/src/font/u8g2_font_unifont_h_cjk.h"
        "${ARDUINO_GFX_PATH}/src/font/u8g2_font_unifont_h_chinese4.h")
    idf_build_get_property(python PYTHON)
    execute_process(COMMAND ${python} ${LYRIC_FONT_INPUTS} "${LYRIC_FONT}" --stats
        RESULT_VARIABLE LYRIC_FONT_RESULT OUTPUT_VARIABLE LYRIC_FONT_STATS ERROR_VARIABLE LYRIC_FONT_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT LYRIC_FONT_RESULT EQUAL 0)
        message(FATAL_ERROR "lyric font generation failed: ${LYRIC_FONT_ERROR}")
    endif()
    message(STATUS "wireTFT lyric font: ${LYRIC_FONT_STATS}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LYRIC_FONT_INPUTS})
endif()

idf_component_register(SRCS "main.cpp" "../wifi_bridge.c" "../lyric_font.cpp"
    INCLUDE_DIRS ".."
    REQUIRES arduino panel esp_wifi esp_netif esp_event nvs_flash
             esp_hw_support efuse esp_http_server spiffs lwip esp_rom bt
    EMBED_FILES "${LYRIC_FONT}")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lwip_bind")
