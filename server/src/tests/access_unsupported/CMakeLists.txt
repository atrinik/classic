cmake_minimum_required(VERSION 3.21)
project(atrinik-access-unsupported-fixture LANGUAGES C)
include(CTest)
include(CheckIncludeFile)
include(CheckFunctionExists)
get_filename_component(CLASSIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

include("${CLASSIC_ROOT}/server/src/cmake.txt")
if (WIN32)
    if (NOT "src/server/access_unsupported.c" IN_LIST SERVER_CORE_SOURCES OR
            "src/server/access_tokens.c" IN_LIST SERVER_CORE_SOURCES OR
            "src/server/access_server.c" IN_LIST SERVER_CORE_SOURCES OR
            "src/server/access_admin.c" IN_LIST SERVER_CORE_SOURCES)
        message(FATAL_ERROR "Windows must select only the unsupported access backend")
    endif ()
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if ("src/server/access_unsupported.c" IN_LIST SERVER_CORE_SOURCES OR
            NOT "src/server/access_tokens.c" IN_LIST SERVER_CORE_SOURCES OR
            NOT "src/server/access_server.c" IN_LIST SERVER_CORE_SOURCES OR
            NOT "src/server/access_admin.c" IN_LIST SERVER_CORE_SOURCES)
        message(FATAL_ERROR "Linux must retain its checked access backend")
    endif ()
endif ()

# Exercise the production backend with real server settings and API headers.
# Only the toolkit's ordinary generated include layout is staged; there is no
# replacement global.h or mock access API. This is not an application build.
foreach (header fcntl.h unistd.h sys/time.h time.h stddef.h dirent.h sys/socket.h arpa/inet.h)
    string(TOUPPER "${header}" variable)
    string(REGEX REPLACE "[/\.]" "_" variable "${variable}")
    check_include_file("${header}" HAVE_${variable})
endforeach ()
foreach (function strtok_r tempnam strdup strndup strerror strcasestr
        gettimeofday getline usleep strnlen mkstemp sincos)
    string(TOUPPER "${function}" variable)
    check_function_exists("${function}" HAVE_${variable})
endforeach ()
file(GLOB toolkit_headers "${CLASSIC_ROOT}/libatrinik/*.h")
foreach (header IN LISTS toolkit_headers)
    get_filename_component(name "${header}" NAME)
    configure_file("${header}" "include/toolkit/${name}" COPYONLY)
endforeach ()
configure_file("${CLASSIC_ROOT}/libatrinik/cmake.h.def"
    "include/toolkit/toolkit_cmake.h")
add_executable(access-unsupported-fixture fixture.c
    "${CLASSIC_ROOT}/server/src/server/access_unsupported.c")
target_include_directories(access-unsupported-fixture PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${CLASSIC_ROOT}/server/src/include"
    "${CLASSIC_ROOT}/server/src/random_maps"
    "${CLASSIC_ROOT}/server/src/types/include"
    "${CLASSIC_ROOT}/protocol/generated/c/include")
target_compile_features(access-unsupported-fixture PRIVATE c_std_17)
target_compile_definitions(access-unsupported-fixture PRIVATE _GNU_SOURCE)
target_compile_options(access-unsupported-fixture PRIVATE -Wall -Wextra -Werror -UNDEBUG)
if (MINGW)
    target_compile_definitions(access-unsupported-fixture PRIVATE
        HAVE_STRUCT_TIMESPEC NOCRYPT __USE_MINGW_ANSI_STDIO=0)
endif ()
if (NOT CMAKE_CROSSCOMPILING)
    add_test(NAME access-unsupported-matrix COMMAND access-unsupported-fixture)
endif ()
