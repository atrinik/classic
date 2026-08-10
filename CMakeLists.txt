cmake_minimum_required(VERSION 3.21)

set(ATRINIK_SOURCE_VERSION "5.1.0")
if (DEFINED ENV{ATRINIK_PACKAGE_VERSION})
    set(ATRINIK_SOURCE_VERSION "$ENV{ATRINIK_PACKAGE_VERSION}")
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
        ATRINIK_SOURCE_VERSION LIMIT_COUNT 1)
else ()
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}"
            describe --tags --exact-match --match "v[0-9]*"
        OUTPUT_VARIABLE ATRINIK_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE ATRINIK_GIT_TAG_RESULT)
    if (ATRINIK_GIT_TAG_RESULT EQUAL 0)
        string(REGEX REPLACE "^v" "" ATRINIK_SOURCE_VERSION
            "${ATRINIK_GIT_TAG}")
    endif ()
endif ()
if (NOT ATRINIK_SOURCE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Atrinik source version must be MAJOR.MINOR.PATCH")
endif ()

project(atrinik-classic VERSION "${ATRINIK_SOURCE_VERSION}" LANGUAGES C)

include(CTest)

option(ATRINIK_BUILD_CLIENT "Build the classic client" ON)
option(ATRINIK_BUILD_SERVER "Build the classic server" ON)
option(ENABLE_WARNING_ERRORS "Treat compiler warnings as errors" ON)
option(ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
option(ENABLE_COVERAGE "Enable GCC coverage instrumentation" OFF)
option(ENABLE_PYTHON_PLUGIN "Build the server Python plugin" ON)
set(PACKAGE_TYPE "none" CACHE STRING "Package type (none or zip)")

if (NOT ATRINIK_BUILD_CLIENT AND NOT ATRINIK_BUILD_SERVER)
    message(FATAL_ERROR
        "The integrated build requires ATRINIK_BUILD_CLIENT or ATRINIK_BUILD_SERVER")
endif ()
if (ENABLE_SANITIZERS AND ENABLE_COVERAGE)
    message(FATAL_ERROR
        "ENABLE_SANITIZERS and ENABLE_COVERAGE cannot share one libatrinik ABI")
endif ()

# libatrinik is also a standalone project. In this graph it inherits this
# function and the shared instrumentation options so both consumers use one
# compatible Atrinik::Core target.
function(atrinik_configure_target target)
    target_compile_features(${target} PRIVATE c_std_17)
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wno-unused-parameter -fno-common)
        if (ENABLE_WARNING_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif ()
    endif ()
    if (ENABLE_SANITIZERS)
        if (MINGW OR NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "ENABLE_SANITIZERS requires native GCC or Clang")
        endif ()
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif ()
    if (ENABLE_COVERAGE)
        if (NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
            message(FATAL_ERROR "ENABLE_COVERAGE requires GCC")
        endif ()
        target_compile_options(${target} PRIVATE
            --coverage -fprofile-abs-path -O0 -g)
        get_target_property(target_type ${target} TYPE)
        if (target_type MATCHES "^(EXECUTABLE|MODULE_LIBRARY|SHARED_LIBRARY)$")
            target_link_options(${target} PRIVATE --coverage)
        endif ()
    endif ()
endfunction()

set(ATRINIK_INTEGRATED_BUILD ON)
set(atrinik_protocol_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protocol")
set(LIBATRINIK_PACKAGE_LAYOUT ON CACHE BOOL
    "Use the namespaced libatrinik include layout" FORCE)

add_subdirectory(protocol)
add_subdirectory(libatrinik)
if (ATRINIK_BUILD_CLIENT)
    add_subdirectory(client)
endif ()
if (ATRINIK_BUILD_SERVER)
    add_subdirectory(server)
endif ()
