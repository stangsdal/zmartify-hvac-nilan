cmake_minimum_required(VERSION 3.16)

# The generic gateway components are shared with the AHC9000 product. Override
# this path for another checkout with -DNILAN_AHC9000_PLATFORM_DIR=/path.
set(NILAN_AHC9000_PLATFORM_DIR "/Users/peter/zmartify-hvac-ahc9000" CACHE PATH
    "AHC9000 shared ESP-IDF platform checkout")
if(EXISTS "${NILAN_AHC9000_PLATFORM_DIR}/components")
    set(EXTRA_COMPONENT_DIRS
        "${NILAN_AHC9000_PLATFORM_DIR}/components/board"
        "${NILAN_AHC9000_PLATFORM_DIR}/components/comm_rs485"
        "${NILAN_AHC9000_PLATFORM_DIR}/components/net")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zmartify_hvac_nilan)
