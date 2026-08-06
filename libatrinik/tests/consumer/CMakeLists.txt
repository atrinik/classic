cmake_minimum_required(VERSION 3.21)
project(libatrinik_consumer LANGUAGES C)

find_package(LibAtrinik 1 CONFIG REQUIRED)

add_executable(libatrinik-consumer main.c)
target_link_libraries(libatrinik-consumer PRIVATE Atrinik::Core)

