include_guard(GLOBAL)

function(atrinik_extract_immutable_source)
    set(one_value_args NAME URL SHA256 CACHE_DIR OUTPUT)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "${one_value_args}" "")
    foreach (required IN LISTS one_value_args)
        if (NOT arg_${required})
            message(FATAL_ERROR
                "atrinik_extract_immutable_source requires ${required}")
        endif ()
    endforeach ()
    if (NOT arg_NAME MATCHES "^[a-z0-9][a-z0-9-]*$")
        message(FATAL_ERROR "Invalid immutable source cache name: ${arg_NAME}")
    endif ()
    string(LENGTH "${arg_SHA256}" sha256_length)
    if (NOT arg_SHA256 MATCHES "^[0-9a-f]+$" OR
            NOT sha256_length EQUAL 64)
        message(FATAL_ERROR
            "Invalid immutable source cache SHA-256 for ${arg_NAME}")
    endif ()

    set(source_root "${arg_CACHE_DIR}/sources/${arg_NAME}-${arg_SHA256}")
    set(marker "${source_root}/.atrinik-source-sha256")
    set(expected_marker "${arg_SHA256}\n")

    file(MAKE_DIRECTORY "${arg_CACHE_DIR}" "${arg_CACHE_DIR}/sources")
    file(LOCK "${arg_CACHE_DIR}/${arg_NAME}.lock"
        GUARD FUNCTION TIMEOUT 300 RESULT_VARIABLE lock_result)
    if (NOT lock_result EQUAL 0)
        message(FATAL_ERROR
            "Could not lock the shared ${arg_NAME} source cache: ${lock_result}")
    endif ()

    if (EXISTS "${source_root}")
        if (NOT EXISTS "${marker}" OR
                NOT EXISTS "${source_root}/CMakeLists.txt")
            message(FATAL_ERROR
                "Incomplete shared ${arg_NAME} source cache: ${source_root}")
        endif ()
        file(READ "${marker}" actual_marker)
        if (NOT actual_marker STREQUAL expected_marker)
            message(FATAL_ERROR
                "Mismatched shared ${arg_NAME} source cache: ${source_root}")
        endif ()
        set(${arg_OUTPUT} "${source_root}" PARENT_SCOPE)
        return()
    endif ()

    set(download_dir "${arg_CACHE_DIR}/downloads")
    set(archive "${download_dir}/${arg_NAME}-${arg_SHA256}.tar.gz")
    set(staging "${arg_CACHE_DIR}/staging/${arg_NAME}-${arg_SHA256}")
    set(unpack "${staging}/unpack")
    file(MAKE_DIRECTORY "${download_dir}" "${arg_CACHE_DIR}/staging")
    file(REMOVE_RECURSE "${staging}")
    file(MAKE_DIRECTORY "${unpack}")

    file(DOWNLOAD "${arg_URL}" "${archive}"
        EXPECTED_HASH "SHA256=${arg_SHA256}"
        TLS_VERIFY ON
        STATUS download_status
        LOG download_log)
    list(GET download_status 0 download_code)
    if (NOT download_code EQUAL 0)
        list(GET download_status 1 download_message)
        message(FATAL_ERROR
            "Could not download verified ${arg_NAME} source: ${download_message}")
    endif ()

    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${unpack}")
    file(GLOB extracted_entries LIST_DIRECTORIES true "${unpack}/*")
    list(LENGTH extracted_entries extracted_count)
    if (NOT extracted_count EQUAL 1)
        message(FATAL_ERROR
            "Verified ${arg_NAME} archive has an unexpected root layout")
    endif ()
    list(GET extracted_entries 0 extracted_root)
    if (NOT IS_DIRECTORY "${extracted_root}" OR
            NOT EXISTS "${extracted_root}/CMakeLists.txt")
        message(FATAL_ERROR
            "Verified ${arg_NAME} archive has no source root")
    endif ()
    file(WRITE "${extracted_root}/.atrinik-source-sha256"
        "${expected_marker}")
    file(RENAME "${extracted_root}" "${source_root}"
        RESULT rename_result)
    if (rename_result)
        message(FATAL_ERROR
            "Could not publish verified ${arg_NAME} source: ${rename_result}")
    endif ()
    file(CHMOD_RECURSE "${source_root}"
        FILE_PERMISSIONS OWNER_READ GROUP_READ WORLD_READ
        DIRECTORY_PERMISSIONS
            OWNER_READ OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    file(REMOVE_RECURSE "${staging}")
    set(${arg_OUTPUT} "${source_root}" PARENT_SCOPE)
endfunction()
