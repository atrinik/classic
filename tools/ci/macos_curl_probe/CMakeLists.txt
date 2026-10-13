# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.21)
include(../../../cmake/AtrinikLLVM.cmake)
project(AtrinikMacOSCurlProbe LANGUAGES C)
atrinik_require_llvm()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "curl baseline collection requires native macOS")
endif()
set(CURL_NO_CURL_CMAKE TRUE)
find_package(CURL MODULE REQUIRED)
get_target_property(curl_header_directories CURL::libcurl INTERFACE_INCLUDE_DIRECTORIES)
file(WRITE "${CMAKE_BINARY_DIR}/selected-headers.txt" "${curl_header_directories}\n")
add_executable(macos-curl-probe main.c)
set_target_properties(macos-curl-probe PROPERTIES
    C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(macos-curl-probe PRIVATE CURL::libcurl)
if(ATRINIK_CURL_COHORT)
    find_package(c-ares CONFIG REQUIRED)
    target_link_libraries(macos-curl-probe PRIVATE c-ares::cares)
    target_compile_definitions(macos-curl-probe PRIVATE ATRINIK_CURL_COHORT=1)
    get_target_property(cares_header_directories c-ares::cares INTERFACE_INCLUDE_DIRECTORIES)
    file(WRITE "${CMAKE_BINARY_DIR}/selected-cares-headers.txt" "${cares_header_directories}\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected-cares-library.txt"
        CONTENT "$<TARGET_FILE:c-ares::cares>\n")
endif()
target_compile_options(macos-curl-probe PRIVATE -Wall -Wextra -Werror)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected-library.txt"
    CONTENT "$<TARGET_FILE:CURL::libcurl>\n")
