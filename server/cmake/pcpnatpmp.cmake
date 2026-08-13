include_guard(GLOBAL)

include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/immutable_source_cache.cmake)

file(READ "${CMAKE_CURRENT_LIST_DIR}/immutable_sources.lock.json"
    ATRINIK_IMMUTABLE_SOURCE_LOCK)
string(JSON ATRINIK_IMMUTABLE_SOURCE_SCHEMA ERROR_VARIABLE lock_error
    GET "${ATRINIK_IMMUTABLE_SOURCE_LOCK}" schema_version)
if (lock_error OR NOT ATRINIK_IMMUTABLE_SOURCE_SCHEMA EQUAL 1)
    message(FATAL_ERROR "immutable_sources.lock.json has an unsupported schema")
endif ()
foreach (field IN ITEMS url sha256 tree_sha256 mingw_tree_sha256)
    string(JSON ATRINIK_PCPNATPMP_${field} ERROR_VARIABLE lock_error
        GET "${ATRINIK_IMMUTABLE_SOURCE_LOCK}" sources libpcpnatpmp ${field})
    if (lock_error)
        message(FATAL_ERROR "libpcpnatpmp immutable source metadata is invalid")
    endif ()
    if (field STREQUAL "url")
        string(REGEX MATCH
            "^https://github.com/libpcpnatpmp/libpcpnatpmp/archive/([0-9a-f]+)\\.tar\\.gz$"
            source_url_match "${ATRINIK_PCPNATPMP_${field}}")
        string(REGEX REPLACE
            "^https://github.com/libpcpnatpmp/libpcpnatpmp/archive/([0-9a-f]+)\\.tar\\.gz$"
            "\\1" source_commit "${ATRINIK_PCPNATPMP_${field}}")
        string(LENGTH "${source_commit}" source_commit_length)
        if (NOT source_url_match OR NOT source_commit_length EQUAL 40)
            message(FATAL_ERROR "libpcpnatpmp immutable source URL is invalid")
        endif ()
    else ()
        string(LENGTH "${ATRINIK_PCPNATPMP_${field}}" digest_length)
        if (NOT ATRINIK_PCPNATPMP_${field} MATCHES "^[0-9a-f]+$" OR
                NOT digest_length EQUAL 64)
            message(FATAL_ERROR "libpcpnatpmp immutable source digest is invalid")
        endif ()
    endif ()
endforeach ()
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
            SOURCE_LOCK "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/immutable_sources.lock.json"
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
                "${ATRINIK_PCPNATPMP_sha256}:${pcpnatpmp_patch_sha256}")
            set(expected_patch_marker
                "${expected_patch_prefix}:${ATRINIK_PCPNATPMP_mingw_tree_sha256}\n")
            set(recreate_pcpnatpmp_source true)
            if (EXISTS "${pcpnatpmp_patch_marker}")
                file(READ "${pcpnatpmp_patch_marker}" actual_patch_marker)
                if (actual_patch_marker STREQUAL expected_patch_marker)
                    atrinik_source_tree_sha256("${pcpnatpmp_source}"
                        actual_patched_tree_sha256)
                    if (actual_patched_tree_sha256 STREQUAL
                            ATRINIK_PCPNATPMP_mingw_tree_sha256)
                        set(recreate_pcpnatpmp_source false)
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
                if (NOT patched_tree_sha256 STREQUAL
                        ATRINIK_PCPNATPMP_mingw_tree_sha256)
                    message(FATAL_ERROR
                        "Patched libpcpnatpmp source has unexpected content")
                endif ()
                file(WRITE "${pcpnatpmp_patch_marker}"
                    "${expected_patch_marker}")
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
