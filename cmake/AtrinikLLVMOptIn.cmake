# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

# This prerequisite retains the previous producer's defaults. The LLVM preset
# explicitly opts in; resolve from this packaged module rather than assuming
# that a component always has the monorepo's parent directory layout.
function(atrinik_initialize_optional_llvm_toolchain)
    if (ATRINIK_LLVM_TOOLCHAIN AND NOT CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/toolchains/llvm.cmake" CACHE FILEPATH
            "Pinned native LLVM toolchain")
    endif ()
endfunction()

# Explicit toolchains retain their settings, but an LLVM opt-in must not
# silently configure with GCC or a mismatched LLVM cohort.
function(atrinik_require_optional_llvm)
    if (NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
            NOT CMAKE_C_COMPILER_VERSION VERSION_EQUAL "23.1.3")
        message(FATAL_ERROR "LLVM opt-in requires Clang 23.1.3")
    endif ()
    if (CMAKE_CXX_COMPILER_LOADED AND
            (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
             NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "23.1.3"))
        message(FATAL_ERROR "LLVM opt-in C++ dependencies require Clang 23.1.3")
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
endfunction()
