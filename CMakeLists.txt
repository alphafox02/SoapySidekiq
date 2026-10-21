########################################################################
# Build Soapy SDR support module for Sidekiq Devices
#
# This file configures the production module and its Sidekiq SDK dependency.
########################################################################
cmake_minimum_required(VERSION 3.21)
project(SoapySidekiq LANGUAGES CXX)

option(SOAPYSIDEKIQ_BUILD_MODULE "Build the SDK-dependent SoapySDR module" ON)
option(SOAPYSIDEKIQ_BUILD_TESTS "Build the SDK-independent regression suite" OFF)

if(SOAPYSIDEKIQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/non_hardware)
endif()

if(NOT SOAPYSIDEKIQ_BUILD_MODULE)
    return()
endif()

if(NOT DEFINED SIDEKIQ_SDK_DIR OR "${SIDEKIQ_SDK_DIR}" STREQUAL "")
    if(DEFINED ENV{SIDEKIQ_SDK_DIR} AND NOT "$ENV{SIDEKIQ_SDK_DIR}" STREQUAL "")
        set(SIDEKIQ_SDK_DIR "$ENV{SIDEKIQ_SDK_DIR}" CACHE PATH "Path to the Sidekiq SDK")
    else()
        set(SIDEKIQ_SDK_DIR "$ENV{HOME}/sidekiq_sdk_current" CACHE PATH "Path to the Sidekiq SDK")
    endif()
endif()

find_package(SoapySDR "0.4.0" NO_MODULE REQUIRED)

get_filename_component(_SoapySDR_ROOT "${SoapySDR_INCLUDE_DIRS}/.." ABSOLUTE)
message(STATUS "SoapySDR root directory: ${_SoapySDR_ROOT}")
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT "${_SoapySDR_ROOT}" STREQUAL "/usr")
    set_property(CACHE CMAKE_INSTALL_PREFIX PROPERTY VALUE "${_SoapySDR_ROOT}")
endif()

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
find_package(Sidekiq 4.26.0)

if (NOT Sidekiq_FOUND)
    message(FATAL_ERROR
        "Sidekiq SDK v4.26 or later not found.  "
        "Location can be specified with:"
        "  -DSIDEKIQ_SDK_DIR=/path/to/sidekiq_sdk_current")
endif ()

message(STATUS "SIDEKIQ_SDK_DIR - ${SIDEKIQ_SDK_DIR}")
message(STATUS "Sidekiq_INCLUDE_DIRS - ${Sidekiq_INCLUDE_DIRS}")
message(STATUS "Sidekiq_LIBRARIES - ${Sidekiq_LIBRARIES}")
message(STATUS "Sidekiq_BUILD_CONFIG - ${Sidekiq_BUILD_CONFIG}")
message(STATUS "Sidekiq_PKG_LIBRARY_DIRS - ${Sidekiq_PKG_LIBRARY_DIRS}")

SOAPY_SDR_MODULE_UTIL(
    TARGET SidekiqSupport
    SOURCES
        SoapySidekiq.hpp
        DeviceDiscovery.cpp
        DeviceOptions.cpp
        SdkCardManager.cpp
        SidekiqCardManager.cpp
        SensorReader.cpp
        SidekiqSensorBackend.cpp
        StreamConfiguration.cpp
        # Thread-safe hardware-to-Soapy RX sample handoff.
        RxSampleQueue.cpp
        RxStreamSession.cpp
        SidekiqRxBackend.cpp
        Registration.cpp
        Settings.cpp
        TimeClock.cpp
        RFControls.cpp
        Streaming.cpp
        Sensor.cpp
    LIBRARIES
        Sidekiq::sidekiq

)

target_compile_features(SidekiqSupport PRIVATE cxx_std_17)
if(NOT CMAKE_CXX_STANDARD)
    # CMake 3.21 needs an explicit standard to disable GCC's default gnu++17.
    set_property(TARGET SidekiqSupport PROPERTY CXX_STANDARD 17)
endif()
set_target_properties(SidekiqSupport PROPERTIES
    CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(SidekiqSupport PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(SidekiqSupport SYSTEM PRIVATE ${SoapySDR_INCLUDE_DIRS})

# SoapySDR's older module helper adds this only for GCC. Require resolved
# symbols for Clang modules too, so broken SDK linkage fails during the build.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND UNIX)
    if(APPLE)
        target_link_options(SidekiqSupport PRIVATE "LINKER:-undefined,error")
    else()
        target_link_options(SidekiqSupport PRIVATE "LINKER:--no-undefined")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProjectWarnings.cmake")
soapy_sidekiq_enable_warnings(SidekiqSupport PRIVATE)

# Remove only files recorded by this build's install step; support DESTDIR.
add_custom_target(uninstall
    COMMAND "${CMAKE_COMMAND}"
        "-DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Uninstall.cmake"
    VERBATIM)
