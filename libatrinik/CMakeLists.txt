cmake_minimum_required(VERSION 3.21)

# This directory is both an in-tree dependency of the classic game superbuild and
# the source root of the independently released libatrinik package.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ATRINIK_SOURCE_VERSION "1.0.0")
    if (DEFINED ENV{ATRINIK_PACKAGE_VERSION})
        set(ATRINIK_SOURCE_VERSION "$ENV{ATRINIK_PACKAGE_VERSION}")
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
            ATRINIK_SOURCE_VERSION LIMIT_COUNT 1)
    else ()
        execute_process(
            COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}"
                describe --tags --exact-match --match "v[0-9]*"
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

    project(libatrinik VERSION "${ATRINIK_SOURCE_VERSION}" LANGUAGES C)

    include(CTest)
    include(CMakePackageConfigHelpers)

    option(ENABLE_WARNING_ERRORS "Treat compiler warnings as errors" ON)
    option(ENABLE_COVERAGE "Enable GCC coverage instrumentation" OFF)
    option(BUILD_FUZZING "Build bounded libFuzzer protocol targets" OFF)

    function(atrinik_configure_target target)
        target_compile_features(${target} PRIVATE c_std_17)
        target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
        if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target} PRIVATE
                -Wall
                -Wextra
                -Wno-unused-parameter
                -fno-common)
            if (ENABLE_WARNING_ERRORS)
                target_compile_options(${target} PRIVATE -Werror)
            endif ()
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

    if (NOT TARGET Atrinik::Protocol)
        set(ATRINIK_PROTOCOL_SOURCE_DIR "" CACHE PATH
            "Path to an Atrinik protocol source checkout")
        if (NOT ATRINIK_PROTOCOL_SOURCE_DIR)
            foreach (candidate
                    "${CMAKE_CURRENT_SOURCE_DIR}/../protocol"
                    "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/protocol")
                if (EXISTS "${candidate}/CMakeLists.txt")
                    file(REAL_PATH "${candidate}" resolved)
                    set(ATRINIK_PROTOCOL_SOURCE_DIR "${resolved}"
                        CACHE PATH "Path to an Atrinik protocol source checkout" FORCE)
                    break()
                endif ()
            endforeach ()
        endif ()
        option(LIBATRINIK_USE_INSTALLED_PROTOCOL
            "Use an installed AtrinikProtocol package instead of the locked release"
            OFF)
        if (ATRINIK_PROTOCOL_SOURCE_DIR)
            add_subdirectory("${ATRINIK_PROTOCOL_SOURCE_DIR}"
                "${CMAKE_CURRENT_BINARY_DIR}/atrinik-protocol")
        elseif (LIBATRINIK_USE_INSTALLED_PROTOCOL)
            find_package(AtrinikProtocol "${PROJECT_VERSION}" EXACT CONFIG REQUIRED)
        else ()
            include(FetchContent)
            file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock.json"
                LIBATRINIK_DEPENDENCY_LOCK)
            string(JSON LIBATRINIK_LOCK_SCHEMA ERROR_VARIABLE lock_error
                GET "${LIBATRINIK_DEPENDENCY_LOCK}" schema_version)
            if (lock_error OR NOT LIBATRINIK_LOCK_SCHEMA EQUAL 1)
                message(FATAL_ERROR
                    "dependencies.lock.json has an unsupported schema")
            endif ()
            foreach (field IN ITEMS repository tag commit url sha256)
                string(JSON protocol_${field} ERROR_VARIABLE lock_error
                    GET "${LIBATRINIK_DEPENDENCY_LOCK}"
                    dependencies protocol ${field})
                if (lock_error OR protocol_${field} STREQUAL "")
                    message(FATAL_ERROR
                        "dependencies.lock.json has no protocol.${field}")
                endif ()
            endforeach ()
            string(REGEX REPLACE "^v" "" protocol_version "${protocol_tag}")
            set(expected_protocol_url
                "https://github.com/atrinik/protocol/releases/download/${protocol_tag}/atrinik-protocol-${protocol_version}.tar.gz")
            string(LENGTH "${protocol_commit}" protocol_commit_length)
            string(LENGTH "${protocol_sha256}" protocol_sha256_length)
            if (NOT protocol_repository STREQUAL "atrinik/protocol" OR
                    NOT protocol_tag MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$" OR
                    NOT protocol_commit MATCHES "^[0-9a-f]+$" OR
                    NOT protocol_commit_length EQUAL 40 OR
                    NOT protocol_url STREQUAL expected_protocol_url OR
                    NOT protocol_sha256 MATCHES "^[0-9a-f]+$" OR
                    NOT protocol_sha256_length EQUAL 64)
                message(FATAL_ERROR
                    "dependencies.lock.json has invalid protocol metadata")
            endif ()
            FetchContent_Declare(atrinik_protocol
                URL "${protocol_url}"
                URL_HASH "SHA256=${protocol_sha256}"
                DOWNLOAD_EXTRACT_TIMESTAMP false)
            FetchContent_MakeAvailable(atrinik_protocol)
        endif ()
    endif ()

    if (MINGW)
        set(PLUGIN_SUFFIX ".dll")
    else ()
        set(LINUX true)
        set(PLUGIN_SUFFIX ".so")
    endif ()
endif ()

set(LIBATRINIK_PACKAGE_LAYOUT_DEFAULT false)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(LIBATRINIK_PACKAGE_LAYOUT_DEFAULT true)
endif ()
option(LIBATRINIK_PACKAGE_LAYOUT
    "Use the namespaced include layout from the standalone source package"
    ${LIBATRINIK_PACKAGE_LAYOUT_DEFAULT})

set(ATRINIK_TOOLKIT atrinik-core)

# This target is deliberately configured before the toolkit dependencies. It
# can also be built and installed directly from pathfinding/ without them.
set(ATRINIK_PATHFINDING_PARENT_TESTING false)
set(ATRINIK_PATHFINDING_PARENT_INSTALL false)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    set(ATRINIK_PATHFINDING_PARENT_TESTING true)
endif ()
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ATRINIK_PATHFINDING_PARENT_INSTALL true)
endif ()
add_subdirectory(pathfinding)

# cURL is required by the metaserver.
find_package(CURL REQUIRED)
set(HAVE_CURL true)

find_package(ZLIB REQUIRED)
set(HAVE_ZLIB true)

# Try to find some GNU/Linux libraries.
if (UNIX)
    find_library(MATH_LIBRARY m)

    if (NOT MATH_LIBRARY)
        message(FATAL_ERROR "Could not find math library.")
    endif ()

endif ()

find_library(CRYPT_LIBRARY crypt)

if (CRYPT_LIBRARY)
    set(HAVE_CRYPT true)
else ()
    message(STATUS "Could not find crypt library, encryption of strings such as passwords will be disabled.")
endif ()

find_library(READLINE_LIBRARY readline)

if (READLINE_LIBRARY)
    set(HAVE_READLINE true)
else ()
    message(STATUS "Could not find readline library, simple interactive console will be compiled instead.")
endif ()

find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(IDN2 REQUIRED IMPORTED_TARGET libidn2)

# This is very ugly. There is essentially a namespace clash between newer
# MinGW and the Windows port of pthreads, and they both define a timespec
# structure. This would be OK if both checked for its existence, but the
# bigger problem is that the two definitions are *INCOMPATIBLE*. Indeed,
# the pthread.h version uses a 32-bit integer for the seconds timestamp,
# while the MinGW time.h one uses a 64-bit one.
#
# The solution below disables the pthread.h declaration. This means that
# any code working with a timespec assumes the MinGW structure declaration,
# and if there is any code involving pthreads that passes around a
# timespec... tough luck.
#
# Also, we need it here, otherwise the threads check below will not set the
# correct libraries to use.
if (MINGW)
    set(CMAKE_REQUIRED_DEFINITIONS -DHAVE_STRUCT_TIMESPEC)
endif ()

find_package(Threads REQUIRED)
unset(CMAKE_REQUIRED_DEFINITIONS)

if (POLICY CMP0075)
    cmake_policy(SET CMP0075 NEW)
endif ()

include(CheckIncludeFiles)
include(CheckFunctionExists)

set(CMAKE_REQUIRED_INCLUDES "math.h")
set(CMAKE_REQUIRED_LIBRARIES "m")

# Check for various include files.
check_include_files(fcntl.h HAVE_FCNTL_H)
check_include_files(unistd.h HAVE_UNISTD_H)
check_include_files(sys/time.h HAVE_SYS_TIME_H)
check_include_files(time.h HAVE_TIME_H)
check_include_files(stddef.h HAVE_STDDEF_H)
check_include_files(dirent.h HAVE_DIRENT_H)
check_include_files(sys/ndir.h HAVE_SYS_NDIR_H)
check_include_files(sys/dir.h HAVE_SYS_DIR_H)
check_include_files(sys/socket.h HAVE_SYS_SOCKET_H)
check_include_files(ndir.h HAVE_NDIR_H)
check_include_files(crypt.h HAVE_CRYPT_H)
check_include_files(arpa/inet.h HAVE_ARPA_INET_H)

# Check for various functions.
check_function_exists(strerror HAVE_STRERROR)
check_function_exists(strcasecmp HAVE_STRCASECMP)
check_function_exists(strncasecmp HAVE_STRNCASECMP)
check_function_exists(stricmp HAVE_STRICMP)
check_function_exists(strnicmp HAVE_STRNICMP)
check_function_exists(getdtablesize HAVE_GETDTABLESIZE)
check_function_exists(sysconf HAVE_SYSCONF)
check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
check_function_exists(tempnam HAVE_TEMPNAM)
check_function_exists(strcasestr HAVE_STRCASESTR)
check_function_exists(strdup HAVE_STRDUP)
check_function_exists(strndup HAVE_STRNDUP)
check_function_exists(strtok_r HAVE_STRTOK_R)
check_function_exists(sigaction HAVE_SIGACTION)
check_function_exists(getline HAVE_GETLINE)
check_function_exists(usleep HAVE_USLEEP)
check_function_exists(strnlen HAVE_STRNLEN)
check_function_exists(mkstemp HAVE_MKSTEMP)
check_function_exists(sincos HAVE_SINCOS)
check_function_exists(pselect HAVE_PSELECT)

if (WIN32)
    check_include_files(wspiapi.h HAVE_WSPIAPI_H)
    check_include_files(ws2tcpip.h HAVE_WS2TCPIP_H)
    if (HAVE_WSPIAPI_H OR HAVE_WS2TCPIP_H)
        set(HAVE_GETADDRINFO TRUE)
    endif (HAVE_WSPIAPI_H OR HAVE_WS2TCPIP_H)
elseif (LINUX)
    check_function_exists(getaddrinfo HAVE_GETADDRINFO)
endif ()

try_compile(HAVE_IPV6 ${CMAKE_CURRENT_BINARY_DIR}/compile_tests ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ipv6test.c)

# Generate the CMake configuration header and a namespaced build include tree.
if (LIBATRINIK_PACKAGE_LAYOUT)
    set(LIBATRINIK_BUILD_INCLUDE_ROOT
        "${CMAKE_CURRENT_BINARY_DIR}/include")
    set(LIBATRINIK_GENERATED_INCLUDE_DIR
        "${LIBATRINIK_BUILD_INCLUDE_ROOT}/toolkit")
    file(MAKE_DIRECTORY "${LIBATRINIK_GENERATED_INCLUDE_DIR}")
    file(GLOB LIBATRINIK_BUILD_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
    foreach (header IN LISTS LIBATRINIK_BUILD_HEADERS)
        cmake_path(GET header FILENAME header_name)
        configure_file("${header}"
            "${LIBATRINIK_GENERATED_INCLUDE_DIR}/${header_name}" COPYONLY)
    endforeach ()
    configure_file(cmake.h.def
        "${LIBATRINIK_GENERATED_INCLUDE_DIR}/toolkit_cmake.h")
    configure_file(cmake.h.def
        "${LIBATRINIK_BUILD_INCLUDE_ROOT}/toolkit_cmake.h")
else ()
    set(LIBATRINIK_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    configure_file(cmake.h.def
        "${LIBATRINIK_GENERATED_INCLUDE_DIR}/toolkit_cmake.h")
endif ()

set(TOOLKIT_SOURCES
    binreloc.c
    clioptions.c
    colorspace.c
    console.c
    curl.c
    datetime.c
    logger.c
    map_protocol.c
    metaserver_url.c
    metaserver_publisher.c
    metaserver_publisher_sequence.c
    math.c
    memory.c
    mempool.c
    packet.c
    password.c
    path.c
    porting.c
    process.c
    rendezvous.c
    shstr.c
    signals.c
    socket.c
    socket_asset.c
    socket_direct.c
    socket_quic.c
    stringbuffer.c
    string.c
    toolkit.c
    )

add_library(${ATRINIK_TOOLKIT} STATIC ${TOOLKIT_SOURCES})
add_library(Atrinik::Core ALIAS ${ATRINIK_TOOLKIT})
set_target_properties(${ATRINIK_TOOLKIT} PROPERTIES
    EXPORT_NAME Core
    OUTPUT_NAME atrinik)
atrinik_configure_target(${ATRINIK_TOOLKIT})
target_link_libraries(${ATRINIK_TOOLKIT} PUBLIC Atrinik::Protocol)
if (BUILD_FUZZING)
    target_compile_options(${ATRINIK_TOOLKIT} PRIVATE -fsanitize=fuzzer-no-link)
endif ()
if (LIBATRINIK_PACKAGE_LAYOUT)
    target_include_directories(${ATRINIK_TOOLKIT}
        PUBLIC
            $<BUILD_INTERFACE:${LIBATRINIK_BUILD_INCLUDE_ROOT}>
            $<INSTALL_INTERFACE:include>)
else ()
    target_include_directories(${ATRINIK_TOOLKIT}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/..>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
            $<INSTALL_INTERFACE:include>)
endif ()

if (CURL_INCLUDE_DIRS)
    target_include_directories(${ATRINIK_TOOLKIT} PRIVATE ${CURL_INCLUDE_DIRS})
endif ()
if (ZLIB_INCLUDE_DIRS)
    target_include_directories(${ATRINIK_TOOLKIT} PRIVATE ${ZLIB_INCLUDE_DIRS})
endif ()
if (OPENSSL_INCLUDE_DIR)
    target_include_directories(${ATRINIK_TOOLKIT} PRIVATE ${OPENSSL_INCLUDE_DIR})
endif ()

if (TARGET CURL::libcurl)
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE CURL::libcurl)
else ()
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE ${CURL_LIBRARIES})
endif ()
target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE PkgConfig::IDN2)
target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE Threads::Threads)

if (MINGW)
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE wsock32 wldap32 winmm ws2_32
            imagehlp iphlpapi advapi32)
endif ()

if (TARGET ZLIB::ZLIB)
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE ZLIB::ZLIB)
else ()
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE ${ZLIB_LIBRARIES})
endif ()

# Link math...
if (MATH_LIBRARY)
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE ${MATH_LIBRARY})
endif ()

# Link crypt.
if (CRYPT_LIBRARY)
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE ${CRYPT_LIBRARY})
endif ()

# Link readline if available.
if (HAVE_READLINE)
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE ${READLINE_LIBRARY})
endif ()

if (TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
else ()
    target_link_libraries(${ATRINIK_TOOLKIT} PRIVATE ${OPENSSL_LIBRARIES})
endif ()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    install(TARGETS ${ATRINIK_TOOLKIT}
        EXPORT LibAtrinikTargets
        ARCHIVE DESTINATION lib)
    file(GLOB ATRINIK_PUBLIC_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
    list(FILTER ATRINIK_PUBLIC_HEADERS EXCLUDE REGEX "_private\\.h$")
    install(FILES ${ATRINIK_PUBLIC_HEADERS} DESTINATION include/toolkit)
    install(FILES "${LIBATRINIK_GENERATED_INCLUDE_DIR}/toolkit_cmake.h"
        DESTINATION include/toolkit)
    install(EXPORT LibAtrinikTargets
        FILE LibAtrinikTargets.cmake
        NAMESPACE Atrinik::
        DESTINATION lib/cmake/LibAtrinik)

    configure_package_config_file(
        cmake/LibAtrinikConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/LibAtrinikConfig.cmake"
        INSTALL_DESTINATION lib/cmake/LibAtrinik)
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/LibAtrinikConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/LibAtrinikConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/LibAtrinikConfigVersion.cmake"
        DESTINATION lib/cmake/LibAtrinik)

    if (BUILD_TESTING)
        add_executable(libatrinik-smoke tests/smoke.c)
        atrinik_configure_target(libatrinik-smoke)
        target_link_libraries(libatrinik-smoke PRIVATE Atrinik::Core)
        add_test(NAME libatrinik-smoke COMMAND libatrinik-smoke)

        add_executable(libatrinik-clioptions tests/clioptions.c)
        atrinik_configure_target(libatrinik-clioptions)
        target_link_libraries(libatrinik-clioptions PRIVATE Atrinik::Core)
        add_test(NAME libatrinik-clioptions COMMAND libatrinik-clioptions)

        add_executable(libatrinik-bresenham tests/bresenham.c)
        atrinik_configure_target(libatrinik-bresenham)
        target_link_libraries(libatrinik-bresenham PRIVATE Atrinik::Core)
        add_test(NAME libatrinik-bresenham COMMAND libatrinik-bresenham)

        add_executable(libatrinik-path tests/path.c)
        atrinik_configure_target(libatrinik-path)
        target_link_libraries(libatrinik-path PRIVATE Atrinik::Core)
        add_test(NAME libatrinik-path COMMAND libatrinik-path)

        add_executable(libatrinik-rendezvous tests/rendezvous.c)
        atrinik_configure_target(libatrinik-rendezvous)
        target_link_libraries(libatrinik-rendezvous PRIVATE Atrinik::Core)
        add_test(NAME libatrinik-rendezvous COMMAND libatrinik-rendezvous
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rendezvous-invite-v1.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rendezvous-invite-v1-negative.json")

        add_executable(libatrinik-metaserver-publisher
            tests/metaserver_publisher.c)
        atrinik_configure_target(libatrinik-metaserver-publisher)
        target_link_libraries(libatrinik-metaserver-publisher PRIVATE Atrinik::Core)
        add_test(NAME libatrinik-metaserver-publisher
            COMMAND libatrinik-metaserver-publisher
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/metaserver-publisher-v1.json")

        add_executable(libatrinik-metaserver-url tests/metaserver_url.c)
        atrinik_configure_target(libatrinik-metaserver-url)
        target_link_libraries(libatrinik-metaserver-url PRIVATE Atrinik::Core)
        add_test(NAME libatrinik-metaserver-url COMMAND libatrinik-metaserver-url)

        if (NOT WIN32)
            add_executable(libatrinik-curl tests/curl.c)
            atrinik_configure_target(libatrinik-curl)
            target_link_libraries(libatrinik-curl PRIVATE Atrinik::Core Threads::Threads)
            add_test(NAME libatrinik-curl COMMAND libatrinik-curl)
        endif ()
    endif ()
endif ()
