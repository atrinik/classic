cmake_minimum_required(VERSION 3.21)

set(ATRINIK_SOURCE_VERSION "5.1.0")
if (DEFINED ENV{ATRINIK_PACKAGE_VERSION})
    set(ATRINIK_SOURCE_VERSION "$ENV{ATRINIK_PACKAGE_VERSION}")
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
        ATRINIK_SOURCE_VERSION LIMIT_COUNT 1)
else ()
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}"
            describe --tags --exact-match --match "v[0-9]*"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE ATRINIK_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE ATRINIK_GIT_TAG_RESULT)
    if (ATRINIK_GIT_TAG_RESULT EQUAL 0)
        string(REGEX REPLACE "^v" "" ATRINIK_SOURCE_VERSION
            "${ATRINIK_GIT_TAG}")
    endif ()
endif ()
if (NOT ATRINIK_SOURCE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Atrinik source version must be MAJOR.MINOR.PATCH")
endif ()

project(atrinik-server VERSION "${ATRINIK_SOURCE_VERSION}" LANGUAGES C)

include(CTest)
include(FetchContent)

function(atrinik_find_root_document output filename)
    file(REAL_PATH "${CMAKE_CURRENT_LIST_FILE}" ATRINIK_REAL_CMAKE_FILE)
    get_filename_component(ATRINIK_REAL_SOURCE_DIR
        "${ATRINIK_REAL_CMAKE_FILE}" DIRECTORY)
    foreach (candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/${filename}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../${filename}"
            "${ATRINIK_REAL_SOURCE_DIR}/${filename}"
            "${ATRINIK_REAL_SOURCE_DIR}/../${filename}")
        if (EXISTS "${candidate}")
            file(REAL_PATH "${candidate}" resolved)
            set(${output} "${resolved}" PARENT_SCOPE)
            return()
        endif ()
    endforeach ()
    message(FATAL_ERROR "Cannot locate required root document: ${filename}")
endfunction()

atrinik_find_root_document(ATRINIK_LICENSE_FILE LICENSE.md)
atrinik_find_root_document(ATRINIK_ATTRIBUTIONS_FILE ATTRIBUTIONS.md)

option(ENABLE_WARNING_ERRORS "Treat compiler warnings as errors" ON)
option(ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
option(ENABLE_COVERAGE "Enable GCC coverage instrumentation" OFF)
if (ATRINIK_INTEGRATED_BUILD)
    option(ATRINIK_SERVER_ENABLE_PRECOMPILED_HEADERS
        "Precompile the server global header for the core object library" ON)
else ()
    option(ENABLE_PRECOMPILED_HEADERS
        "Precompile the server global header for the core object library" ON)
    set(ATRINIK_SERVER_ENABLE_PRECOMPILED_HEADERS
        "${ENABLE_PRECOMPILED_HEADERS}")
endif ()
option(ENABLE_PYTHON_PLUGIN "Build the Python plugin" ON)
set(PACKAGE_TYPE "none" CACHE STRING "Package type (none or zip)")

set(PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Semantic version shown by the server and used for packages")
if (NOT PACKAGE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "PACKAGE_VERSION must be a three-part semantic version")
endif ()
set(PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(PACKAGE_VERSION_PATCH "${CMAKE_MATCH_3}")

if (MINGW)
    set(PLUGIN_SUFFIX ".dll")
    add_compile_definitions(HAVE_STRUCT_TIMESPEC NOCRYPT __USE_MINGW_ANSI_STDIO=0)
else ()
    set(LINUX true)
    set(PLUGIN_SUFFIX ".so")
endif ()

function(atrinik_configure_target target)
    target_compile_features(${target} PRIVATE c_std_17)
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wno-unused-parameter -fno-common)
        if (ENABLE_WARNING_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif ()
    endif ()
    if (ENABLE_SANITIZERS)
        if (MINGW OR NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "ENABLE_SANITIZERS requires native GCC or Clang")
        endif ()
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif ()
    if (ENABLE_COVERAGE)
        if (NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
            message(FATAL_ERROR "ENABLE_COVERAGE requires GCC")
        endif ()
        target_compile_options(${target} PRIVATE
            --coverage -fprofile-abs-path -fprofile-update=atomic -O0 -g)
        get_target_property(target_type ${target} TYPE)
        if (target_type MATCHES "^(EXECUTABLE|MODULE_LIBRARY|SHARED_LIBRARY)$")
            target_link_options(${target} PRIVATE --coverage)
        endif ()
    endif ()
endfunction()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies.lock.json"
    ATRINIK_SOURCE_LOCK)
string(JSON ATRINIK_SOURCE_LOCK_SCHEMA ERROR_VARIABLE lock_error
    GET "${ATRINIK_SOURCE_LOCK}" schema_version)
if (lock_error OR NOT ATRINIK_SOURCE_LOCK_SCHEMA EQUAL 1)
    message(FATAL_ERROR "cmake/dependencies.lock.json has an unsupported schema")
endif ()

function(atrinik_fetch_source name expected_repository expected_archive)
    foreach (field IN ITEMS repository tag commit url sha256)
        string(JSON dependency_${field} ERROR_VARIABLE lock_error
            GET "${ATRINIK_SOURCE_LOCK}" dependencies ${name} ${field})
        if (lock_error OR dependency_${field} STREQUAL "")
            message(FATAL_ERROR "Source dependency ${name}.${field} is missing")
        endif ()
    endforeach ()
    string(REGEX REPLACE "^v" "" dependency_version "${dependency_tag}")
    set(expected_url
        "https://github.com/${expected_repository}/releases/download/${dependency_tag}/${expected_archive}-${dependency_version}.tar.gz")
    string(LENGTH "${dependency_commit}" commit_length)
    string(LENGTH "${dependency_sha256}" digest_length)
    if (NOT dependency_repository STREQUAL expected_repository OR
            NOT dependency_tag MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$" OR
            NOT dependency_commit MATCHES "^[0-9a-f]+$" OR
            NOT commit_length EQUAL 40 OR
            NOT dependency_url STREQUAL expected_url OR
            NOT dependency_sha256 MATCHES "^[0-9a-f]+$" OR
            NOT digest_length EQUAL 64)
        message(FATAL_ERROR "Source dependency ${name} has invalid lock metadata")
    endif ()
    FetchContent_Declare(${name}
        URL "${dependency_url}"
        URL_HASH "SHA256=${dependency_sha256}"
        DOWNLOAD_EXTRACT_TIMESTAMP false)
endfunction()

if (NOT FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL)
    foreach (candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/../protocol"
            "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/protocol")
        if (EXISTS "${candidate}/CMakeLists.txt")
            file(REAL_PATH "${candidate}" resolved)
            set(FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL "${resolved}"
                CACHE PATH "Atrinik protocol source" FORCE)
            break()
        endif ()
    endforeach ()
endif ()
if (NOT FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL)
    message(FATAL_ERROR
        "Classic protocol source is required: use the sibling protocol/ tree, "
        "the packaged dependencies/protocol tree, or "
        "FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL")
endif ()
if (NOT FETCHCONTENT_SOURCE_DIR_LIBATRINIK)
    foreach (candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/../libatrinik"
            "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libatrinik")
        if (EXISTS "${candidate}/CMakeLists.txt")
            file(REAL_PATH "${candidate}" resolved)
            set(FETCHCONTENT_SOURCE_DIR_LIBATRINIK "${resolved}"
                CACHE PATH "libatrinik source" FORCE)
            break()
        endif ()
    endforeach ()
endif ()

if (NOT TARGET Atrinik::Protocol)
    FetchContent_Declare(atrinik_protocol
        SOURCE_DIR "${FETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL}")
    FetchContent_MakeAvailable(atrinik_protocol)
endif ()
file(STRINGS
    "${atrinik_protocol_SOURCE_DIR}/generated/c/include/atrinik/protocol/game_commands.h"
    ATRINIK_PROTOCOL_VERSION_LINE REGEX "^#define ATRINIK_PROTOCOL_VERSION [0-9]+U$")
if (NOT ATRINIK_PROTOCOL_VERSION_LINE MATCHES " 1076U$")
    message(FATAL_ERROR "Server requires classic game protocol v1076")
endif ()
if (NOT TARGET Atrinik::Core)
    set(LIBATRINIK_PACKAGE_LAYOUT ON CACHE BOOL "" FORCE)
    atrinik_fetch_source(libatrinik "atrinik/libatrinik" "libatrinik")
    FetchContent_MakeAvailable(libatrinik)
endif ()
include(cmake/pcpnatpmp.cmake)

set(EXECUTABLE atrinik-server)
set(PACKAGE_NAME "Atrinik Server")

set(ATRINIK_SERVER_INCLUDE_DIRS
    ${CMAKE_CURRENT_BINARY_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/random_maps
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/src/types/monster/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/types/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(FLEX REQUIRED)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/src/loaders")
set(LEXERS_OUT)
foreach (name map_header object random_map)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/src/loaders/${name}.l")
    set(dst "${CMAKE_CURRENT_BINARY_DIR}/src/loaders/${name}.c")
    flex_target(${name} "${src}" "${dst}"
        COMPILE_FLAGS "-Cfa -i --prefix yy_${name}")
    list(APPEND LEXERS_OUT ${FLEX_${name}_OUTPUTS})
endforeach ()

include(src/cmake.txt)
find_package(PkgConfig REQUIRED)

if (NOT MINGW)
    pkg_check_modules(MINIUPNPC REQUIRED IMPORTED_TARGET miniupnpc)
endif ()

find_library(DL_LIBRARY dl)
if (NOT DL_LIBRARY AND NOT MINGW)
    message(FATAL_ERROR "Could not find dl library.")
endif ()

find_library(GD_LIBRARY gd)
if (GD_LIBRARY)
    set(HAVE_WORLD_MAKER true)
    set(SOURCES_WORLD_MAKER src/modules/world_maker.c)
else ()
    message(STATUS "Could not find gd library, world maker module will be disabled.")
endif ()

configure_file(src/include/cmake.h.def src/include/cmake.h)
configure_file(src/include/version.h.def src/include/version.h)

add_library(atrinik-server-core OBJECT
    ${SERVER_CORE_SOURCES}
    ${SOURCES_WORLD_MAKER}
    ${LEXERS_OUT})
atrinik_configure_target(atrinik-server-core)
if (ATRINIK_SERVER_ENABLE_PRECOMPILED_HEADERS)
    target_precompile_headers(atrinik-server-core PRIVATE src/include/global.h)
endif ()
target_include_directories(atrinik-server-core BEFORE PRIVATE
    ${ATRINIK_SERVER_INCLUDE_DIRS})
target_link_libraries(atrinik-server-core PRIVATE
    Atrinik::Core
    Atrinik::Pathfinding
    pcpnatpmp::pcpnatpmp)

if (MINGW)
    find_library(ATRINIK_MINIUPNPC_STATIC_LIBRARY miniupnpc)
    if (NOT ATRINIK_MINIUPNPC_STATIC_LIBRARY)
        message(FATAL_ERROR "Could not find the static MiniUPnPc library.")
    endif ()

    target_compile_definitions(atrinik-server-core PRIVATE MINIUPNP_STATICLIB)
    target_link_libraries(atrinik-server-core PRIVATE
        ${ATRINIK_MINIUPNPC_STATIC_LIBRARY}
        ws2_32
        iphlpapi)
else ()
    target_link_libraries(atrinik-server-core PRIVATE PkgConfig::MINIUPNPC)
endif ()

if (DL_LIBRARY)
    target_link_libraries(atrinik-server-core PRIVATE ${DL_LIBRARY})
endif ()

if (GD_LIBRARY)
    target_link_libraries(atrinik-server-core PRIVATE ${GD_LIBRARY})
    if (MINGW)
        target_link_libraries(atrinik-server-core PRIVATE png)
    endif ()
endif ()

add_executable(${EXECUTABLE} src/server/entrypoint.c)
atrinik_configure_target(${EXECUTABLE})
target_include_directories(${EXECUTABLE} BEFORE PRIVATE
    ${ATRINIK_SERVER_INCLUDE_DIRS})
target_link_libraries(${EXECUTABLE} PRIVATE atrinik-server-core Atrinik::Core)

add_library(plugin_arena MODULE
    src/plugins/plugin_arena/plugin_arena.c)
atrinik_configure_target(plugin_arena)
target_include_directories(plugin_arena BEFORE PRIVATE
    ${ATRINIK_SERVER_INCLUDE_DIRS})
target_link_libraries(plugin_arena PRIVATE Atrinik::Core)

# Create the Python plugin, if requested and Python was found.
if (ENABLE_PYTHON_PLUGIN)
    set(ATRINIK_WINDOWS_PYTHON_INCLUDE_DIR "" CACHE PATH
        "Target Python headers for a MinGW server build")
    set(ATRINIK_WINDOWS_PYTHON_LIBRARY "" CACHE FILEPATH
        "Target Python import library for a MinGW server build")

    if (MINGW AND ATRINIK_WINDOWS_PYTHON_INCLUDE_DIR AND
            ATRINIK_WINDOWS_PYTHON_LIBRARY)
        set(PYTHONLIBS_FOUND true)
        set(PYTHON_INCLUDE_DIRS ${ATRINIK_WINDOWS_PYTHON_INCLUDE_DIR})
        set(PYTHON_LIBRARIES ${ATRINIK_WINDOWS_PYTHON_LIBRARY})
    else ()
        find_package(Python QUIET COMPONENTS Development)

        if (Python_Development_FOUND)
            set(PYTHONLIBS_FOUND true)
            set(PYTHON_INCLUDE_DIRS ${Python_INCLUDE_DIRS})
            set(PYTHON_LIBRARIES Python::Python)
        endif ()
    endif ()

    if (PYTHONLIBS_FOUND)
        set(HAVE_PYTHON true)

        set(SOURCES_PLUGIN_PYTHON
            src/plugins/plugin_python/atrinik_archetype.c
            src/plugins/plugin_python/atrinik_map.c
            src/plugins/plugin_python/atrinik_object.c
            src/plugins/plugin_python/atrinik_party.c
            src/plugins/plugin_python/atrinik_player.c
            src/plugins/plugin_python/atrinik_region.c
            src/plugins/plugin_python/attr_list.c
            src/plugins/plugin_python/plugin_python.c)
        add_library(plugin_python MODULE ${SOURCES_PLUGIN_PYTHON})
        atrinik_configure_target(plugin_python)
        target_include_directories(plugin_python BEFORE PRIVATE
            ${ATRINIK_SERVER_INCLUDE_DIRS}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/plugin_python/include
            ${PYTHON_INCLUDE_DIRS})
        target_link_libraries(plugin_python PRIVATE
            Atrinik::Core
            ${PYTHON_LIBRARIES})
        add_dependencies(${EXECUTABLE} plugin_python)
    else ()
        message(FATAL_ERROR "Python development files are required for the Python plugin.")
    endif ()
else ()
    message(STATUS "Python plugin disabled by ENABLE_PYTHON_PLUGIN=false.")
endif ()

if (BUILD_TESTING AND NOT MINGW)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    pkg_check_modules(CHECK_PKG QUIET IMPORTED_TARGET check)

    if (CHECK_PKG_FOUND)
        include(src/tests/suites.cmake)
        set(SOURCES_CHECK
            src/tests/main.c
            src/tests/check.c
            src/tests/server_clock_fake.c)
        set(ATRINIK_SERVER_UNIT_SUITES)
        set(ATRINIK_SERVER_TEST_SUITE_DECLARATIONS)
        set(ATRINIK_SERVER_TEST_SUITE_ENTRIES)
        foreach (suite_record IN LISTS ATRINIK_SERVER_TEST_SUITES)
            string(REPLACE "|" ";" suite_fields "${suite_record}")
            list(LENGTH suite_fields suite_field_count)
            if (NOT suite_field_count EQUAL 3)
                message(FATAL_ERROR "Invalid server test suite: ${suite_record}")
            endif ()

            list(GET suite_fields 0 suite_name)
            list(GET suite_fields 1 suite_function)
            list(GET suite_fields 2 suite_source)
            list(APPEND ATRINIK_SERVER_UNIT_SUITES "${suite_name}")
            list(APPEND SOURCES_CHECK "${suite_source}")
            string(APPEND ATRINIK_SERVER_TEST_SUITE_DECLARATIONS
                "extern void ${suite_function}(void);\n")
            string(APPEND ATRINIK_SERVER_TEST_SUITE_ENTRIES
                "    {\"${suite_name}\", ${suite_function}},\n")
        endforeach ()

        configure_file(
            src/tests/check_suites.h.in
            src/include/check_suites.h
            @ONLY)
        configure_file(
            src/tests/check_suites.inc.in
            src/include/check_suites.inc
            @ONLY)

        add_executable(atrinik-server-tests ${SOURCES_CHECK})
        atrinik_configure_target(atrinik-server-tests)
        target_include_directories(atrinik-server-tests BEFORE PRIVATE
            ${ATRINIK_SERVER_INCLUDE_DIRS})
        target_compile_definitions(atrinik-server-tests PRIVATE
            ATRINIK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/tests/data")
        target_link_libraries(atrinik-server-tests PRIVATE
            Atrinik::Core
            atrinik-server-core
            PkgConfig::CHECK_PKG)

        set(ATRINIK_SERVER_TEST_RUNTIME_SEED
            "${CMAKE_CURRENT_BINARY_DIR}/server-test-runtime-seed")
        set(ATRINIK_SERVER_TEST_RUNTIME_STAMP
            "${ATRINIK_SERVER_TEST_RUNTIME_SEED}/.prepared")
        set(ATRINIK_SERVER_TEST_RUNTIMES
            "${CMAKE_CURRENT_BINARY_DIR}/server-test-runtimes")
        set(ATRINIK_SERVER_TEST_RUNTIME_ARGS
            -DATRINIK_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DATRINIK_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DATRINIK_RUNTIME_DIR=${ATRINIK_SERVER_TEST_RUNTIME_SEED}
            -DATRINIK_ARENA_PLUGIN=$<TARGET_FILE:plugin_arena>)
        set(ATRINIK_SERVER_TEST_RUNTIME_DEPENDS plugin_arena)
        if (TARGET plugin_python)
            set(ATRINIK_GAME_COMMANDS_HEADER
                "${atrinik_protocol_SOURCE_DIR}/generated/c/include/atrinik/protocol/game_commands.h")
            list(APPEND ATRINIK_SERVER_TEST_RUNTIME_ARGS
                -DATRINIK_PYTHON_PLUGIN=$<TARGET_FILE:plugin_python>
                -DATRINIK_GAME_COMMANDS_HEADER=${ATRINIK_GAME_COMMANDS_HEADER})
            list(APPEND ATRINIK_SERVER_TEST_RUNTIME_DEPENDS plugin_python)
        endif ()

        file(GLOB_RECURSE ATRINIK_SERVER_TEST_RESOURCE_INPUTS
            CONFIGURE_DEPENDS
            LIST_DIRECTORIES false
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/content/*"
            "${CMAKE_CURRENT_SOURCE_DIR}/install_data/*")
        list(APPEND ATRINIK_SERVER_TEST_RESOURCE_INPUTS
            ${CMAKE_CURRENT_SOURCE_DIR}/ca-bundle.crt
            ${CMAKE_CURRENT_SOURCE_DIR}/permissions.cfg
            ${CMAKE_CURRENT_SOURCE_DIR}/server.cfg
            ${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock.json
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/data/content_benchmark
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/data/plugin_python/python_unit_fail.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/data/plugin_python/python_unit_pass.py)
        if (TARGET plugin_python)
            list(APPEND ATRINIK_SERVER_TEST_RESOURCE_INPUTS
                ${ATRINIK_GAME_COMMANDS_HEADER})
        endif ()

        add_custom_command(
            OUTPUT "${ATRINIK_SERVER_TEST_RUNTIME_STAMP}"
            COMMAND ${CMAKE_COMMAND}
                ${ATRINIK_SERVER_TEST_RUNTIME_ARGS}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepare_server_test_runtime.cmake
            DEPENDS
                ${ATRINIK_SERVER_TEST_RUNTIME_DEPENDS}
                ${ATRINIK_SERVER_TEST_RESOURCE_INPUTS}
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepare_locked_content_python_unit.cmake
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepare_server_test_runtime.cmake
            COMMENT "Preparing isolated server test runtime..."
            VERBATIM)
        add_custom_target(server-test-runtime-seed
            DEPENDS "${ATRINIK_SERVER_TEST_RUNTIME_STAMP}")
        add_dependencies(atrinik-server-tests server-test-runtime-seed)
        set(ATRINIK_SERVER_TEST_RUNNER
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_isolated_test.py
            --seed ${ATRINIK_SERVER_TEST_RUNTIME_SEED}
            --runtimes ${ATRINIK_SERVER_TEST_RUNTIMES}
            --timeout 270)

        foreach (suite IN LISTS ATRINIK_SERVER_UNIT_SUITES)
            add_test(NAME server-unit-${suite}
                COMMAND ${ATRINIK_SERVER_TEST_RUNNER}
                    --name server-unit-${suite}
                    --
                    $<TARGET_FILE:atrinik-server-tests>
                    --unit
                    --logger_filter_stdout=-info,-devel
                    --port_mapping=off)
            set_tests_properties(server-unit-${suite} PROPERTIES
                ENVIRONMENT "ATRINIK_TEST_SUITE=${suite}"
                LABELS "server;unit"
                TIMEOUT 300
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endforeach ()

        if (ENABLE_SANITIZERS)
            add_test(NAME server-lsan-smoke
                COMMAND ${ATRINIK_SERVER_TEST_RUNNER}
                    --name server-lsan-smoke
                    --
                    $<TARGET_FILE:atrinik-server-tests>
                    --unit
                    --logger_filter_stdout=-info,-devel
                    --port_mapping=off)
            set_tests_properties(server-lsan-smoke PROPERTIES
                ENVIRONMENT
                    "ASAN_OPTIONS=detect_leaks=1;ATRINIK_TEST_SUITE=toolkit.string;CK_FORK=no"
                LABELS "server;unit;sanitizer;lsan"
                TIMEOUT 300
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif ()

        if (TARGET plugin_python)
            add_test(NAME server-plugin-python
                COMMAND ${ATRINIK_SERVER_TEST_RUNNER}
                    --name server-plugin-python
                    --copy-python-maps
                    --
                    ${CMAKE_COMMAND}
                    -DATRINIK_SERVER=$<TARGET_FILE:${EXECUTABLE}>
                    -DATRINIK_RUNTIME_DIR=.
                    -DATRINIK_UNIT_SCRIPT=python_unit_content.py
                    -DATRINIK_EXPECT_FAILURE=false
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_plugin_unit_test.cmake)
            set_tests_properties(server-plugin-python PROPERTIES
                LABELS "server;plugin;runtime"
                TIMEOUT 300
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

            add_test(NAME server-plugin-python-fixture-pass
                COMMAND ${ATRINIK_SERVER_TEST_RUNNER}
                    --name server-plugin-python-fixture-pass
                    --copy-python-maps
                    --
                    ${CMAKE_COMMAND}
                    -DATRINIK_SERVER=$<TARGET_FILE:${EXECUTABLE}>
                    -DATRINIK_RUNTIME_DIR=.
                    -DATRINIK_UNIT_SCRIPT=python_unit_pass.py
                    -DATRINIK_EXPECT_FAILURE=false
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_plugin_unit_test.cmake)
            set_tests_properties(server-plugin-python-fixture-pass PROPERTIES
                LABELS "server;plugin;runtime"
                TIMEOUT 300
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

            add_test(NAME server-plugin-python-fixture-fail
                COMMAND ${ATRINIK_SERVER_TEST_RUNNER}
                    --name server-plugin-python-fixture-fail
                    --copy-python-maps
                    --
                    ${CMAKE_COMMAND}
                    -DATRINIK_SERVER=$<TARGET_FILE:${EXECUTABLE}>
                    -DATRINIK_RUNTIME_DIR=.
                    -DATRINIK_UNIT_SCRIPT=python_unit_fail.py
                    -DATRINIK_EXPECT_FAILURE=true
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_plugin_unit_test.cmake)
            set_tests_properties(server-plugin-python-fixture-fail PROPERTIES
                LABELS "server;plugin;runtime"
                TIMEOUT 300
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif ()

        add_test(NAME server-content-benchmark
            COMMAND ${ATRINIK_SERVER_TEST_RUNNER}
                --name server-content-benchmark
                --
                $<TARGET_FILE:${EXECUTABLE}>
                --content_benchmark=/tests/content_benchmark
                --content_benchmark_iterations=2
                --port_mapping=off)
        set_tests_properties(server-content-benchmark PROPERTIES
            LABELS "server;runtime;benchmark"
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

        add_test(NAME server-assetspath-migration
            COMMAND ${ATRINIK_SERVER_TEST_RUNNER}
                --name server-assetspath-migration
                --
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/assetspath_migration.py
                $<TARGET_FILE:${EXECUTABLE}>)
        set_tests_properties(server-assetspath-migration PROPERTIES
            LABELS "server;runtime"
            PROCESSORS 4
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

        add_custom_target(check
            COMMAND ${CMAKE_CTEST_COMMAND}
                --test-dir ${CMAKE_CURRENT_BINARY_DIR}
                --output-on-failure
                --parallel 4
                -L server
            DEPENDS
                atrinik-server-tests
                ${EXECUTABLE}
                server-test-runtime-seed
            COMMENT "Executing server tests..."
            VERBATIM)
    else ()
        message(WARNING
            "Check was not found; dedicated server tests will not be built.")
    endif ()
endif ()

if (MINGW AND ${PACKAGE_TYPE} STREQUAL "zip")
    if (NOT TARGET plugin_python)
        message(FATAL_ERROR
            "The portable Windows server package requires the Python plugin.")
    endif ()

    set(ATRINIK_WINDOWS_RUNTIME_DIR "" CACHE PATH
        "Directory containing MinGW runtime DLLs")
    set(ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR "" CACHE PATH
        "Directory containing the embedded Windows Python runtime")
    set(ATRINIK_SERVER_PACKAGE_ROOT "" CACHE PATH
        "Prepared maps and collected resources for the server package")

    foreach (required_dir
            ATRINIK_WINDOWS_RUNTIME_DIR
            ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR
            ATRINIK_SERVER_PACKAGE_ROOT)
        if (NOT IS_DIRECTORY "${${required_dir}}")
            message(FATAL_ERROR
                "${required_dir} is not a directory: ${${required_dir}}")
        endif ()
    endforeach ()

    if (NOT IS_DIRECTORY "${ATRINIK_SERVER_PACKAGE_ROOT}/maps" OR
            NOT IS_DIRECTORY "${ATRINIK_SERVER_PACKAGE_ROOT}/lib" OR
            NOT IS_DIRECTORY "${ATRINIK_SERVER_PACKAGE_ROOT}/client-maps")
        message(FATAL_ERROR
            "ATRINIK_SERVER_PACKAGE_ROOT must contain maps/, lib/, and "
            "client-maps/.")
    endif ()

    install(TARGETS ${EXECUTABLE} plugin_arena plugin_python
        RUNTIME DESTINATION server
        LIBRARY DESTINATION server)

    file(GLOB ATRINIK_WINDOWS_RUNTIME_DLLS
        "${ATRINIK_WINDOWS_RUNTIME_DIR}/*.dll")
    list(FILTER ATRINIK_WINDOWS_RUNTIME_DLLS EXCLUDE REGEX
        "/libffi-8\\.dll$")
    if (NOT ATRINIK_WINDOWS_RUNTIME_DLLS)
        message(FATAL_ERROR
            "No DLLs found in ATRINIK_WINDOWS_RUNTIME_DIR: ${ATRINIK_WINDOWS_RUNTIME_DIR}")
    endif ()
    install(FILES ${ATRINIK_WINDOWS_RUNTIME_DLLS} DESTINATION server)

    install(DIRECTORY "${ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR}/"
        DESTINATION server)
    install(DIRECTORY "${ATRINIK_SERVER_PACKAGE_ROOT}/maps"
        DESTINATION server
        PATTERN ".git" EXCLUDE)
    install(DIRECTORY "${ATRINIK_SERVER_PACKAGE_ROOT}/lib/"
        DESTINATION server/lib)
    install(DIRECTORY "${ATRINIK_SERVER_PACKAGE_ROOT}/client-maps"
        DESTINATION server/assets)
    install(DIRECTORY install_data resources tools
        DESTINATION server
        PATTERN ".git" EXCLUDE)
    install(FILES
        README.md
        ${ATRINIK_LICENSE_FILE}
        ${ATRINIK_ATTRIBUTIONS_FILE}
        ca-bundle.crt
        permissions.cfg
        server.bat
        server.cfg
        ${CMAKE_CURRENT_SOURCE_DIR}/server-custom.cfg.example
        DESTINATION server)

    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Atrinik multiplayer online role-playing game server")
    set(CPACK_PACKAGE_VENDOR "Atrinik Development Team")
    set(CPACK_PACKAGE_CONTACT
        "Atrinik Development Team <admin@atrinik.org>")
    set(CPACK_PACKAGE_VERSION_MAJOR "${PACKAGE_VERSION_MAJOR}")
    set(CPACK_PACKAGE_VERSION_MINOR "${PACKAGE_VERSION_MINOR}")
    set(CPACK_PACKAGE_VERSION_PATCH "${PACKAGE_VERSION_PATCH}")
    set(CPACK_PACKAGE_FILE_NAME
        "atrinik-classic-server-${PACKAGE_VERSION}-windows-x86_64")
    set(CPACK_BINARY_ZIP ON)
    set(CPACK_BINARY_STGZ OFF)
    set(CPACK_BINARY_TGZ OFF)
    set(CPACK_BINARY_TZ OFF)
    include(CPack)
endif ()
