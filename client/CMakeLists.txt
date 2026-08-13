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

project(atrinik-client VERSION "${ATRINIK_SOURCE_VERSION}" LANGUAGES C)

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
    option(ATRINIK_CLIENT_ENABLE_PRECOMPILED_HEADERS
        "Precompile the client global header for the main executable" ON)
else ()
    option(ENABLE_PRECOMPILED_HEADERS
        "Precompile the client global header for the main executable" ON)
    set(ATRINIK_CLIENT_ENABLE_PRECOMPILED_HEADERS
        "${ENABLE_PRECOMPILED_HEADERS}")
endif ()
set(PACKAGE_TYPE "none" CACHE STRING "Package type (none, deb, rpm, nsis, or zip)")
set(ATRINIK_DISCORD_APPLICATION_ID_FILE "" CACHE FILEPATH
    "Optional file containing the public Discord Application ID")

if (ATRINIK_DISCORD_APPLICATION_ID_FILE)
    if (NOT EXISTS "${ATRINIK_DISCORD_APPLICATION_ID_FILE}")
        message(FATAL_ERROR "ATRINIK_DISCORD_APPLICATION_ID_FILE does not exist")
    endif ()
    file(STRINGS "${ATRINIK_DISCORD_APPLICATION_ID_FILE}"
        ATRINIK_DISCORD_APPLICATION_ID_LINES LIMIT_COUNT 2)
    list(LENGTH ATRINIK_DISCORD_APPLICATION_ID_LINES
        ATRINIK_DISCORD_APPLICATION_ID_LINE_COUNT)
    if (NOT ATRINIK_DISCORD_APPLICATION_ID_LINE_COUNT EQUAL 1)
        message(FATAL_ERROR "Discord Application ID file must contain exactly one line")
    endif ()
    list(GET ATRINIK_DISCORD_APPLICATION_ID_LINES 0
        ATRINIK_DISCORD_APPLICATION_ID_VALUE)
    string(LENGTH "${ATRINIK_DISCORD_APPLICATION_ID_VALUE}"
        ATRINIK_DISCORD_APPLICATION_ID_LENGTH)
    if (ATRINIK_DISCORD_APPLICATION_ID_LENGTH LESS 1 OR
            ATRINIK_DISCORD_APPLICATION_ID_LENGTH GREATER 20 OR
            NOT ATRINIK_DISCORD_APPLICATION_ID_VALUE MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Discord Application ID file is invalid")
    endif ()
    if (ATRINIK_DISCORD_APPLICATION_ID_LENGTH EQUAL 20 AND
            ATRINIK_DISCORD_APPLICATION_ID_VALUE STRGREATER "18446744073709551615")
        message(FATAL_ERROR "Discord Application ID exceeds uint64 range")
    endif ()
    set(ATRINIK_DISCORD_APPLICATION_ID_NORMALIZED
        "${CMAKE_CURRENT_BINARY_DIR}/discord-application-id")
    file(WRITE "${ATRINIK_DISCORD_APPLICATION_ID_NORMALIZED}"
        "${ATRINIK_DISCORD_APPLICATION_ID_VALUE}\n")
    unset(ATRINIK_DISCORD_APPLICATION_ID_LINES)
endif ()

set(PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Semantic version shown by the client and used for packages")
if (NOT PACKAGE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "PACKAGE_VERSION must be a three-part semantic version")
endif ()
set(PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(PACKAGE_VERSION_PATCH "${CMAKE_MATCH_3}")
set(ATRINIK_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if (ATRINIK_BUILD_TYPE STREQUAL "")
    set(ATRINIK_BUILD_TYPE "multi-config")
endif ()
set(ATRINIK_COMPILER_ID "${CMAKE_C_COMPILER_ID}")
set(ATRINIK_COMPILER_VERSION "${CMAKE_C_COMPILER_VERSION}")
set(ATRINIK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}")
set(ATRINIK_BENCHMARK_REVISION "$ENV{ATRINIK_BENCHMARK_REVISION}")
if (ATRINIK_BENCHMARK_REVISION STREQUAL "")
    set(ATRINIK_BENCHMARK_REVISION "unknown")
endif ()
string(LENGTH "${ATRINIK_BENCHMARK_REVISION}" ATRINIK_BENCHMARK_REVISION_LENGTH)
if (NOT ATRINIK_BENCHMARK_REVISION STREQUAL "unknown" AND
    (NOT ATRINIK_BENCHMARK_REVISION MATCHES "^[0-9A-Fa-f]+$" OR
     ATRINIK_BENCHMARK_REVISION_LENGTH LESS 7 OR
     ATRINIK_BENCHMARK_REVISION_LENGTH GREATER 64))
    message(FATAL_ERROR "ATRINIK_BENCHMARK_REVISION must be unknown or a hexadecimal revision")
endif ()
set(ATRINIK_BENCHMARK_DIRTY "$ENV{ATRINIK_BENCHMARK_DIRTY}")
if (ATRINIK_BENCHMARK_DIRTY STREQUAL "")
    set(ATRINIK_BENCHMARK_DIRTY "unknown")
endif ()
if (NOT ATRINIK_BENCHMARK_DIRTY MATCHES "^(unknown|true|false)$")
    message(FATAL_ERROR "ATRINIK_BENCHMARK_DIRTY must be unknown, true, or false")
endif ()

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
            --coverage -fprofile-abs-path -O0 -g)
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
if (NOT ATRINIK_PROTOCOL_VERSION_LINE MATCHES " 1079U$")
    message(FATAL_ERROR "Client requires classic game protocol v1079")
endif ()
if (NOT TARGET Atrinik::Core)
    set(LIBATRINIK_PACKAGE_LAYOUT ON CACHE BOOL "" FORCE)
    atrinik_fetch_source(libatrinik "atrinik/libatrinik" "libatrinik")
    FetchContent_MakeAvailable(libatrinik)
endif ()

set(EXECUTABLE atrinik)
set(PACKAGE_NAME "Atrinik Client")

# SDL3 is required to run the client. Use its exported CMake targets so Linux
# and MinGW builds share the same dependency contract.
find_package(SDL3 3.4 CONFIG REQUIRED)
set(HAVE_SDL true)

# SDL_image is also required, otherwise no bitmaps or map images could be
# displayed.
find_package(SDL3_image 3.2 CONFIG REQUIRED)
if (SDLIMAGE_BACKEND_WIC)
    message(FATAL_ERROR
        "The asynchronous face loader requires a thread-safe SDL_image backend; WIC uses "
        "process-global COM state and is not supported")
endif ()
set(HAVE_SDL_IMAGE true)

# SDL_ttf is used for all text drawing, so it's required.
find_package(SDL3_ttf 3.2 CONFIG REQUIRED)
set(HAVE_SDL_TTF true)

# LibXML2 is required.
find_package(LibXml2 REQUIRED)

# Sound effects and music are core client functionality on every platform.
find_package(SDL3_mixer 3.2.4 CONFIG REQUIRED)
set(HAVE_SDL_MIXER true)

# Add sources.
include(src/cmake.txt)

if (MINGW)
    ADD_CUSTOM_COMMAND(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/atrinikrc.o
            COMMAND ${CMAKE_RC_COMPILER} -I${CMAKE_CURRENT_SOURCE_DIR}
            -i${CMAKE_CURRENT_SOURCE_DIR}/atrinik.rc
            -o ${CMAKE_CURRENT_BINARY_DIR}/atrinikrc.o
            VERBATIM)
    SET(SOURCES ${SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/atrinikrc.o")
endif ()

# Compile the sources into an executable.
add_executable(${EXECUTABLE} WIN32 ${SOURCES})
atrinik_configure_target(${EXECUTABLE})
if (ATRINIK_CLIENT_ENABLE_PRECOMPILED_HEADERS)
    target_precompile_headers(${EXECUTABLE} PRIVATE src/include/global.h)
endif ()
target_compile_definitions(${EXECUTABLE} PRIVATE SDL_DISABLE_OLD_NAMES)
target_include_directories(${EXECUTABLE} BEFORE PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBXML2_INCLUDE_DIR})

target_link_libraries(${EXECUTABLE} PRIVATE Atrinik::Core)

# Link all the available libraries.
target_link_libraries(${EXECUTABLE} PRIVATE
    SDL3::SDL3
    SDL3_image::SDL3_image
    SDL3_ttf::SDL3_ttf)
if (TARGET LibXml2::LibXml2)
    target_link_libraries(${EXECUTABLE} PRIVATE LibXml2::LibXml2)
else ()
    target_link_libraries(${EXECUTABLE} PRIVATE ${LIBXML2_LIBRARIES})
endif ()

# Sound effects and music are required on every supported platform.
target_compile_definitions(${EXECUTABLE} PRIVATE HAVE_SDL_MIXER)
target_link_libraries(${EXECUTABLE} PRIVATE SDL3_mixer::SDL3_mixer)
if (MINGW)
    target_link_libraries(${EXECUTABLE} PRIVATE advapi32)
endif ()

if (MINGW)
    if (NOT CMAKE_OBJDUMP)
        message(FATAL_ERROR
            "CMAKE_OBJDUMP is required to validate Windows client sound support")
    endif ()

    add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DATRINIK_OBJDUMP=${CMAKE_OBJDUMP}"
            "-DATRINIK_CLIENT_EXECUTABLE=$<TARGET_FILE:${EXECUTABLE}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_windows_client_sound.cmake"
        COMMENT "Verifying that the Windows client imports SDL3_mixer"
        VERBATIM)
endif ()

if (BUILD_TESTING)
    find_program(ATRINIK_PYTHON3_EXECUTABLE python3 REQUIRED)
    add_test(NAME client-lighting-lut-generated
        COMMAND "${ATRINIK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_lighting_lut.py" --check)

    add_executable(client-lighting-transfer-tests
        src/tests/lighting_transfer.c
        src/client/lighting_transfer.c)
    atrinik_configure_target(client-lighting-transfer-tests)
    target_include_directories(client-lighting-transfer-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    add_test(NAME client-lighting-transfer COMMAND client-lighting-transfer-tests)

    add_executable(client-rich-presence-tests
        src/tests/rich_presence.c
        src/client/discord_rpc.c
        src/client/rich_presence_policy.c)
    atrinik_configure_target(client-rich-presence-tests)
    target_include_directories(client-rich-presence-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    if (MINGW)
        target_compile_definitions(client-rich-presence-tests PRIVATE DISCORD_RPC_TESTING)
        target_link_libraries(client-rich-presence-tests PRIVATE advapi32)
    endif ()
    add_test(NAME client-rich-presence COMMAND client-rich-presence-tests)
    set_tests_properties(client-rich-presence PROPERTIES TIMEOUT 10)

    if (NOT MINGW)
        target_compile_definitions(${EXECUTABLE} PRIVATE ATRINIK_WIDGET_TESTS)

    add_executable(client-stun-config-tests
        src/tests/stun_config.c
        src/client/stun_config.c)
    atrinik_configure_target(client-stun-config-tests)
    target_include_directories(client-stun-config-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    target_link_libraries(client-stun-config-tests PRIVATE Atrinik::Core)
    add_test(NAME client-stun-config COMMAND client-stun-config-tests)

    add_executable(client-setting-value-tests
        src/tests/setting_value.c
        src/client/setting_value.c)
    atrinik_configure_target(client-setting-value-tests)
    target_include_directories(client-setting-value-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    target_link_libraries(client-setting-value-tests PRIVATE Atrinik::Core)
    add_test(NAME client-setting-value COMMAND client-setting-value-tests)

    add_executable(client-metaserver-options-tests
        src/tests/metaserver_options.c
        src/client/metaserver_options.c)
    atrinik_configure_target(client-metaserver-options-tests)
    target_include_directories(client-metaserver-options-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    target_link_libraries(client-metaserver-options-tests PRIVATE Atrinik::Core)
    add_test(NAME client-metaserver-options COMMAND client-metaserver-options-tests)

    add_executable(client-window-title-tests
        src/tests/window_title.c
        src/client/window_title.c)
    atrinik_configure_target(client-window-title-tests)
    target_include_directories(client-window-title-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    target_link_libraries(client-window-title-tests PRIVATE SDL3::SDL3)
    add_test(NAME client-window-title COMMAND client-window-title-tests)

    add_executable(client-metaserver-directory-tests
        src/tests/metaserver_directory.c
        src/client/metaserver_directory.c)
    atrinik_configure_target(client-metaserver-directory-tests)
    target_include_directories(client-metaserver-directory-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-metaserver-directory-tests PRIVATE Atrinik::Core)
    if (TARGET LibXml2::LibXml2)
        target_link_libraries(client-metaserver-directory-tests PRIVATE LibXml2::LibXml2)
    else ()
        target_include_directories(client-metaserver-directory-tests PRIVATE ${LIBXML2_INCLUDE_DIR})
        target_link_libraries(client-metaserver-directory-tests PRIVATE ${LIBXML2_LIBRARIES})
    endif ()
    add_test(NAME client-metaserver-directory
        COMMAND client-metaserver-directory-tests
            "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/classic-directory-v4.xml")

    add_executable(client-connection-failure-tests
        src/tests/connection_failure.c
        src/client/connection_failure.c
        src/client/join_credentials.c
        src/client/metaserver_identity.c)
    atrinik_configure_target(client-connection-failure-tests)
    target_include_directories(client-connection-failure-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    target_link_libraries(client-connection-failure-tests PRIVATE Atrinik::Core)
    add_test(NAME client-connection-failure COMMAND client-connection-failure-tests)

    add_executable(client-packet-payload-tests
        src/tests/packet_payload.c
        src/client/packet_payload.c)
    atrinik_configure_target(client-packet-payload-tests)
    target_include_directories(client-packet-payload-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    target_link_libraries(client-packet-payload-tests PRIVATE Atrinik::Core)
    add_test(NAME client-packet-payload COMMAND client-packet-payload-tests)

    add_executable(client-command-queue-tests
        src/tests/client_command_queue.c
        src/client/client_command_queue.c)
    atrinik_configure_target(client-command-queue-tests)
    target_compile_definitions(client-command-queue-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-command-queue-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-command-queue-tests PRIVATE Atrinik::Core SDL3::SDL3)
    add_test(NAME client-command-queue COMMAND client-command-queue-tests)

    add_executable(client-keybind-input-tests
        src/tests/keybind_input.c
        src/client/keybind_event.c
        src/client/keybind_input.c
        src/client/keybind_storage.c
        src/client/upgrader.c)
    atrinik_configure_target(client-keybind-input-tests)
    target_compile_definitions(client-keybind-input-tests PRIVATE
        ATRINIK_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        ATRINIK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-keybind-input-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-keybind-input-tests PRIVATE Atrinik::Core SDL3::SDL3)
    add_test(NAME client-keybind-input COMMAND client-keybind-input-tests)

    add_executable(client-move-stream-tests
        src/tests/move_stream.c
        src/client/keybind_input.c
        src/events/move.c)
    atrinik_configure_target(client-move-stream-tests)
    target_compile_definitions(client-move-stream-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-move-stream-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-move-stream-tests PRIVATE Atrinik::Core SDL3::SDL3)
    add_test(NAME client-move-stream COMMAND client-move-stream-tests)

    add_executable(client-compound-packet-tests
        src/tests/compound_packet.c
        src/client/item_packet.c
        src/gui/popups/interface_packet.c)
    atrinik_configure_target(client-compound-packet-tests)
    target_compile_definitions(client-compound-packet-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-compound-packet-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-compound-packet-tests PRIVATE
        Atrinik::Core
        SDL3::SDL3
        SDL3_image::SDL3_image
        SDL3_ttf::SDL3_ttf)
    add_test(NAME client-compound-packet COMMAND client-compound-packet-tests)

    add_executable(client-player-status-tests
        src/tests/player_status.c
        src/client/player_status.c
        src/gui/widgets/active_effects_model.c)
    atrinik_configure_target(client-player-status-tests)
    target_compile_definitions(client-player-status-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-player-status-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-player-status-tests PRIVATE
        Atrinik::Core
        SDL3::SDL3)
    add_test(NAME client-player-status COMMAND client-player-status-tests)

    add_executable(client-face-asset-tests
        src/tests/face_asset.c
        src/client/face_asset.c)
    atrinik_configure_target(client-face-asset-tests)
    target_include_directories(client-face-asset-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    target_link_libraries(client-face-asset-tests PRIVATE Atrinik::Core)
    add_test(NAME client-face-asset COMMAND client-face-asset-tests)

    add_executable(client-face-cache-tests
        src/tests/face_cache.c
        src/client/face_cache.c)
    atrinik_configure_target(client-face-cache-tests)
    target_compile_definitions(client-face-cache-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-face-cache-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-face-cache-tests PRIVATE Atrinik::Core SDL3::SDL3)
    add_test(NAME client-face-cache COMMAND client-face-cache-tests)

    add_executable(client-face-request-tests
        src/tests/face_request.c
        src/client/image.c
        src/client/face_asset.c
        src/client/face_loader.c
        src/client/image_codec.c)
    atrinik_configure_target(client-face-request-tests)
    target_compile_definitions(client-face-request-tests PRIVATE
        ATRINIK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        ATRINIK_FACE_REQUEST_TESTING
        ATRINIK_IMAGE_CODEC_TESTING
        FACE_URGENT_MAX_AGE_MS=5
        FACE_TRANSFER_RETRY_BASE_MS=1
        SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-face-request-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-face-request-tests PRIVATE
        Atrinik::Core
        SDL3::SDL3
        SDL3_image::SDL3_image)
    add_test(NAME client-face-request COMMAND client-face-request-tests)

    add_executable(client-image-codec-tests
        src/tests/image_codec.c
        src/client/image_codec.c)
    atrinik_configure_target(client-image-codec-tests)
    target_compile_definitions(client-image-codec-tests PRIVATE
        ATRINIK_IMAGE_CODEC_TESTING
        SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-image-codec-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-image-codec-tests PRIVATE
        Atrinik::Core
        SDL3::SDL3
        SDL3_image::SDL3_image)
    add_test(NAME client-image-codec COMMAND client-image-codec-tests)
    set_tests_properties(client-image-codec PROPERTIES TIMEOUT 10)

    add_executable(client-face-stream-tests
        src/tests/face_stream.c
        src/client/asset.c)
    atrinik_configure_target(client-face-stream-tests)
    target_compile_definitions(client-face-stream-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-face-stream-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-face-stream-tests PRIVATE
        Atrinik::Core
        SDL3::SDL3)
    add_test(NAME client-face-stream COMMAND client-face-stream-tests)
    set_tests_properties(client-face-stream PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)

    add_executable(client-asset-batch-tests
        src/tests/asset_batch.c
        src/client/asset.c)
    atrinik_configure_target(client-asset-batch-tests)
    target_compile_definitions(client-asset-batch-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-asset-batch-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-asset-batch-tests PRIVATE
        Atrinik::Core
        SDL3::SDL3)
    add_test(NAME client-asset-batch COMMAND client-asset-batch-tests)
    set_tests_properties(client-asset-batch PROPERTIES TIMEOUT 10)

    add_executable(client-surface-primitives-tests
        src/tests/surface_primitives.c
        src/client/sprite_pixels.c
        src/gui/toolkit/surface_primitives.c)
    atrinik_configure_target(client-surface-primitives-tests)
    target_compile_definitions(client-surface-primitives-tests PRIVATE
        ATRINIK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-surface-primitives-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-surface-primitives-tests PRIVATE
        Atrinik::Core
        SDL3::SDL3
        SDL3_image::SDL3_image)
    add_test(NAME client-surface-primitives COMMAND client-surface-primitives-tests)

    add_executable(client-text-color-tests
        src/tests/text_color.c
        src/gui/toolkit/text_color.c)
    atrinik_configure_target(client-text-color-tests)
    target_compile_definitions(client-text-color-tests PRIVATE
        ATRINIK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-text-color-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-text-color-tests PRIVATE
        Atrinik::Core
        SDL3::SDL3
        SDL3_ttf::SDL3_ttf)
    add_test(NAME client-text-color COMMAND client-text-color-tests)

    add_executable(client-textwin-event-tests
        src/tests/textwin_event.c
        src/gui/widgets/textwin_event.c)
    atrinik_configure_target(client-textwin-event-tests)
    target_compile_definitions(client-textwin-event-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-textwin-event-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-textwin-event-tests PRIVATE Atrinik::Core SDL3::SDL3)
    add_test(NAME client-textwin-event COMMAND client-textwin-event-tests)

    add_executable(client-widget-priority-tests
        src/tests/widget_priority.c
        src/gui/toolkit/widget_priority.c)
    atrinik_configure_target(client-widget-priority-tests)
    target_compile_definitions(client-widget-priority-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-widget-priority-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-widget-priority-tests PRIVATE Atrinik::Core SDL3::SDL3)
    add_test(NAME client-widget-priority COMMAND client-widget-priority-tests)

    add_executable(client-map-light-state-tests src/tests/map_light_state.c)
    atrinik_configure_target(client-map-light-state-tests)
    target_compile_definitions(client-map-light-state-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-map-light-state-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-map-light-state-tests PRIVATE Atrinik::Core SDL3::SDL3)
    add_test(NAME client-map-light-state COMMAND client-map-light-state-tests)

    add_executable(client-playerdoll-equipment-tests
        src/tests/playerdoll_equipment.c
        src/gui/widgets/playerdoll_equipment.c)
    atrinik_configure_target(client-playerdoll-equipment-tests)
    target_compile_definitions(client-playerdoll-equipment-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-playerdoll-equipment-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-playerdoll-equipment-tests PRIVATE Atrinik::Core SDL3::SDL3)
    add_test(NAME client-playerdoll-equipment COMMAND client-playerdoll-equipment-tests)

    add_test(NAME client-widget-priority-integration
        COMMAND atrinik --widget-priority-test
            "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/widget-priority-interface.cfg"
            "${CMAKE_CURRENT_BINARY_DIR}/widget-priority-interface-saved.cfg")
    set_tests_properties(client-widget-priority-integration PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    set(player_view_fixtures
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/player_view")
    add_test(NAME client-player-view-smooth
        COMMAND atrinik --player-view "${player_view_fixtures}/smooth.xml" -)
    add_test(NAME client-player-view-smooth-player-sublayer
        COMMAND atrinik --player-view
            "${player_view_fixtures}/smooth-player-sublayer-1.xml" -)
    add_test(NAME client-player-view-discrete
        COMMAND atrinik --player-view "${player_view_fixtures}/discrete.xml" -)
    add_test(NAME client-player-view-radial-light-smooth
        COMMAND atrinik --player-view
            "${player_view_fixtures}/radial-light-smooth.xml" -)
    add_test(NAME client-player-view-radial-light-discrete
        COMMAND atrinik --player-view
            "${player_view_fixtures}/radial-light-discrete.xml" -)
    add_test(NAME client-player-view-exit-cues
        COMMAND atrinik --player-view "${player_view_fixtures}/exit-cues.xml" -)
    add_test(NAME client-player-view-player-outline-unobscured
        COMMAND atrinik --player-view
            "${player_view_fixtures}/player-outline-unobscured.xml" -)
    add_test(NAME client-player-view-player-outline-same-level
        COMMAND atrinik --player-view
            "${player_view_fixtures}/player-outline-same-level.xml" -)
    add_test(NAME client-player-view-player-outline-upper-level
        COMMAND atrinik --player-view
            "${player_view_fixtures}/player-outline-upper-level.xml" -)
    add_test(NAME client-player-view-player-outline-nearby-living
        COMMAND atrinik --player-view
            "${player_view_fixtures}/player-outline-nearby-living.xml" -)
    add_test(NAME client-player-view-player-outline-non-primary
        COMMAND atrinik --player-view
            "${player_view_fixtures}/player-outline-non-primary.xml" -)
    add_test(NAME client-player-view-player-outline-widget-state
        COMMAND atrinik --player-view
            "${player_view_fixtures}/player-outline-widget-state.xml" -)
    add_test(NAME client-player-view-living-outline-unobscured
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-unobscured.xml" -)
    add_test(NAME client-player-view-living-outline-same-level
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-same-level.xml" -)
    add_test(NAME client-player-view-living-outline-upper-level
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-upper-level.xml" -)
    add_test(NAME client-player-view-living-outline-nearby-wall
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-nearby-wall.xml" -)
    add_test(NAME client-player-view-living-outline-translucent
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-translucent.xml" -)
    add_test(NAME client-player-view-living-outline-double
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-double.xml" -)
    add_test(NAME client-player-view-living-outline-dark-actor
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-dark-actor.xml" -)
    add_test(NAME client-player-view-living-outline-dark-wall
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-dark-wall.xml" -)
    add_test(NAME client-player-view-living-outline-multiple
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-multiple.xml" -)
    add_test(NAME client-player-view-living-outline-crowded
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-crowded.xml" -)
    add_test(NAME client-player-view-living-outline-crowded-unobscured
        COMMAND atrinik --player-view
            "${player_view_fixtures}/living-outline-crowded-unobscured.xml" -)
    add_test(NAME client-player-view-movement-benchmark-smooth
        COMMAND "${ATRINIK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_movement_benchmark.py"
            "$<TARGET_FILE:atrinik>"
            "${player_view_fixtures}/movement-colored.xml"
            standard)
    add_test(NAME client-player-view-movement-benchmark-discrete
        COMMAND "${ATRINIK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_movement_benchmark.py"
            "$<TARGET_FILE:atrinik>"
            "${player_view_fixtures}/movement-colored-discrete.xml"
            standard)
    add_test(NAME client-player-view-movement-benchmark-smooth-large
        COMMAND "${ATRINIK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_movement_benchmark.py"
            "$<TARGET_FILE:atrinik>"
            "${player_view_fixtures}/movement-colored.xml"
            large)
    add_test(NAME client-player-view-movement-benchmark-discrete-large
        COMMAND "${ATRINIK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_movement_benchmark.py"
            "$<TARGET_FILE:atrinik>"
            "${player_view_fixtures}/movement-colored-discrete.xml"
            large)
    add_test(NAME client-player-view-movement-fault-injection
        COMMAND "${ATRINIK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_movement_fault_injection.py"
            "$<TARGET_FILE:atrinik>"
            "${player_view_fixtures}/colored-smooth.xml"
            "${player_view_fixtures}/movement-colored.xml")
    set_tests_properties(
        client-player-view-movement-benchmark-smooth
        client-player-view-movement-benchmark-discrete
        PROPERTIES
        LABELS "performance;standard-performance"
        TIMEOUT 420)
    set_tests_properties(
        client-player-view-movement-benchmark-smooth-large
        client-player-view-movement-benchmark-discrete-large
        PROPERTIES
        LABELS "performance;long-performance"
        TIMEOUT 1900)
    set_tests_properties(client-player-view-movement-fault-injection PROPERTIES
        LABELS fault-injection
        TIMEOUT 75)
    add_test(NAME client-player-view-colored-smooth
        COMMAND atrinik --player-view "${player_view_fixtures}/colored-smooth.xml" -)
    add_test(NAME client-player-view-colored-discrete
        COMMAND atrinik --player-view "${player_view_fixtures}/colored-discrete.xml" -)
    add_test(NAME client-player-view-high-key-smooth
        COMMAND atrinik --player-view "${player_view_fixtures}/high-key-smooth.xml" -)
    add_test(NAME client-player-view-high-key-discrete
        COMMAND atrinik --player-view "${player_view_fixtures}/high-key-discrete.xml" -)
    add_test(NAME client-player-view-output
        COMMAND ${CMAKE_COMMAND}
            "-DATRINIK_CLIENT=$<TARGET_FILE:atrinik>"
            "-DPLAYER_VIEW_MANIFEST=${player_view_fixtures}/smooth.xml"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_player_view.cmake")
    add_test(NAME client-player-view-malformed-snapshot
        COMMAND atrinik --player-view "${player_view_fixtures}/malformed.xml" -)
    add_test(NAME client-player-view-mismatched-asset
        COMMAND atrinik --player-view "${player_view_fixtures}/mismatched-asset.xml" -)
    add_test(NAME client-player-view-missing-face
        COMMAND atrinik --player-view "${player_view_fixtures}/missing-face.xml" -)
    add_test(NAME client-player-view-unknown-field
        COMMAND atrinik --player-view "${player_view_fixtures}/unknown-field.xml" -)
    set_tests_properties(
        client-player-view-malformed-snapshot
        client-player-view-mismatched-asset
        client-player-view-missing-face
        client-player-view-unknown-field
        PROPERTIES WILL_FAIL TRUE)
    endif ()
endif ()

# Installer.
if (WIN32 AND NOT UNIX)
    set(INSTALL_SUBDIR_BIN ".")
    set(INSTALL_SUBDIR_SHARE ".")
    set(INSTALL_SUBDIR_DOC ".")

    set(ATRINIK_WINDOWS_RUNTIME_DIR "" CACHE PATH
        "Directory containing MinGW runtime DLLs for portable packages")
    set(ATRINIK_CA_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/ca-bundle.crt" CACHE FILEPATH
        "CA certificate bundle to install with the client")

    install(TARGETS atrinik
        RUNTIME DESTINATION ${INSTALL_SUBDIR_BIN})

    if (ATRINIK_WINDOWS_RUNTIME_DIR)
        file(GLOB ATRINIK_WINDOWS_RUNTIME_DLLS
            "${ATRINIK_WINDOWS_RUNTIME_DIR}/*.dll")

        if (ATRINIK_WINDOWS_RUNTIME_DLLS)
            install(FILES ${ATRINIK_WINDOWS_RUNTIME_DLLS}
                DESTINATION ${INSTALL_SUBDIR_BIN})
        else ()
            message(FATAL_ERROR
                "No DLLs found in ATRINIK_WINDOWS_RUNTIME_DIR: ${ATRINIK_WINDOWS_RUNTIME_DIR}")
        endif ()
    elseif (PACKAGE_TYPE STREQUAL "zip")
        message(WARNING "ATRINIK_WINDOWS_RUNTIME_DIR is empty; portable packages will not include dependency DLLs")
    endif ()

    install(FILES "${ATRINIK_CA_BUNDLE}" DESTINATION ${INSTALL_SUBDIR_BIN}
        RENAME ca-bundle.crt)

    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/timidity.cfg")
        install(FILES timidity.cfg DESTINATION ${INSTALL_SUBDIR_BIN})
    endif ()
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/timidity")
        install(DIRECTORY timidity DESTINATION ${INSTALL_SUBDIR_BIN})
    endif ()
else ()
    set(INSTALL_SUBDIR_BIN "games")
    set(INSTALL_SUBDIR_SHARE "share/games/atrinik")
    set(INSTALL_SUBDIR_DOC "share/doc/atrinik")

    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/atrinik.desktop DESTINATION "share/applications")
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/textures/icon.png DESTINATION "share/pixmaps" RENAME "atrinik.png")
endif ()

if (NOT WIN32 OR UNIX)
    install(TARGETS atrinik DESTINATION ${INSTALL_SUBDIR_BIN})
endif ()
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/INSTALL
        ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        ${ATRINIK_LICENSE_FILE}
        ${ATRINIK_ATTRIBUTIONS_FILE}
        ${CMAKE_CURRENT_SOURCE_DIR}/client.cfg
        DESTINATION ${INSTALL_SUBDIR_DOC})
install(DIRECTORY
        textures cache data fonts gfx_user settings sound srv_files
        DESTINATION ${INSTALL_SUBDIR_SHARE}
        PATTERN ".git" EXCLUDE
        PATTERN "discord-application-id" EXCLUDE)
if (ATRINIK_DISCORD_APPLICATION_ID_FILE)
    install(FILES "${ATRINIK_DISCORD_APPLICATION_ID_NORMALIZED}"
        DESTINATION "${INSTALL_SUBDIR_SHARE}/data"
        RENAME discord-application-id)
endif ()

include(InstallRequiredSystemLibraries)

set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fantasy multiplayer online role-playing game")
set(CPACK_PACKAGE_VENDOR "Atrinik Development Team")
set(CPACK_PACKAGE_CONTACT "Atrinik Development Team <admin@atrinik.org>")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${ATRINIK_LICENSE_FILE}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PACKAGE_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PACKAGE_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PACKAGE_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Atrinik")

if (MINGW)
    set(CPACK_PACKAGE_EXECUTABLES "atrinik" "Atrinik")
    set(CPACK_CREATE_DESKTOP_LINKS "atrinik")
endif ()

if (MINGW)
    set(CPACK_PACKAGE_FILE_NAME
        "atrinik-classic-client-${PACKAGE_VERSION}-windows-x86_64")
endif ()

set(CPACK_DEBIAN_PACKAGE_NAME "atrinik")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libgcc-s1, libsdl3-0, libsdl3-image0, libsdl3-ttf0, libcurl4, timidity, zlib1g")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Fantasy multiplayer online role-playing game with 2.5D graphics\n .\n Homepage: http://www.atrinik.org/")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")

set(CPACK_RPM_PACKAGE_NAME "atrinik")
set(CPACK_RPM_PACKAGE_DESCRIPTION "Fantasy multiplayer online role-playing game with 2.5D graphics\n .\n Homepage: http://www.atrinik.org/")
set(CPACK_RPM_PACKAGE_LICENSE "GNU General Public License (GPL)")
set(CPACK_RPM_PACKAGE_GROUP "Games")

set(CPACK_NSIS_INSTALLED_ICON_NAME "atrinik")
set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/textures/icon.ico")
set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/textures/icon.ico")
set(CPACK_NSIS_DISPLAY_NAME "Atrinik")
set(CPACK_NSIS_MODIFY_PATH "ON")
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")

if (${PACKAGE_TYPE} STREQUAL "deb")
    set(CPACK_BINARY_DEB "ON")
elseif (${PACKAGE_TYPE} STREQUAL "nsis")
    set(CPACK_BINARY_NSIS "ON")
elseif (${PACKAGE_TYPE} STREQUAL "rpm")
    set(CPACK_BINARY_RPM "ON")
elseif (${PACKAGE_TYPE} STREQUAL "zip")
    set(CPACK_BINARY_ZIP "ON")
endif (${PACKAGE_TYPE} STREQUAL "deb")

set(CPACK_BINARY_STGZ "OFF")
set(CPACK_BINARY_TGZ "OFF")
set(CPACK_BINARY_TZ "OFF")
include(CPack)

# Configure the .h file with the configuration options (size of long,
# package version, etc).
configure_file(src/include/cmake.h.def src/include/cmake.h)
configure_file(src/include/version.h.def src/include/version.h)
