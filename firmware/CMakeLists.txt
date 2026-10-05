# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.5.1")
add_definitions(-DFIRMWARE_VERSION=\"${PROJECT_VER}\")
# The Embody Mode release (release.sh passes -D EMBODY_VERSION=v0.3.0): what the robot reports as
# its firmware over USB and to its servers. Unset in own builds: they report PROJECT_VER.
if(EMBODY_VERSION)
    add_definitions(-DEMBODY_VERSION=\"${EMBODY_VERSION}\")
endif()

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Auto-load a local sdkconfig overlay (git-ignored) when present. Lets
# self-hosted / custom deployments pin CONFIG_STACKCHAN_SERVER_URL,
# CONFIG_OTA_URL, etc. without touching committed defaults.
# Manual equivalent: SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" idf.py build
# Never for a release build (-D STACKCHAN_RELEASE=1, ./container.sh release): the local overlay
# holds a token, and a release image is published.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local" AND NOT STACKCHAN_RELEASE)
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local"
        CACHE STRING "ESP-IDF sdkconfig default files (auto-extended with local overlay)" FORCE)
    message(STATUS "StackChan: detected sdkconfig.defaults.local, applying overlay")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stack-chan)
