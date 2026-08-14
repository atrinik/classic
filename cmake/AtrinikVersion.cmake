set(ATRINIK_DEVELOPMENT_VERSION "5.1.0")

function(atrinik_resolve_version output)
    set(ATRINIK_PACKAGE_VERSION "" CACHE STRING
        "Explicit Atrinik release version (MAJOR.MINOR.PATCH)")

    if (NOT ATRINIK_PACKAGE_VERSION STREQUAL "")
        set(resolved "${ATRINIK_PACKAGE_VERSION}")
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
            resolved LIMIT_COUNT 1)
    else ()
        execute_process(
            COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}"
                describe --tags --exact-match --match "v[0-9]*"
            OUTPUT_VARIABLE tag
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE tag_result)
        if (tag_result EQUAL 0)
            string(REGEX REPLACE "^v" "" resolved "${tag}")
        else ()
            set(resolved "${ATRINIK_DEVELOPMENT_VERSION}")
        endif ()
    endif ()

    if (NOT resolved MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        message(FATAL_ERROR
            "ATRINIK_PACKAGE_VERSION must be MAJOR.MINOR.PATCH; resolved '${resolved}'")
    endif ()
    set(${output} "${resolved}" PARENT_SCOPE)
endfunction()

macro(atrinik_initialize_version_metadata)
    if (PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(PACKAGE_VERSION "${PROJECT_VERSION}")
        set(PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
        set(PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
        set(PACKAGE_VERSION_PATCH "${CMAKE_MATCH_3}")
    else ()
        message(FATAL_ERROR "Atrinik project version must be MAJOR.MINOR.PATCH")
    endif ()

    set(ATRINIK_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    if (ATRINIK_BUILD_TYPE STREQUAL "")
        set(ATRINIK_BUILD_TYPE "multi-config")
    endif ()
    set(ATRINIK_COMPILER_ID "${CMAKE_C_COMPILER_ID}")
    set(ATRINIK_COMPILER_VERSION "${CMAKE_C_COMPILER_VERSION}")
    set(ATRINIK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}")
    foreach (variable IN ITEMS
            ATRINIK_BUILD_TYPE
            ATRINIK_COMPILER_ID
            ATRINIK_COMPILER_VERSION
            ATRINIK_SYSTEM_NAME)
        string(LENGTH "${${variable}}" value_length)
        if (value_length GREATER 128 OR
                NOT "${${variable}}" MATCHES "^[A-Za-z0-9][A-Za-z0-9._+ -]*$")
            message(FATAL_ERROR "${variable} contains unsupported characters")
        endif ()
    endforeach ()

    set(ATRINIK_BENCHMARK_REVISION "$ENV{ATRINIK_BENCHMARK_REVISION}")
    if (ATRINIK_BENCHMARK_REVISION STREQUAL "")
        set(ATRINIK_BENCHMARK_REVISION "unknown")
    endif ()
    string(LENGTH "${ATRINIK_BENCHMARK_REVISION}"
        ATRINIK_BENCHMARK_REVISION_LENGTH)
    if (NOT ATRINIK_BENCHMARK_REVISION STREQUAL "unknown" AND
            (NOT ATRINIK_BENCHMARK_REVISION MATCHES "^[0-9A-Fa-f]+$" OR
             ATRINIK_BENCHMARK_REVISION_LENGTH LESS 7 OR
             ATRINIK_BENCHMARK_REVISION_LENGTH GREATER 64))
        message(FATAL_ERROR
            "ATRINIK_BENCHMARK_REVISION must be unknown or a hexadecimal revision")
    endif ()

    set(ATRINIK_BENCHMARK_DIRTY "$ENV{ATRINIK_BENCHMARK_DIRTY}")
    if (ATRINIK_BENCHMARK_DIRTY STREQUAL "")
        set(ATRINIK_BENCHMARK_DIRTY "unknown")
    endif ()
    if (NOT ATRINIK_BENCHMARK_DIRTY MATCHES "^(unknown|true|false)$")
        message(FATAL_ERROR
            "ATRINIK_BENCHMARK_DIRTY must be unknown, true, or false")
    endif ()
endmacro()

function(atrinik_apply_version_metadata target)
    target_compile_definitions(${target} PRIVATE
        "PACKAGE_VERSION=\"${PACKAGE_VERSION}\""
        PACKAGE_VERSION_MAJOR=${PACKAGE_VERSION_MAJOR}
        PACKAGE_VERSION_MINOR=${PACKAGE_VERSION_MINOR}
        PACKAGE_VERSION_PATCH=${PACKAGE_VERSION_PATCH}
        "ATRINIK_BUILD_TYPE=\"${ATRINIK_BUILD_TYPE}\""
        "ATRINIK_COMPILER_ID=\"${ATRINIK_COMPILER_ID}\""
        "ATRINIK_COMPILER_VERSION=\"${ATRINIK_COMPILER_VERSION}\""
        "ATRINIK_SYSTEM_NAME=\"${ATRINIK_SYSTEM_NAME}\""
        "ATRINIK_BENCHMARK_REVISION=\"${ATRINIK_BENCHMARK_REVISION}\""
        "ATRINIK_BENCHMARK_DIRTY=\"${ATRINIK_BENCHMARK_DIRTY}\"")
    if (DEFINED PACKAGE_NAME)
        target_compile_definitions(${target} PRIVATE
            "PACKAGE_NAME=\"${PACKAGE_NAME}\"")
    endif ()
endfunction()
