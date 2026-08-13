include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ATRINIK_IMMUTABLE_FETCHER
    "${CMAKE_CURRENT_LIST_DIR}/../tools/dependencies.py")

# MinGW applies a local patch after acquisition. Its resulting tree remains a
# CMake-owned build input, so verify that copy without creating another fetcher.
function(atrinik_source_tree_sha256 source output)
    file(GLOB_RECURSE source_files
        LIST_DIRECTORIES false
        RELATIVE "${source}"
        "${source}/*")
    list(FILTER source_files EXCLUDE REGEX
        "(^|/)\\.atrinik-(source|mingw-patch)-sha256$")
    list(SORT source_files)
    set(manifest "")
    foreach (relative_path IN LISTS source_files)
        file(SHA256 "${source}/${relative_path}" file_sha256)
        string(APPEND manifest "${file_sha256}  ${relative_path}\n")
    endforeach ()
    string(SHA256 tree_sha256 "${manifest}")
    set(${output} "${tree_sha256}" PARENT_SCOPE)
endfunction()

function(atrinik_extract_immutable_source)
    set(one_value_args NAME URL SHA256 TREE_SHA256 CACHE_DIR OUTPUT)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "${one_value_args}" "")
    foreach (required IN LISTS one_value_args)
        if (NOT arg_${required})
            message(FATAL_ERROR
                "atrinik_extract_immutable_source requires ${required}")
        endif ()
    endforeach ()

    file(MAKE_DIRECTORY "${arg_CACHE_DIR}")
    file(LOCK "${arg_CACHE_DIR}/${arg_NAME}.lock"
        GUARD FUNCTION TIMEOUT 300 RESULT_VARIABLE lock_result)
    if (NOT lock_result EQUAL 0)
        message(FATAL_ERROR
            "Could not lock the shared ${arg_NAME} source cache: ${lock_result}")
    endif ()

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${ATRINIK_IMMUTABLE_FETCHER}" source
            --name "${arg_NAME}"
            --url "${arg_URL}"
            --sha256 "${arg_SHA256}"
            --tree-sha256 "${arg_TREE_SHA256}"
            --cache "${arg_CACHE_DIR}"
        RESULT_VARIABLE fetch_result
        OUTPUT_VARIABLE fetched_source
        ERROR_VARIABLE fetch_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT fetch_result EQUAL 0)
        message(FATAL_ERROR
            "Immutable ${arg_NAME} acquisition failed:\n${fetch_error}")
    endif ()
    set(${arg_OUTPUT} "${fetched_source}" PARENT_SCOPE)
endfunction()
