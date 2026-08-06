include_guard(GLOBAL)

include(FetchContent)

function(atrinik_add_pcpnatpmp)
    # libpcpnatpmp uses generic option names that overlap Atrinik's component
    # switches. Function scope keeps these dependency-only values isolated.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_CLI_CLIENT OFF)
    set(BUILD_SERVER OFF)
    set(BUILD_TESTS OFF)

    set(pcpnatpmp_patch_args)
    if (MINGW)
        find_program(PATCH_EXECUTABLE patch REQUIRED)
        set(pcpnatpmp_patch_args
            PATCH_COMMAND
                ${CMAKE_COMMAND}
                -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
                -DSOURCE_DIR=<SOURCE_DIR>
                -DPATCH_FILE=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libpcpnatpmp-mingw.patch
                -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apply_patch_idempotent.cmake)
    endif ()

    FetchContent_Declare(libpcpnatpmp
        URL https://github.com/libpcpnatpmp/libpcpnatpmp/archive/866d283da99f5e98eecff702a8df63e2ae57ffca.tar.gz
        URL_HASH SHA256=65ab99547ecc8277434527607d24f8a1b02a2344ed4cea475bed751606e60202
        DOWNLOAD_EXTRACT_TIMESTAMP false
        ${pcpnatpmp_patch_args})
    FetchContent_MakeAvailable(libpcpnatpmp)

    if (NOT TARGET pcpnatpmp)
        message(FATAL_ERROR "libpcpnatpmp did not define its library target")
    endif ()

    add_library(pcpnatpmp::pcpnatpmp ALIAS pcpnatpmp)
endfunction()

atrinik_add_pcpnatpmp()
