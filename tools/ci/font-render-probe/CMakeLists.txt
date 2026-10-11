cmake_minimum_required(VERSION 3.21)
project(atrinik-font-render-probe LANGUAGES C)

# Deliberately independent of the application's compiler policy: compile this
# exact source with both the pinned MXE baseline and the LLVM candidate.
find_package(SDL3 3.4 CONFIG REQUIRED)
find_package(SDL3_ttf 3.2 CONFIG REQUIRED)
add_executable(client-font-render-probe
    ../../../client/src/tests/font_render.c)
set_target_properties(client-font-render-probe PROPERTIES
    C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(client-font-render-probe PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf)
