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
    foreach (tool CMAKE_AR CMAKE_RANLIB CMAKE_LINKER)
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
    endif ()
endfunction()
