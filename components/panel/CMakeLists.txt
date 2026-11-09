if(CMAKE_BUILD_EARLY_EXPANSION)
     idf_component_register(REQUIRES arduino)
     return()
endif()
if(NOT IDF_TARGET STREQUAL "esp32")
     message(FATAL_ERROR "wireTFT panel compatibility is restricted to classic ESP32")
endif()
file(READ "${ARDUINO_GFX_PATH}/src/databus/Arduino_ESP32SPI.cpp" SPI_SOURCE)
string(REPLACE "spiFrequencyToClockDiv(old_apb /" "spiFrequencyToClockDiv(_spi, old_apb /" SPI_SOURCE "${SPI_SOURCE}")
string(REPLACE "spiFrequencyToClockDiv(_speed)" "spiFrequencyToClockDiv(&_spi_bus_array[_spi_num], _speed)" SPI_SOURCE "${SPI_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/Arduino_ESP32SPI_compat.cpp" "${SPI_SOURCE}")
idf_component_register(
    SRCS "${ARDUINO_GFX_PATH}/src/Arduino_DataBus.cpp"
         "${ARDUINO_GFX_PATH}/src/Arduino_G.cpp"
         "${ARDUINO_GFX_PATH}/src/Arduino_GFX.cpp"
         "${ARDUINO_GFX_PATH}/src/Arduino_TFT.cpp"
         "${CMAKE_CURRENT_BINARY_DIR}/Arduino_ESP32SPI_compat.cpp"
         "${ARDUINO_GFX_PATH}/src/display/Arduino_ST7789.cpp"
         "${ARDUINO_GFX_PATH}/src/canvas/Arduino_Canvas.cpp"
     INCLUDE_DIRS "include" "${ARDUINO_GFX_PATH}/src" "${ARDUINO_GFX_PATH}/src/databus"
    REQUIRES arduino)