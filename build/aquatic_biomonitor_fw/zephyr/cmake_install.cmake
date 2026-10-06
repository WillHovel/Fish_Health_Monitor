# Install script for directory: C:/ncs/v3.4.1/zephyr

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/Zephyr-Kernel")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/ncs/toolchains/4f5b6ad6dd/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-objdump.exe")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/arch/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/lib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/soc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/boards/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/subsys/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/drivers/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/nrf/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/mcuboot/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/mbedtls/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/trusted-firmware-m/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/cjson/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/azure-sdk-for-c/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/cirrus-logic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/openthread/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/nrf_wifi/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/hal_nordic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/memfault-firmware-sdk/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/hostap/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/canopennode/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/chre/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/cmsis/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/cmsis-dsp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/cmsis-nn/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/cmsis_6/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/dhara/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/fatfs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/hal_st/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/hal_tdk/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/hal_wurthelektronik/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/liblc3/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/libmetal/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/libsbc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/littlefs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/loramac-node/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/lvgl/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/mipi-sys-t/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/nanopb/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/open-amp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/percepio/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/picolibc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/segger/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/uoscore-uedhoc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/zcbor/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/nrfxlib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/nrf_hw_models/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/modules/connectedhomeip/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/kernel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/cmake/flash/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/cmake/usage/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/cmake/reports/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/Users/willh/Desktop/Fish_monitor/aquatic_biomonitor_fw/build/aquatic_biomonitor_fw/zephyr/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
