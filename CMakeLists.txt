########################################################################
# Build Soapy SDR support module for Sidekiq Devices
########################################################################
cmake_minimum_required(VERSION 3.10)

if (POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

project(SoapySidekiq VERSION 2.0.0 LANGUAGES CXX)

set(SOAPYSIDEKIQ_VERSION "${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)

message(STATUS "SoapySidekiq version - ${SOAPYSIDEKIQ_VERSION}")

find_package(SoapySDR "0.4.0" NO_MODULE REQUIRED)
if (NOT SoapySDR_FOUND)
    message(FATAL_ERROR "Soapy SDR development files not found...")
endif ()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Sidekiq)

if (NOT Sidekiq_FOUND)
    message(FATAL_ERROR "Sidekiq development files not found...")
endif ()
message(STATUS "Sidekiq_INCLUDE_DIRS - ${Sidekiq_INCLUDE_DIRS}")
message(STATUS "Sidekiq_LIBRARIES - ${Sidekiq_LIBRARIES}")
message(STATUS "OTHER_LIBS - ${OTHER_LIBS}")
message(STATUS "PKGCONFIG_LIBS - ${PKGCONFIG_LIBS}")
message(STATUS "Sidekiq_RUNTIME_LIBRARIES - ${Sidekiq_RUNTIME_LIBRARIES}")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${Sidekiq_INCLUDE_DIRS})

#enable c++11 features
if(CMAKE_COMPILER_IS_GNUCXX)

    #Thread support enabled (not the same as -lpthread)
    list(APPEND Sidekiq_LIBRARIES -pthread)

    #disable warnings for unused parameters
    add_definitions(-Wno-unused-parameter)

endif(CMAKE_COMPILER_IS_GNUCXX)

if (APPLE)
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wc++11-extensions")
endif(APPLE)

set(OTHER_LIBS "" CACHE STRING "Other libraries")

SOAPY_SDR_MODULE_UTIL(
    TARGET SidekiqSupport
    VERSION ${SOAPYSIDEKIQ_VERSION}
    SOURCES
        SoapySidekiq.hpp
        Registation.cpp
        Settings.cpp
        Streaming.cpp
        Sensor.cpp
    LIBRARIES
        ${Sidekiq_LIBRARIES}
        ${CMAKE_DL_LIBS}
        ${OTHER_LIBS}
        ${PKGCONFIG_LIBS}
        ${Sidekiq_RUNTIME_LIBRARIES}

)

include(CTest)

option(SOAPYSIDEKIQ_BUILD_TESTS "Build SoapySidekiq test utilities" ON)
option(SOAPYSIDEKIQ_ENABLE_HARDWARE_TESTS
       "Register hardware smoke tests with CTest. These require attached Sidekiq hardware."
       OFF)

if (SOAPYSIDEKIQ_BUILD_TESTS)
    foreach(test_app
            antenna_smoke
            range_smoke
            rx_multi_cs16_validate
            rx_multicard_cs16_validate)
        add_executable(${test_app} tests/${test_app}.cpp)
        target_link_libraries(${test_app} SoapySDR)
    endforeach()

    if (SOAPYSIDEKIQ_ENABLE_HARDWARE_TESTS)
        add_test(NAME range_smoke COMMAND range_smoke)
        add_test(NAME antenna_smoke COMMAND antenna_smoke)
        add_test(NAME rx_multi_cs16_validate COMMAND rx_multi_cs16_validate)
        add_test(NAME rx_multicard_cs16_validate
                 COMMAND rx_multicard_cs16_validate)
        set_tests_properties(range_smoke
                             antenna_smoke
                             rx_multi_cs16_validate
                             rx_multicard_cs16_validate
                             PROPERTIES LABELS "hardware")
    endif()
endif()
