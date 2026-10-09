cmake_minimum_required(VERSION 3.22)

# Dura M3000 PCB832 revX3 target: ESP32-S3-WROOM-1-N8R2.
# Fitted module: 8 MB flash / 2 MB PSRAM; PSRAM remains disabled by policy.
# The CAD N4R2 entry was selected for its footprint, not fitted flash capacity.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dura_m3000_firmware)
