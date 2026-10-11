# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

# Call before project() so standalone and integrated graphs choose the same
# compiler. Explicit portable and LLVM-MinGW toolchains retain their target
# platform, sysroot and architecture settings.
function(atrinik_initialize_llvm_toolchain)
    if (NOT CMAKE_TOOLCHAIN_FILE AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND
            NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(CMAKE_TOOLCHAIN_FILE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/toolchains/llvm.cmake" CACHE FILEPATH
            "Pinned native LLVM toolchain")
    elseif (NOT CMAKE_TOOLCHAIN_FILE AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND
            NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(CMAKE_TOOLCHAIN_FILE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/toolchains/llvm-macos.cmake" CACHE FILEPATH
            "Pinned native macOS LLVM toolchain")
    endif ()
endfunction()

# Call after project(). Compile/link flags below preserve the selected build
# type's optimization and explicit portable CPU baseline; no LTO or PGO.
function(atrinik_require_llvm)
    if (NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
            NOT CMAKE_C_COMPILER_VERSION VERSION_EQUAL "23.1.3")
        message(FATAL_ERROR "Classic native builds require Clang 23.1.3")
    endif ()
    if (CMAKE_CXX_COMPILER_LOADED AND
            (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
             NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "23.1.3"))
        message(FATAL_ERROR "Classic C++ build dependencies require Clang 23.1.3")
    endif ()
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        if (NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
            message(FATAL_ERROR "Classic Windows builds require 64-bit LLVM-MinGW")
        endif ()
        set(windows_languages C)
        if (CMAKE_CXX_COMPILER_LOADED)
            list(APPEND windows_languages CXX)
        endif ()
        if (CMAKE_CONFIGURATION_TYPES)
            set(windows_configurations ${CMAKE_CONFIGURATION_TYPES})
        elseif (CMAKE_BUILD_TYPE)
            set(windows_configurations "${CMAKE_BUILD_TYPE}")
        else ()
            set(windows_configurations __default)
        endif ()
        foreach (language IN LISTS windows_languages)
            if (NOT CMAKE_${language}_COMPILER_FRONTEND_VARIANT STREQUAL "GNU" OR
                    CMAKE_${language}_SIMULATE_ID STREQUAL "MSVC")
                message(FATAL_ERROR "Classic Windows builds require the LLVM-MinGW GNU frontend")
            endif ()
            set(target_arguments)
            if (CMAKE_${language}_COMPILER_ARG1)
                separate_arguments(target_arguments NATIVE_COMMAND "${CMAKE_${language}_COMPILER_ARG1}")
            endif ()
            if (CMAKE_${language}_COMPILER_TARGET)
                list(APPEND target_arguments "--target=${CMAKE_${language}_COMPILER_TARGET}")
            endif ()
            separate_arguments(common_arguments NATIVE_COMMAND "${CMAKE_${language}_FLAGS}")
            list(APPEND target_arguments ${common_arguments})
            foreach (configuration IN LISTS windows_configurations)
                set(configuration_arguments)
                if (NOT configuration STREQUAL __default)
                    string(TOUPPER "${configuration}" configuration_upper)
                    separate_arguments(configuration_arguments NATIVE_COMMAND
                        "${CMAKE_${language}_FLAGS_${configuration_upper}}")
                endif ()
                execute_process(COMMAND "${CMAKE_${language}_COMPILER}"
                    ${target_arguments} ${configuration_arguments} -dumpmachine
                    OUTPUT_VARIABLE windows_target ERROR_VARIABLE target_error
                    RESULT_VARIABLE target_result OUTPUT_STRIP_TRAILING_WHITESPACE)
                # LLVM-MinGW's alias reports the canonical Windows GNU triple.
                # Include flags in compile order: each enabled configuration
                # can override the default target or pointer architecture.
                if (NOT target_result EQUAL 0 OR
                        NOT windows_target MATCHES "^x86_64-w64-(mingw32|windows-gnu)$")
                    message(FATAL_ERROR
                        "Classic Windows builds require the x86_64-w64-mingw32 target (${language}/${configuration}): ${windows_target}${target_error}")
                endif ()
            endforeach ()
        endforeach ()
    endif ()
    foreach (tool CMAKE_AR CMAKE_RANLIB CMAKE_LINKER CMAKE_NM CMAKE_STRIP
            CMAKE_OBJCOPY CMAKE_OBJDUMP CMAKE_READELF)
        execute_process(COMMAND "${${tool}}" --version
            OUTPUT_VARIABLE tool_version ERROR_VARIABLE tool_error
            RESULT_VARIABLE tool_result)
        if (NOT tool_result EQUAL 0 OR
                NOT tool_version MATCHES "LLVM version 23\\.1\\.3([^0-9.]|$)" AND
                NOT tool_version MATCHES "LLD 23\\.1\\.3([^0-9.]|$)")
            message(FATAL_ERROR "${tool} must be LLVM 23.1.3: ${tool_version}${tool_error}")
        endif ()
    endforeach ()
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_link_options(--rtlib=compiler-rt --unwindlib=libunwind -fuse-ld=lld)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        add_link_options(--rtlib=compiler-rt "-fuse-ld=${CMAKE_LINKER}")
    endif ()
endfunction()
