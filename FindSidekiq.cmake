# - Try to find Sidekiq
#
# Uses the SDK's sidekiq-config tool when it is available (libsidekiq v4.26.0
# and later).  Older SDKs, and installs without sidekiq-config, fall back to
# locating the library for the host platform directly.
#
# Inputs (cache variables or environment variables):
#  SIDEKIQ_SDK_DIR            - Sidekiq SDK root (default: ~/sidekiq_sdk_current)
#  Sidekiq_DIR (environment)  - older name for the SDK root, still honored
#  SIDEKIQ_USE_SIDEKIQ_CONFIG - set OFF to skip sidekiq-config (default: ON)
#  SUFFIX / PLATFORM          - legacy detection only: SDK platform suffix
#
# Once done this will define
#  Sidekiq_FOUND - System has Sidekiq
#  Sidekiq_LIBRARIES - The Sidekiq libraries (an imported target with sidekiq-config)
#  Sidekiq_INCLUDE_DIRS - The Sidekiq include directories
#  Sidekiq_PKG_LIBRARY_DIRS - The Sidekiq support library directory
#  Sidekiq_BUILD_CONFIG - The SDK build config (sidekiq-config builds only)
#  Sidekiq_VERSION - The SDK version (sidekiq-config builds only)
#  Sidekiq_RUNTIME_LIBRARIES, OTHER_LIBS, PKGCONFIG_LIBS - extra link libraries
#      needed by the legacy detection (empty with sidekiq-config)

if(NOT Sidekiq_FOUND)

    if(DEFINED SIDEKIQ_SDK_DIR AND NOT "${SIDEKIQ_SDK_DIR}" STREQUAL "")
        set(_Sidekiq_SDK_DIR_HINT "${SIDEKIQ_SDK_DIR}")
    elseif(DEFINED ENV{SIDEKIQ_SDK_DIR} AND NOT "$ENV{SIDEKIQ_SDK_DIR}" STREQUAL "")
        set(_Sidekiq_SDK_DIR_HINT "$ENV{SIDEKIQ_SDK_DIR}")
    elseif(DEFINED ENV{Sidekiq_DIR} AND NOT "$ENV{Sidekiq_DIR}" STREQUAL "")
        set(_Sidekiq_SDK_DIR_HINT "$ENV{Sidekiq_DIR}")
    else()
        set(_Sidekiq_SDK_DIR_HINT "$ENV{HOME}/sidekiq_sdk_current")
    endif()
    get_filename_component(_Sidekiq_SDK_DIR_HINT "${_Sidekiq_SDK_DIR_HINT}" ABSOLUTE)

    option(SIDEKIQ_USE_SIDEKIQ_CONFIG
           "Use the SDK's sidekiq-config tool when it is available" ON)

    if(SIDEKIQ_USE_SIDEKIQ_CONFIG)
        find_program(Sidekiq_CONFIG_EXECUTABLE
            NAMES sidekiq-config
            HINTS "${_Sidekiq_SDK_DIR_HINT}/bin"
            NO_DEFAULT_PATH)
    endif()

    if(Sidekiq_CONFIG_EXECUTABLE)
        message(STATUS "Using ${Sidekiq_CONFIG_EXECUTABLE}")

        function(_sidekiq_config _out_var _flag)
            execute_process(
                COMMAND "${Sidekiq_CONFIG_EXECUTABLE}" "${_flag}"
                RESULT_VARIABLE _sidekiq_config_result
                OUTPUT_VARIABLE _sidekiq_config_output
                ERROR_VARIABLE _sidekiq_config_error
                OUTPUT_STRIP_TRAILING_WHITESPACE)

            if(NOT _sidekiq_config_result EQUAL 0)
                message(FATAL_ERROR
                    "Failed to run ${Sidekiq_CONFIG_EXECUTABLE} ${_flag}: "
                    "${_sidekiq_config_error}")
            endif()

            set(${_out_var} "${_sidekiq_config_output}" PARENT_SCOPE)
        endfunction()

        _sidekiq_config(Sidekiq_CFLAGS "--cflags")
        _sidekiq_config(Sidekiq_LINK_FLAGS "--libs-static")
        _sidekiq_config(Sidekiq_SDK_DIR "--prefix")
        _sidekiq_config(Sidekiq_PKG_LIBRARY_DIRS "--support-dir")
        _sidekiq_config(Sidekiq_BUILD_CONFIG "--build-config")
        _sidekiq_config(Sidekiq_VERSION "--version")

        set(SIDEKIQ_SDK_DIR "${Sidekiq_SDK_DIR}" CACHE PATH "Path to the Sidekiq SDK" FORCE)

        separate_arguments(Sidekiq_CFLAGS_LIST UNIX_COMMAND "${Sidekiq_CFLAGS}")
        separate_arguments(Sidekiq_LINK_LIBRARIES UNIX_COMMAND "${Sidekiq_LINK_FLAGS}")

        set(Sidekiq_INCLUDE_DIRS "")
        set(Sidekiq_COMPILE_OPTIONS "")
        foreach(_Sidekiq_CFLAG IN LISTS Sidekiq_CFLAGS_LIST)
            if("${_Sidekiq_CFLAG}" MATCHES "^-I(.+)")
                list(APPEND Sidekiq_INCLUDE_DIRS "${CMAKE_MATCH_1}")
            else()
                list(APPEND Sidekiq_COMPILE_OPTIONS "${_Sidekiq_CFLAG}")
            endif()
        endforeach()

        include(FindPackageHandleStandardArgs)
        find_package_handle_standard_args(Sidekiq
            REQUIRED_VARS
                Sidekiq_CONFIG_EXECUTABLE
                Sidekiq_INCLUDE_DIRS
                Sidekiq_LINK_LIBRARIES
            VERSION_VAR Sidekiq_VERSION)

        if(Sidekiq_FOUND AND NOT TARGET Sidekiq::sidekiq)
            add_library(Sidekiq::sidekiq INTERFACE IMPORTED)
            set_target_properties(Sidekiq::sidekiq PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${Sidekiq_INCLUDE_DIRS}"
                INTERFACE_LINK_LIBRARIES "${Sidekiq_LINK_LIBRARIES}")
            if(Sidekiq_COMPILE_OPTIONS)
                set_target_properties(Sidekiq::sidekiq PROPERTIES
                    INTERFACE_COMPILE_OPTIONS "${Sidekiq_COMPILE_OPTIONS}")
            endif()
        endif()

        set(Sidekiq_LIBRARIES Sidekiq::sidekiq)
        set(Sidekiq_RUNTIME_LIBRARIES "")
        set(OTHER_LIBS "")
        set(PKGCONFIG_LIBS "")

        mark_as_advanced(
            Sidekiq_CONFIG_EXECUTABLE
            Sidekiq_INCLUDE_DIRS
            Sidekiq_LIBRARIES
            Sidekiq_LINK_LIBRARIES
            Sidekiq_PKG_LIBRARY_DIRS
            OTHER_LIBS
            PKGCONFIG_LIBS)
    else()
        # Legacy detection for SDKs that do not ship sidekiq-config.
        message(STATUS "sidekiq-config not found in ${_Sidekiq_SDK_DIR_HINT}/bin; "
                       "using legacy Sidekiq SDK detection")

        set(Sidekiq_ROOT "${_Sidekiq_SDK_DIR_HINT}" CACHE PATH "Root of the Sidekiq SDK/runtime")
        set(SIDEKIQ_SDK_DIR "${Sidekiq_ROOT}")


        if(DEFINED PLATFORM AND NOT DEFINED SUFFIX)
            set(SUFFIX "${PLATFORM}")
        endif()


        find_path(Sidekiq_INCLUDE_DIR
                NAMES sidekiq_api.h
                HINTS
                    ${Sidekiq_PKG_INCLUDE_DIRS}
                    ${Sidekiq_ROOT}/sidekiq_core/inc
                    ${Sidekiq_ROOT}/include/sidekiq
                    ${Sidekiq_ROOT}/include
                    $ENV{Sidekiq_DIR}/sidekiq_core/inc
                    $ENV{Sidekiq_DIR}/include/sidekiq
                    $ENV{Sidekiq_DIR}/include
                PATHS
                    ~/sidekiq_sdk_current/sidekiq_core/inc/
                    /usr/local/include/sidekiq
                    /usr/local/include
                    /usr/include/sidekiq
                    /usr/include
                    /opt/include
                    /opt/local/include)

        execute_process (
            COMMAND uname -m
            OUTPUT_VARIABLE cpu_arch
        )

        string(STRIP "${cpu_arch}" cpu_arch)

        message(STATUS "cpu_arch is: '${cpu_arch}'")

        if(NOT DEFINED SUFFIX OR "${SUFFIX}" STREQUAL "")
            set(SUFFIX "none")
        endif()

        if (NOT ${cpu_arch} MATCHES "x86_64" AND ("${SUFFIX}" STREQUAL "none"))
            set(SDK_DIR "${Sidekiq_ROOT}/lib")
            file(GLOB LIB_FILES "${SDK_DIR}/libsidekiq__*.a")

            if(LIB_FILES)
                list(GET LIB_FILES 0 FOUND_LIB)
                get_filename_component(LIB_NAME "${FOUND_LIB}" NAME)
                string(REPLACE "libsidekiq__" "" SUFFIX_WITH_EXT "${LIB_NAME}")
                string(REPLACE ".a" "" SUFFIX "${SUFFIX_WITH_EXT}")
                message(STATUS "Detected SDK SUFFIX: ${SUFFIX}")
            endif()
        endif()


        if("${cpu_arch}" STREQUAL "x86_64")
            set (libname  "libsidekiq__x86_64.gcc.a")
            set (otherlib "none")
          elseif("${SUFFIX}" STREQUAL "msiq-x40")
            set(otherlib "none")
            set(libname  "libsidekiq__msiq-x40.a")
          elseif("${SUFFIX}" STREQUAL "msiq-g20g40")
            set(otherlib "none")
            set(libname  "libsidekiq__msiq-g20g40.a")
          elseif("${SUFFIX}" STREQUAL "z3u")
            set(otherlib "libiio")
            set(libname  "libsidekiq__z3u.a")
          elseif("${SUFFIX}" STREQUAL "aarch64")
            set (libname  "libsidekiq__aarch64.a")
            set (otherlib "iio")
          elseif("${SUFFIX}" STREQUAL "aarch64.gcc6.3")
            set (libname  "libsidekiq__aarch64.gcc6.3.a")
            set (otherlib "iio")
          elseif("${SUFFIX}" STREQUAL "arm_cortex-a9.gcc7.2.1_gnueabihf")
            set (libname  "libsidekiq__arm_cortex-a9.gcc7.2.1_gnueabihf.a")
            set (otherlib "iio")
        else()
          message(FATAL_ERROR "Invalid platform ${SUFFIX}")
        endif()

        message(STATUS "library is ${libname} ")
        message(STATUS "otherlib is ${otherlib} ")

        find_library(Sidekiq_LIBRARY
            NAMES
                ${libname}
                sidekiq
                libsidekiq.so
                libsidekiq.so.4.25.0
                libsidekiq.so.4.24.0
                libsidekiq.so.4.23.0
                libsidekiq.so.4.19.0
            HINTS
                ${Sidekiq_PKG_LIBRARY_DIRS}
                ${Sidekiq_ROOT}/lib
                ${Sidekiq_ROOT}/lib/support/${SUFFIX}/usr/lib/epiq
                $ENV{Sidekiq_DIR}/lib
                $ENV{Sidekiq_DIR}/lib/support/${SUFFIX}/usr/lib/epiq
            PATHS
                ~/sidekiq_sdk_current/lib/
                /usr/lib/epiq
                /usr/local/lib
                /usr/lib
                /opt/lib
                /opt/local/lib)


        #    find_library(Sidekiq_LIBRARY
        #    NAMES ${libname}
        #    HINTS ${Sidekiq_PKG_LIBRARY_DIRS} $ENV{Sidekiq_DIR}/include
        #    PATHS ~/sidekiq_sw)

        set(Sidekiq_LIBRARIES ${Sidekiq_LIBRARY})
        set(Sidekiq_INCLUDE_DIRS ${Sidekiq_INCLUDE_DIR})
        set(Sidekiq_PKG_LIBRARY_DIRS "${Sidekiq_ROOT}/lib/support/${SUFFIX}/usr/lib/epiq")
        set(ENV{Sidekiq_DIR} "${Sidekiq_ROOT}")

        set(Sidekiq_RUNTIME_LIBRARY_HINTS
            ${Sidekiq_PKG_LIBRARY_DIRS}
            ${Sidekiq_ROOT}/lib
            $ENV{Sidekiq_DIR}/lib
            $ENV{Sidekiq_DIR}/lib/support/${SUFFIX}/usr/lib/epiq)

        find_library(Sidekiq_USB_LIBRARY
            NAMES usb-1.0 libusb-1.0.so
            HINTS ${Sidekiq_RUNTIME_LIBRARY_HINTS}
            PATHS /usr/lib/epiq /usr/local/lib /usr/lib /opt/lib /opt/local/lib)

        find_library(Sidekiq_GLIB_LIBRARY
            NAMES glib-2.0 libglib-2.0.so
            HINTS ${Sidekiq_RUNTIME_LIBRARY_HINTS}
            PATHS /usr/lib/epiq /usr/local/lib /usr/lib /opt/lib /opt/local/lib)

        find_library(Sidekiq_TIRPC_LIBRARY
            NAMES tirpc libtirpc.so
            HINTS ${Sidekiq_RUNTIME_LIBRARY_HINTS}
            PATHS /usr/lib/epiq /usr/local/lib /usr/lib /opt/lib /opt/local/lib)

        find_library(Sidekiq_RT_LIBRARY
            NAMES rt librt.so
            PATHS /usr/local/lib /usr/lib /opt/lib /opt/local/lib)

        set(Sidekiq_RUNTIME_LIBRARIES "")
        foreach(runtime_lib
                Sidekiq_USB_LIBRARY
                Sidekiq_GLIB_LIBRARY
                Sidekiq_TIRPC_LIBRARY)
            if(${runtime_lib})
                list(APPEND Sidekiq_RUNTIME_LIBRARIES ${${runtime_lib}})
            else()
                message(WARNING
                        "${runtime_lib} was not found. The build may still work "
                        "with a shared libsidekiq, but static SDK builds usually "
                        "need the Epiq support libraries.")
            endif()
        endforeach()

        if(Sidekiq_RT_LIBRARY)
            list(APPEND Sidekiq_RUNTIME_LIBRARIES ${Sidekiq_RT_LIBRARY})
        else()
            list(APPEND Sidekiq_RUNTIME_LIBRARIES rt)
        endif()

        if("${cpu_arch}" STREQUAL "x86_64")
            message(STATUS "building for x86_64.gcc")
            include(FindPackageHandleStandardArgs)
            # handle the QUIETLY and REQUIRED arguments and set LibSidekiq_FOUND to TRUE
            # if all listed variables are TRUE
            find_package_handle_standard_args(Sidekiq  DEFAULT_MSG
                Sidekiq_LIBRARY Sidekiq_INCLUDE_DIR )

            set(OTHER_LIBS "")
            set(PKGCONFIG_LIBS "")
            mark_as_advanced(Sidekiq_INCLUDE_DIRS Sidekiq_LIBRARIES
                             Sidekiq_RUNTIME_LIBRARIES OTHER_LIBS PKGCONFIG_LIBS)
          elseif("${SUFFIX}" MATCHES "^(z3u|aarch64|aarch64\\.gcc6\\.3|arm_cortex-a9\\.gcc7\\.2\\.1_gnueabihf)$")
            message(STATUS "building for aarch")


            find_library(OTHER_LIBS
                NAMES ${otherlib}
                HINTS ${Sidekiq_PKG_LIBRARY_DIRS} $ENV{Sidekiq_DIR}/include
                PATHS /usr/lib/epiq/ /usr/local/lib /usr/lib /opt/lib /opt/local/lib)

            set(OTHER_LIBS ${OTHER_LIBS})

            include(FindPackageHandleStandardArgs)
            # handle the QUIETLY and REQUIRED arguments and set LibSidekiq_FOUND to TRUE
            # if all listed variables are TRUE
            find_package_handle_standard_args(Sidekiq  DEFAULT_MSG
                Sidekiq_LIBRARY Sidekiq_INCLUDE_DIR OTHER_LIBS)

            set(PKGCONFIG_LIBS "")

            mark_as_advanced(Sidekiq_INCLUDE_DIRS Sidekiq_LIBRARIES
                             Sidekiq_RUNTIME_LIBRARIES OTHER_LIBS PKGCONFIG_LIBS)
          elseif("${SUFFIX}" STREQUAL "msiq-x40")
            message(STATUS "building for x40")

            # Set the PKG_CONFIG_PATH from the SDK root
            set(ENV{PKG_CONFIG_PATH} "${Sidekiq_ROOT}/lib/support/msiq-x40/usr/lib/epiq/pkgconfig")

            message(STATUS "PKG_CONFIG_PATH $ENV{PKG_CONFIG_PATH}")

            execute_process(
                COMMAND pkg-config --libs-only-l grpc++ protobuf
                OUTPUT_VARIABLE PKG_LIBS
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )

            # Convert PKG_LIBS into a list
            string(REPLACE " " ";" PKG_LIBS_LIST ${PKG_LIBS})

            set (PKGCONFIG_LIBS ${PKG_LIBS_LIST} -lgpiod -lstdc++)
            message(STATUS "PKGCONFIG ${PKGCONFIG_LIBS}")

            execute_process(
                COMMAND pkg-config --variable=libdir protobuf
                OUTPUT_VARIABLE LIB_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )

            message(STATUS "LIB_PATH ${LIB_PATH}")
            link_directories(${LIB_PATH})

            include(FindPackageHandleStandardArgs)
            # handle the QUIETLY and REQUIRED arguments and set LibSidekiq_FOUND to TRUE
            # if all listed variables are TRUE
            find_package_handle_standard_args(Sidekiq  DEFAULT_MSG
                Sidekiq_LIBRARY Sidekiq_INCLUDE_DIR )

            set(OTHER_LIBS "")

            mark_as_advanced(Sidekiq_INCLUDE_DIRS Sidekiq_LIBRARIES
                             Sidekiq_RUNTIME_LIBRARIES OTHER_LIBS PKGCONFIG_LIBS)
        else()
          message(STATUS "building for ${SUFFIX}")
            include(FindPackageHandleStandardArgs)
            # handle the QUIETLY and REQUIRED arguments and set LibSidekiq_FOUND to TRUE
            # if all listed variables are TRUE
            find_package_handle_standard_args(Sidekiq  DEFAULT_MSG
                Sidekiq_LIBRARY Sidekiq_INCLUDE_DIR )

            set(OTHER_LIBS "")
            set(PKGCONFIG_LIBS "")
            mark_as_advanced(Sidekiq_INCLUDE_DIRS Sidekiq_LIBRARIES
                             Sidekiq_RUNTIME_LIBRARIES OTHER_LIBS PKGCONFIG_LIBS)
        endif()
    endif()

endif(NOT Sidekiq_FOUND)
