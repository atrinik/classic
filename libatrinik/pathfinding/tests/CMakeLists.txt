add_executable(atrinik-pathfinding-tests test_pathfinding.c)
target_link_libraries(atrinik-pathfinding-tests PRIVATE Atrinik::Pathfinding)
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(atrinik-pathfinding-tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if (NOT DEFINED ENABLE_WARNING_ERRORS OR ENABLE_WARNING_ERRORS)
        target_compile_options(atrinik-pathfinding-tests PRIVATE -Werror)
    endif ()
endif ()
add_test(NAME atrinik-pathfinding COMMAND atrinik-pathfinding-tests)

add_executable(atrinik-pathfinding-benchmark benchmark.c)
target_link_libraries(atrinik-pathfinding-benchmark PRIVATE Atrinik::Pathfinding)
