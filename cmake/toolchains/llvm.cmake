# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Native Linux LLVM cohort. Cross Windows builds use LLVM-MinGW's toolchain.
# Opt in with -DCMAKE_TOOLCHAIN_FILE=/path/to/classic/cmake/toolchains/llvm.cmake.
if (NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This LLVM toolchain requires a native Linux host")
endif ()

find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
find_program(CMAKE_AR NAMES llvm-ar REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib REQUIRED)
find_program(CMAKE_LINKER NAMES ld.lld REQUIRED)

foreach (tool CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_AR CMAKE_RANLIB CMAKE_LINKER)
    execute_process(COMMAND "${${tool}}" --version
        OUTPUT_VARIABLE tool_version ERROR_VARIABLE tool_error
        RESULT_VARIABLE tool_result)
    if (NOT tool_result EQUAL 0 OR
            NOT tool_version MATCHES "[Vv]ersion 23\\.1\\.3([^0-9.]|$)" AND
            NOT tool_version MATCHES "LLD 23\\.1\\.3([^0-9.]|$)")
        message(FATAL_ERROR "${tool} must be LLVM 23.1.3: ${tool_version}${tool_error}")
    endif ()
endforeach ()

set(CMAKE_C_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}")
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}")
# Keep CMake's build-type optimization defaults; do not enable LTO or PGO.
set(CMAKE_EXE_LINKER_FLAGS_INIT "--rtlib=compiler-rt --unwindlib=libunwind -fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--rtlib=compiler-rt --unwindlib=libunwind -fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "--rtlib=compiler-rt --unwindlib=libunwind -fuse-ld=lld")
