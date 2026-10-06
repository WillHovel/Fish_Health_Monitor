# =============================================================================
# aquatic_biomonitor_fw — CMakeLists.txt
#
# Zephyr application build script for the Fish ECG / Ventilation Monitor.
# Targets: nRF52840 DK (nrf52840dk_nrf52840)
#
# Required Zephyr version : >= 3.0.0
# nRF Connect SDK version  : >= 2.4.0
#
# Build (west):
#   west build -b nrf52840dk_nrf52840 -- -DCONF_FILE=prj.conf
# Flash:
#   west flash
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

# ---------------------------------------------------------------------------
# Locate the Zephyr CMake package.  The ZEPHYR_BASE environment variable must
# be set (handled automatically by a west workspace).
# ---------------------------------------------------------------------------
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(aquatic_biomonitor_fw
    VERSION 1.0.0
    DESCRIPTION "Fish ECG/Ventilation biometric monitor — Zephyr RTOS firmware"
    LANGUAGES C
)

# ---------------------------------------------------------------------------
# Application sources
# All .c files live under src/.  Add additional translation units here.
# ---------------------------------------------------------------------------
target_sources(app
    PRIVATE
        src/main.c
)

# ---------------------------------------------------------------------------
# CMSIS-DSP
#
# Zephyr ships CMSIS-DSP as a module (CONFIG_CMSIS_DSP=y in prj.conf).
# The Zephyr build system automatically links the library when the Kconfig
# symbol is enabled; no manual target_link_libraries() call is needed.
#
# If you are using an out-of-tree CMSIS-DSP, uncomment the block below and
# set CMSIS_DSP_ROOT to the absolute path of the CMSIS-DSP source tree:
#
# set(CMSIS_DSP_ROOT $ENV{CMSIS_DSP_ROOT})
# include(${CMSIS_DSP_ROOT}/Source/CMakeLists.txt)
# target_link_libraries(app PRIVATE CMSISDSP)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Compiler feature flags
#
# -mfpu=fpv4-sp-d16  : Cortex-M4F single-precision FPU
# -mfloat-abi=hard   : Use hardware FP registers for function arguments
# These flags are normally set by Zephyr when CONFIG_FPU=y, but we make them
# explicit here for clarity and to guard against toolchain misconfiguration.
# ---------------------------------------------------------------------------
target_compile_options(app
    PRIVATE
        -mfpu=fpv4-sp-d16
        -mfloat-abi=hard
)

# ---------------------------------------------------------------------------
# Include directories
# Zephyr's own include paths are injected automatically.  Add project-local
# headers below when the project grows (e.g., ble_service.h, dsp_filters.h).
# ---------------------------------------------------------------------------
target_include_directories(app
    PRIVATE
        src
)
