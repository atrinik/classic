include_guard(GLOBAL)

include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/immutable_source_cache.cmake)

set(ATRINIK_PCPNATPMP_COMMIT
    866d283da99f5e98eecff702a8df63e2ae57ffca)
set(ATRINIK_PCPNATPMP_SHA256
    65ab99547ecc8277434527607d24f8a1b02a2344ed4cea475bed751606e60202)
set(ATRINIK_PCPNATPMP_TREE_SHA256
    16dbd3e0d59aaa060da9e78b953d3f5e310b5312edc4743218dc8297cbd1292e)
set(ATRINIK_PCPNATPMP_URL
    "https://github.com/libpcpnatpmp/libpcpnatpmp/archive/${ATRINIK_PCPNATPMP_COMMIT}.tar.gz")
set(ATRINIK_DEPENDENCY_CACHE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/../dependency-cache" CACHE PATH
    "Shared verified source cache for immutable Atrinik build dependencies")

function(atrinik_add_pcpnatpmp)
    # libpcpnatpmp uses generic option names that overlap Atrinik's component
    # switches. Function scope keeps these dependency-only values isolated.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_CLI_CLIENT OFF)
    set(BUILD_SERVER OFF)
    set(BUILD_TESTS OFF)

    if (FETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP)
        set(pcpnatpmp_source "${FETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP}")
    else ()
        atrinik_extract_immutable_source(
            NAME libpcpnatpmp
            URL "${ATRINIK_PCPNATPMP_URL}"
            SHA256 "${ATRINIK_PCPNATPMP_SHA256}"
            TREE_SHA256 "${ATRINIK_PCPNATPMP_TREE_SHA256}"
            CACHE_DIR "${ATRINIK_DEPENDENCY_CACHE_DIR}"
            OUTPUT pcpnatpmp_shared_source)
        if (MINGW)
            find_program(PATCH_EXECUTABLE patch REQUIRED)
            file(SHA256
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libpcpnatpmp-mingw.patch"
                pcpnatpmp_patch_sha256)
            set(pcpnatpmp_source
                "${CMAKE_CURRENT_BINARY_DIR}/_deps/libpcpnatpmp-mingw-source")
            set(pcpnatpmp_patch_marker
                "${pcpnatpmp_source}/.atrinik-mingw-patch-sha256")
            set(expected_patch_prefix
                "${ATRINIK_PCPNATPMP_SHA256}:${pcpnatpmp_patch_sha256}")
            set(recreate_pcpnatpmp_source true)
            if (EXISTS "${pcpnatpmp_patch_marker}")
                file(READ "${pcpnatpmp_patch_marker}" actual_patch_marker)
                if (actual_patch_marker MATCHES
                        "^${expected_patch_prefix}:([0-9a-f]+)\n$")
                    set(expected_patched_tree_sha256 "${CMAKE_MATCH_1}")
                    string(LENGTH "${expected_patched_tree_sha256}"
                        patched_tree_sha256_length)
                    if (patched_tree_sha256_length EQUAL 64)
                        atrinik_source_tree_sha256("${pcpnatpmp_source}"
                            actual_patched_tree_sha256)
                        if (actual_patched_tree_sha256 STREQUAL
                                expected_patched_tree_sha256)
                            set(recreate_pcpnatpmp_source false)
                        endif ()
                    endif ()
                endif ()
            endif ()
            if (recreate_pcpnatpmp_source)
                file(REMOVE_RECURSE "${pcpnatpmp_source}")
                file(COPY "${pcpnatpmp_shared_source}/"
                    DESTINATION "${pcpnatpmp_source}")
                file(CHMOD_RECURSE "${pcpnatpmp_source}"
                    FILE_PERMISSIONS
                        OWNER_READ OWNER_WRITE
                        GROUP_READ WORLD_READ
                    DIRECTORY_PERMISSIONS
                        OWNER_READ OWNER_WRITE OWNER_EXECUTE
                        GROUP_READ GROUP_EXECUTE
                        WORLD_READ WORLD_EXECUTE)
                execute_process(
                    COMMAND ${CMAKE_COMMAND}
                        -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
                        -DSOURCE_DIR=${pcpnatpmp_source}
                        -DPATCH_FILE=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libpcpnatpmp-mingw.patch
                        -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apply_patch_idempotent.cmake
                    COMMAND_ERROR_IS_FATAL ANY)
                atrinik_source_tree_sha256("${pcpnatpmp_source}"
                    patched_tree_sha256)
                file(WRITE "${pcpnatpmp_patch_marker}"
                    "${expected_patch_prefix}:${patched_tree_sha256}\n")
            endif ()
        else ()
            set(pcpnatpmp_source "${pcpnatpmp_shared_source}")
        endif ()
    endif ()

    FetchContent_Declare(libpcpnatpmp
        SOURCE_DIR "${pcpnatpmp_source}"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/libpcpnatpmp-build")
    FetchContent_MakeAvailable(libpcpnatpmp)

    if (NOT TARGET pcpnatpmp)
        message(FATAL_ERROR "libpcpnatpmp did not define its library target")
    endif ()

    add_library(pcpnatpmp::pcpnatpmp ALIAS pcpnatpmp)
endfunction()

atrinik_add_pcpnatpmp()
