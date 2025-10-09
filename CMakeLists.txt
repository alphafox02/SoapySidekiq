########################################################################
# Build Soapy SDR support module for Sidekiq Devices
########################################################################
cmake_minimum_required(VERSION 2.8.12...3.10)
project(SoapySidekiq CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_COMPILER g++)

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
    SOURCES
        SoapySidekiq.hpp
        Registation.cpp
        Settings.cpp
        Streaming.cpp
        Sensor.cpp
    LIBRARIES
        ${Sidekiq_LIBRARIES}
        ${OTHER_LIBS}
	${PKGCONFIG_LIBS}
        /usr/lib/epiq/libusb-1.0.so
        /usr/lib/epiq/libglib-2.0.so
        -lrt
        /usr/lib/epiq/libtirpc.so

)
