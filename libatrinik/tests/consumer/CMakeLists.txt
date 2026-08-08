cmake_minimum_required(VERSION 3.21)
project(libatrinik_consumer LANGUAGES C)

set(ATRINIK_CONSUMER_VERSION "" CACHE STRING
    "Exact unified classic package version expected by this consumer")
if (ATRINIK_CONSUMER_VERSION)
    find_package(LibAtrinik "${ATRINIK_CONSUMER_VERSION}" EXACT CONFIG REQUIRED)
    find_package(AtrinikPathfinding "${ATRINIK_CONSUMER_VERSION}" EXACT CONFIG REQUIRED)
else ()
    find_package(LibAtrinik CONFIG REQUIRED)
    find_package(AtrinikPathfinding CONFIG REQUIRED)
endif ()

add_executable(libatrinik-consumer main.c)
target_link_libraries(libatrinik-consumer PRIVATE
    Atrinik::Core
    Atrinik::Pathfinding)
