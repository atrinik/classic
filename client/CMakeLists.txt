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
set(PACKAGE_TYPE "none" CACHE STRING "Package type (none, deb, rpm, nsis, or zip)")

set(PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Semantic version shown by the client and used for packages")
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

atrinik_fetch_source(atrinik_protocol "atrinik/protocol" "atrinik-protocol")
FetchContent_MakeAvailable(atrinik_protocol)
set(LIBATRINIK_PACKAGE_LAYOUT ON CACHE BOOL "" FORCE)
atrinik_fetch_source(libatrinik "atrinik/libatrinik" "libatrinik")
FetchContent_MakeAvailable(libatrinik)

set(EXECUTABLE atrinik)
set(PACKAGE_NAME "Atrinik Client")

# SDL3 is required to run the client. Use its exported CMake targets so Linux
# and MinGW builds share the same dependency contract.
find_package(SDL3 3.4 CONFIG REQUIRED)
set(HAVE_SDL true)

# SDL_image is also required, otherwise no bitmaps or map images could be
# displayed.
find_package(SDL3_image 3.2 CONFIG REQUIRED)
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
    if (NOT CMAKE_OBJDUMP)
        message(FATAL_ERROR
            "CMAKE_OBJDUMP is required to validate Windows client sound support")
    endif ()

    add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DATRINIK_OBJDUMP=${CMAKE_OBJDUMP}"
            "-DATRINIK_CLIENT_EXECUTABLE=$<TARGET_FILE:${EXECUTABLE}>"
            -P "${CMAKE_SOURCE_DIR}/cmake/verify_windows_client_sound.cmake"
        COMMENT "Verifying that the Windows client imports SDL3_mixer"
        VERBATIM)
endif ()

if (BUILD_TESTING AND NOT MINGW)
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

    add_executable(client-keybind-input-tests
        src/tests/keybind_input.c
        src/client/keybind_input.c)
    atrinik_configure_target(client-keybind-input-tests)
    target_compile_definitions(client-keybind-input-tests PRIVATE SDL_DISABLE_OLD_NAMES)
    target_include_directories(client-keybind-input-tests BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/toolkit/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(client-keybind-input-tests PRIVATE Atrinik::Core SDL3::SDL3)
    add_test(NAME client-keybind-input COMMAND client-keybind-input-tests)

    add_executable(client-surface-primitives-tests
        src/tests/surface_primitives.c
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

    set(player_view_fixtures
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/player_view")
    add_test(NAME client-player-view-smooth
        COMMAND atrinik --player-view "${player_view_fixtures}/smooth.xml" -)
    add_test(NAME client-player-view-discrete
        COMMAND atrinik --player-view "${player_view_fixtures}/discrete.xml" -)
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
        ${CMAKE_SOURCE_DIR}/INSTALL
        ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        ${ATRINIK_LICENSE_FILE}
        ${ATRINIK_ATTRIBUTIONS_FILE}
        ${CMAKE_CURRENT_SOURCE_DIR}/client.cfg
        DESTINATION ${INSTALL_SUBDIR_DOC})
install(DIRECTORY
        textures cache data fonts gfx_user settings sound srv_files
        DESTINATION ${INSTALL_SUBDIR_SHARE}
        PATTERN ".git" EXCLUDE)

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
