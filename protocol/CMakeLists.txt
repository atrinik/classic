cmake_minimum_required(VERSION 3.21)

# Release archives carry this shared module beside their own CMake inputs;
# embedded dependencies use the enclosing scoped archive's copy.
file(REAL_PATH "${CMAKE_CURRENT_LIST_FILE}" component_file)
get_filename_component(component_dir "${component_file}" DIRECTORY)
if (EXISTS "${component_dir}/cmake/AtrinikVersion.cmake")
    include("${component_dir}/cmake/AtrinikVersion.cmake")
elseif (EXISTS "${component_dir}/../cmake/AtrinikVersion.cmake")
    include("${component_dir}/../cmake/AtrinikVersion.cmake")
else ()
    include("${component_dir}/../../cmake/AtrinikVersion.cmake")
endif ()
if (ATRINIK_INTEGRATED_BUILD)
    set(ATRINIK_SOURCE_VERSION "${PROJECT_VERSION}")
else ()
    atrinik_resolve_version(ATRINIK_SOURCE_VERSION)
endif ()

project(atrinik-protocol VERSION "${ATRINIK_SOURCE_VERSION}" LANGUAGES NONE)

include(CMakePackageConfigHelpers)
include(CTest)

add_library(atrinik-protocol INTERFACE)
add_library(Atrinik::Protocol ALIAS atrinik-protocol)
set_target_properties(atrinik-protocol PROPERTIES EXPORT_NAME Protocol)
target_include_directories(atrinik-protocol INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated/c/include>
    $<INSTALL_INTERFACE:include>)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ATRINIK_PROTOCOL_GENERATED_OUTPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/c/include/atrinik/protocol/game_commands.def"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/c/include/atrinik/protocol/game_commands.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/python/atrinik_protocol/__init__.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/python/atrinik_protocol/game.py")
set(ATRINIK_PROTOCOL_GENERATE_STAMP
    "${CMAKE_CURRENT_BINARY_DIR}/protocol-generate-check.stamp")
add_custom_command(
    OUTPUT "${ATRINIK_PROTOCOL_GENERATE_STAMP}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate.py" --check
    COMMAND ${CMAKE_COMMAND} -E touch "${ATRINIK_PROTOCOL_GENERATE_STAMP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/schema/game-commands.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate.py"
        ${ATRINIK_PROTOCOL_GENERATED_OUTPUTS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking generated classic game protocol bindings"
    VERBATIM)
add_custom_target(protocol-generate-check ALL
    DEPENDS "${ATRINIK_PROTOCOL_GENERATE_STAMP}")

if (BUILD_TESTING)
    if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        add_test(NAME protocol-generate-check
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate.py" --check)
    endif ()
    add_test(NAME protocol-python-tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_*.py")
endif ()

install(TARGETS atrinik-protocol EXPORT AtrinikProtocolTargets)
install(DIRECTORY generated/c/include/ DESTINATION include)
install(EXPORT AtrinikProtocolTargets
    FILE AtrinikProtocolTargets.cmake
    NAMESPACE Atrinik::
    DESTINATION lib/cmake/AtrinikProtocol)

configure_package_config_file(
    cmake/AtrinikProtocolConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/AtrinikProtocolConfig.cmake"
    INSTALL_DESTINATION lib/cmake/AtrinikProtocol)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AtrinikProtocolConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AtrinikProtocolConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AtrinikProtocolConfigVersion.cmake"
    DESTINATION lib/cmake/AtrinikProtocol)
