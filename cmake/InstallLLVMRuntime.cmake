# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
include_guard(GLOBAL)

function(atrinik_install_llvm_runtime_notices destination component)
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif ()
    get_filename_component(compiler "${CMAKE_C_COMPILER}" REALPATH)
    get_filename_component(compiler_bin "${compiler}" DIRECTORY)
    get_filename_component(compiler_prefix "${compiler_bin}" DIRECTORY)
    set(ATRINIK_LLVM_LICENSE_DIRECTORY
        "${compiler_prefix}/share/licenses/llvm" CACHE PATH
        "Complete notices accompanying the LLVM compiler and static runtimes")
    foreach (name IN ITEMS llvm clang clang-tools-extra lld compiler-rt libunwind)
        set(notice "${ATRINIK_LLVM_LICENSE_DIRECTORY}/${name}-LICENSE.TXT")
        if (NOT EXISTS "${notice}" OR IS_DIRECTORY "${notice}" OR IS_SYMLINK "${notice}")
            message(FATAL_ERROR "LLVM runtime installation requires complete notice: ${notice}")
        endif ()
        file(SIZE "${notice}" notice_size)
        if (notice_size EQUAL 0)
            message(FATAL_ERROR "LLVM runtime installation requires nonempty notice: ${notice}")
        endif ()
        install(FILES "${notice}" DESTINATION "${destination}/licenses/llvm"
            COMPONENT "${component}")
    endforeach ()
endfunction()
