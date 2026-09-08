# Ambient Git selectors must not redirect an owner query into another repository.
# Normal user/system credentials remain available; no network operation is used.
set(ATRINIK_OWNER_GIT_COMMAND "${CMAKE_COMMAND}" -E env
    --unset=GIT_DIR
    --unset=GIT_WORK_TREE
    --unset=GIT_COMMON_DIR
    --unset=GIT_INDEX_FILE
    --unset=GIT_OBJECT_DIRECTORY
    --unset=GIT_ALTERNATE_OBJECT_DIRECTORIES
    --unset=GIT_CONFIG
    --unset=GIT_CONFIG_COUNT
    --unset=GIT_CONFIG_PARAMETERS
    --unset=GIT_CONFIG_SYSTEM
    --unset=GIT_CONFIG_GLOBAL
    --unset=GIT_CONFIG_NOSYSTEM
    --unset=GIT_CEILING_DIRECTORIES
    --unset=GIT_DISCOVERY_ACROSS_FILESYSTEM
    --unset=GIT_NAMESPACE
    --unset=GIT_SHALLOW_FILE
    --unset=GIT_REPLACE_REF_BASE
    git --no-replace-objects)

# Resolve Git only at the physical source owner, never an enclosing workspace.
function(atrinik_source_git_root output)
    file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" source_file)
    get_filename_component(source_dir "${source_file}" DIRECTORY)
    set(candidate "${source_dir}")
    if (NOT EXISTS "${candidate}/.git")
        get_filename_component(component "${source_dir}" NAME)
        get_filename_component(parent "${source_dir}" DIRECTORY)
        file(REAL_PATH "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" module_file)
        file(REAL_PATH "${parent}/cmake/AtrinikVersion.cmake" owner_module)
        if (component MATCHES "^(client|server|protocol|libatrinik)$" AND
                module_file STREQUAL owner_module AND EXISTS "${parent}/.git")
            set(candidate "${parent}")
        else ()
            set(${output} "" PARENT_SCOPE)
            return()
        endif ()
    endif ()
    execute_process(COMMAND ${ATRINIK_OWNER_GIT_COMMAND} -C "${candidate}" rev-parse --show-toplevel
        OUTPUT_VARIABLE git_root OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE result)
    file(REAL_PATH "${candidate}" candidate)
    if (result EQUAL 0 AND git_root STREQUAL candidate)
        set(${output} "${candidate}" PARENT_SCOPE)
    else ()
        set(${output} "" PARENT_SCOPE)
    endif ()
endfunction()

set(ATRINIK_DEVELOPMENT_VERSION "5.1.0")

function(atrinik_resolve_version output)
    set(ATRINIK_PACKAGE_VERSION "" CACHE STRING
        "Explicit Atrinik release version (MAJOR.MINOR.PATCH)")

    if (NOT ATRINIK_PACKAGE_VERSION STREQUAL "")
        set(resolved "${ATRINIK_PACKAGE_VERSION}")
    elseif (DEFINED ENV{ATRINIK_PACKAGE_VERSION} AND NOT "$ENV{ATRINIK_PACKAGE_VERSION}" STREQUAL "")
        set(resolved "$ENV{ATRINIK_PACKAGE_VERSION}")
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
            resolved LIMIT_COUNT 1)
    else ()
        atrinik_source_git_root(owner_root)
        set(tag_result 1)
        if (NOT owner_root STREQUAL "")
            execute_process(
                COMMAND ${ATRINIK_OWNER_GIT_COMMAND} -C "${owner_root}"
                    describe --tags --exact-match --match "v[0-9]*"
                OUTPUT_VARIABLE tag
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE tag_result)
        endif ()
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

    set(ATRINIK_SOURCE_REVISION "" CACHE STRING "Exact physical-owner revision, or unknown")
    set(ATRINIK_SOURCE_DIRTY "" CACHE STRING "Physical-owner dirty state: true, false, unknown")
    atrinik_source_git_root(ATRINIK_OWNER_GIT_ROOT)
    set(ATRINIK_BENCHMARK_REVISION "${ATRINIK_SOURCE_REVISION}")
    if (ATRINIK_BENCHMARK_REVISION STREQUAL "")
        set(ATRINIK_BENCHMARK_REVISION "$ENV{ATRINIK_BENCHMARK_REVISION}")
    endif ()
    if (ATRINIK_BENCHMARK_REVISION STREQUAL "")
        set(benchmark_revision_result 1)
        if (NOT ATRINIK_OWNER_GIT_ROOT STREQUAL "")
            execute_process(
                COMMAND ${ATRINIK_OWNER_GIT_COMMAND} -C "${ATRINIK_OWNER_GIT_ROOT}"
                    rev-parse --verify HEAD
                OUTPUT_VARIABLE ATRINIK_BENCHMARK_REVISION
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE benchmark_revision_result)
        endif ()
        if (NOT benchmark_revision_result EQUAL 0)
            set(ATRINIK_BENCHMARK_REVISION "unknown")
        endif ()
    endif ()
    string(LENGTH "${ATRINIK_BENCHMARK_REVISION}"
        ATRINIK_BENCHMARK_REVISION_LENGTH)
    if (NOT ATRINIK_BENCHMARK_REVISION STREQUAL "unknown" AND
            (NOT ATRINIK_BENCHMARK_REVISION MATCHES "^[0-9A-Fa-f]+$" OR
             (NOT ATRINIK_BENCHMARK_REVISION_LENGTH EQUAL 40 AND
              NOT ATRINIK_BENCHMARK_REVISION_LENGTH EQUAL 64)))
        message(FATAL_ERROR
            "ATRINIK_BENCHMARK_REVISION must be unknown or an exact hexadecimal revision")
    endif ()
    string(TOLOWER "${ATRINIK_BENCHMARK_REVISION}" ATRINIK_BENCHMARK_REVISION)

    set(ATRINIK_BENCHMARK_DIRTY "${ATRINIK_SOURCE_DIRTY}")
    if (ATRINIK_BENCHMARK_DIRTY STREQUAL "")
        set(ATRINIK_BENCHMARK_DIRTY "$ENV{ATRINIK_BENCHMARK_DIRTY}")
    endif ()
    if (ATRINIK_BENCHMARK_DIRTY STREQUAL "")
        set(benchmark_status_result 1)
        if (NOT ATRINIK_OWNER_GIT_ROOT STREQUAL "")
            execute_process(
                COMMAND ${ATRINIK_OWNER_GIT_COMMAND} -C "${ATRINIK_OWNER_GIT_ROOT}"
                    status --porcelain=v1 --untracked-files=normal
                OUTPUT_VARIABLE benchmark_status
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE benchmark_status_result)
        endif ()
        if (benchmark_status_result EQUAL 0)
            if (benchmark_status STREQUAL "")
                set(ATRINIK_BENCHMARK_DIRTY "false")
            else ()
                set(ATRINIK_BENCHMARK_DIRTY "true")
            endif ()
        else ()
            set(ATRINIK_BENCHMARK_DIRTY "unknown")
        endif ()
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
