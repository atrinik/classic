cmake_minimum_required(VERSION 3.21)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(atrinik-pathfinding VERSION 1.0.0 LANGUAGES C)
    include(CTest)
    include(CMakePackageConfigHelpers)
    set(ATRINIK_PATHFINDING_STANDALONE true)
endif ()

add_library(atrinik-pathfinding STATIC pathfinding.c)
add_library(Atrinik::Pathfinding ALIAS atrinik-pathfinding)
set_target_properties(atrinik-pathfinding PROPERTIES
    EXPORT_NAME Pathfinding
    OUTPUT_NAME atrinik-pathfinding
    POSITION_INDEPENDENT_CODE ON)
target_compile_features(atrinik-pathfinding PUBLIC c_std_17)
target_include_directories(atrinik-pathfinding
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(atrinik-pathfinding PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-common)
    if (NOT DEFINED ENABLE_WARNING_ERRORS OR ENABLE_WARNING_ERRORS)
        target_compile_options(atrinik-pathfinding PRIVATE -Werror)
    endif ()
endif ()

if (BUILD_TESTING AND
        (ATRINIK_PATHFINDING_STANDALONE OR
         ATRINIK_PATHFINDING_PARENT_TESTING))
    add_subdirectory(tests)
endif ()

if (ATRINIK_PATHFINDING_STANDALONE OR ATRINIK_PATHFINDING_PARENT_INSTALL)
    install(TARGETS atrinik-pathfinding
        EXPORT AtrinikPathfindingTargets
        ARCHIVE DESTINATION lib)
    install(DIRECTORY include/ DESTINATION include)
    install(EXPORT AtrinikPathfindingTargets
        FILE AtrinikPathfindingTargets.cmake
        NAMESPACE Atrinik::
        DESTINATION lib/cmake/AtrinikPathfinding)
    configure_package_config_file(
        cmake/AtrinikPathfindingConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/AtrinikPathfindingConfig.cmake"
        INSTALL_DESTINATION lib/cmake/AtrinikPathfinding)
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/AtrinikPathfindingConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/AtrinikPathfindingConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/AtrinikPathfindingConfigVersion.cmake"
        DESTINATION lib/cmake/AtrinikPathfinding)
endif ()
