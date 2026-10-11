# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Native macOS LLVM cohort; Xcode supplies the platform SDK and libc++.
if (NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "This LLVM toolchain requires a native macOS host")
endif ()

# Keep caller-selected LLVM tool discovery available in compiler ABI checks.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_PROGRAM_PATH)

find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
find_program(CMAKE_AR NAMES llvm-ar REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib REQUIRED)
find_program(CMAKE_LINKER NAMES ld64.lld REQUIRED)
find_program(CMAKE_NM NAMES llvm-nm REQUIRED)
find_program(CMAKE_STRIP NAMES llvm-strip REQUIRED)
find_program(CMAKE_OBJCOPY NAMES llvm-objcopy REQUIRED)
find_program(CMAKE_OBJDUMP NAMES llvm-objdump REQUIRED)
find_program(CMAKE_READELF NAMES llvm-readelf REQUIRED)

foreach (tool CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_AR CMAKE_RANLIB CMAKE_LINKER
        CMAKE_NM CMAKE_STRIP CMAKE_OBJCOPY CMAKE_OBJDUMP CMAKE_READELF)
    execute_process(COMMAND "${${tool}}" --version
        OUTPUT_VARIABLE tool_version ERROR_VARIABLE tool_error
        RESULT_VARIABLE tool_result)
    if (NOT tool_result EQUAL 0 OR tool_version MATCHES "Apple clang" OR
            NOT tool_version MATCHES "[Vv]ersion 23\\.1\\.3([^0-9.]|$)" AND
            NOT tool_version MATCHES "LLD 23\\.1\\.3([^0-9.]|$)")
        message(FATAL_ERROR "${tool} must be upstream LLVM 23.1.3: ${tool_version}${tool_error}")
    endif ()
endforeach ()

# An explicit SDK, deployment target and architectures remain caller-owned.
find_program(ATRINIK_XCRUN NAMES xcrun REQUIRED)
if (NOT CMAKE_OSX_SYSROOT)
    execute_process(COMMAND "${ATRINIK_XCRUN}" --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE sdk OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE sdk_result)
    if (NOT sdk_result EQUAL 0 OR NOT IS_DIRECTORY "${sdk}")
        message(FATAL_ERROR "A selected Xcode macOS SDK is required")
    endif ()
    set(CMAKE_OSX_SYSROOT "${sdk}" CACHE PATH "Selected Xcode macOS SDK")
endif ()
# Platform libc++ headers may live in the SDK or beside Apple Clang;
# both layouts remain platform-owned.
set(apple_sdk "${CMAKE_OSX_SYSROOT}")
if (NOT IS_DIRECTORY "${apple_sdk}")
    # CMake also accepts a named SDK such as macosx instead of an absolute path.
    execute_process(COMMAND "${ATRINIK_XCRUN}" --sdk "${CMAKE_OSX_SYSROOT}" --show-sdk-path
        OUTPUT_VARIABLE apple_sdk OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE apple_sdk_result)
    if (NOT apple_sdk_result EQUAL 0 OR NOT IS_DIRECTORY "${apple_sdk}")
        message(FATAL_ERROR "A selected Xcode macOS SDK is required")
    endif ()
endif ()
set(apple_cxx_headers "${apple_sdk}/usr/include/c++/v1")
if (NOT IS_DIRECTORY "${apple_cxx_headers}")
    execute_process(COMMAND "${ATRINIK_XCRUN}" --sdk macosx --find clang++
        OUTPUT_VARIABLE apple_cxx OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE apple_result)
    get_filename_component(apple_bin "${apple_cxx}" DIRECTORY)
    get_filename_component(apple_usr "${apple_bin}" DIRECTORY)
    set(apple_cxx_headers "${apple_usr}/include/c++/v1")
    if (NOT apple_result EQUAL 0 OR NOT IS_DIRECTORY "${apple_cxx_headers}")
        message(FATAL_ERROR "The selected Xcode platform libc++ headers are required")
    endif ()
endif ()
# Avoid the downloaded cohort's newer libc++ headers with the system libc++ ABI.
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem \"${apple_cxx_headers}\"")

execute_process(COMMAND "${CMAKE_C_COMPILER}" --rtlib=compiler-rt -print-libgcc-file-name
    OUTPUT_VARIABLE compiler_rt OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE runtime_result)
if (NOT runtime_result EQUAL 0 OR NOT EXISTS "${compiler_rt}" OR
        NOT compiler_rt MATCHES "/libclang_rt\\.osx\\.a$")
    message(FATAL_ERROR "LLVM 23.1.3 macOS compiler-rt builtins are required: ${compiler_rt}")
endif ()
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}")
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}")
# Mach-O LLD, LLVM builtins and platform unwind; preserve build-type optimization.
set(CMAKE_EXE_LINKER_FLAGS_INIT "--rtlib=compiler-rt -fuse-ld=\"${CMAKE_LINKER}\"")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--rtlib=compiler-rt -fuse-ld=\"${CMAKE_LINKER}\"")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "--rtlib=compiler-rt -fuse-ld=\"${CMAKE_LINKER}\"")
