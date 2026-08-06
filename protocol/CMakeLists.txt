cmake_minimum_required(VERSION 3.21)

project(atrinik-protocol VERSION 1.0.0 LANGUAGES NONE)

include(CMakePackageConfigHelpers)
include(CTest)

add_library(atrinik-protocol INTERFACE)
add_library(Atrinik::Protocol ALIAS atrinik-protocol)
set_target_properties(atrinik-protocol PROPERTIES EXPORT_NAME Protocol)
target_include_directories(atrinik-protocol INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated/c/include>
    $<INSTALL_INTERFACE:include>)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(protocol-generate-check ALL
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate.py" --check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking generated legacy protocol bindings"
    VERBATIM)

if (BUILD_TESTING)
    add_test(NAME protocol-generate-check
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate.py" --check)
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
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AtrinikProtocolConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AtrinikProtocolConfigVersion.cmake"
    DESTINATION lib/cmake/AtrinikProtocol)

