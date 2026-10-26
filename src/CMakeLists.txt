cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(AndorEMCCDcontroller VERSION 0.3.1 LANGUAGES C CXX)

add_compile_definitions(AEC_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
# Note: -Ofast enables -ffast-math, which breaks strict IEEE float semantics.
# Use -Ofast instead only if you understand and accept that trade-off.
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3")
endif()

# -----
# Build options
# -----

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(aec_default_backend ON)
else()
    set(aec_default_backend OFF)
endif()

option(AEC_BUILD_BACKEND "Build the camera backend service (Linux only)" ${aec_default_backend})
option(AEC_BUILD_GUI "Build the Qt6 frontend" ON)
option(AEC_BUILD_TESTS "Build unit tests" ON)
set(ANDOR_SDK_ROOT "/usr/local" CACHE PATH
    "Andor SDK2 prefix (contains include/atmcdLXd.h and lib/libandor.so)")
option(AEC_WITH_ANDOR_SDK "Build the real Andor camera driver if the SDK is found" ON)

# Compiler warnings, shared by the std helpers in core/ and exec/.
if(MSVC)
    set(AEC_WARNING_FLAGS /W4 /permissive- /utf-8)
    add_compile_definitions(NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
else()
    set(AEC_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# -----
# External packages
# -----

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 # v3.11.3
    FIND_PACKAGE_ARGS 3.10
)

FetchContent_Declare(
    argparse
    GIT_REPOSITORY https://github.com/p-ranav/argparse
    GIT_TAG        af442b4da0cd7a07b56fa709bd16571889dc7fda # v3.0
    FIND_PACKAGE_ARGS
)

# tinyexpr has no CMake build; it is compiled as a tiny static library below.
FetchContent_Declare(
    tinyexpr
    GIT_REPOSITORY https://github.com/codeplea/tinyexpr
    GIT_TAG        7ab30db449a359e8af77fbb062ab2ec7ee686d62 # master HEAD 2026-10
)

FetchContent_MakeAvailable(nlohmann_json argparse tinyexpr)

if(NOT TARGET tinyexpr)
    add_library(tinyexpr STATIC ${tinyexpr_SOURCE_DIR}/tinyexpr.c)
    target_include_directories(tinyexpr SYSTEM PUBLIC
        $<BUILD_INTERFACE:${tinyexpr_SOURCE_DIR}>)
    set_target_properties(tinyexpr PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(NOT MSVC)
        target_link_libraries(tinyexpr PUBLIC m)
    endif()
endif()

# ZeroMQ: system package (dnf cppzmq-devel / vcpkg cppzmq), else fetched.
find_package(cppzmq QUIET)
if(NOT cppzmq_FOUND)
    message(STATUS "cppzmq not found: fetching libzmq and cppzmq")
    set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
    set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
    set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libzmq
        GIT_REPOSITORY https://github.com/zeromq/libzmq
        GIT_TAG        622fc6dde99ee172ebaa9c8628d85a7a1995a21d # v4.3.5
    )
    FetchContent_Declare(
        cppzmq
        GIT_REPOSITORY https://github.com/zeromq/cppzmq
        GIT_TAG        c94c20743ed7d4aa37835a5c46567ab0790d4acc # v4.10.0
    )
    FetchContent_MakeAvailable(libzmq cppzmq)
endif()

if(AEC_BUILD_BACKEND)
    find_package(spdlog QUIET)
    if(NOT spdlog_FOUND)
        FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog
            GIT_TAG        8e5613379f5140fefb0b60412fbf1f5406e7c7f8 # v1.15.0
        )
        FetchContent_MakeAvailable(spdlog)
    endif()

    set(AEC_HAVE_ANDOR_SDK OFF)
    if(AEC_WITH_ANDOR_SDK)
        find_path(ANDOR_INCLUDE_DIR atmcdLXd.h
            HINTS ${ANDOR_SDK_ROOT}/include NO_DEFAULT_PATH)
        find_library(ANDOR_LIBRARY
            NAMES andor libandor.so.2 andor-x86_64.so.2.104.30088.0
            HINTS ${ANDOR_SDK_ROOT}/lib NO_DEFAULT_PATH)
        if(ANDOR_INCLUDE_DIR AND ANDOR_LIBRARY)
            set(AEC_HAVE_ANDOR_SDK ON)
            message(STATUS "Andor SDK: ${ANDOR_LIBRARY}")
        else()
            message(WARNING "Andor SDK not found under ${ANDOR_SDK_ROOT}; "
                "only the simulated camera is built")
        endif()
    endif()
endif()

if(AEC_BUILD_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network)
    find_package(TIFF REQUIRED)
    find_package(PNG REQUIRED)
    find_package(CFITSIO REQUIRED)
endif()

add_subdirectory(${PROJECT_SOURCE_DIR}/doc)
add_subdirectory(${PROJECT_SOURCE_DIR}/core)
add_subdirectory(${PROJECT_SOURCE_DIR}/exec)

if(AEC_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest
        GIT_TAG        b514bdc898e2951020cbdca1304b75f5950d1f59 # v1.15.2
        FIND_PACKAGE_ARGS NAMES GTest
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(${PROJECT_SOURCE_DIR}/test)
endif()
