if (NOT DEFINED ATRINIK_SOURCE_DIR OR NOT DEFINED ATRINIK_BINARY_DIR OR
        NOT DEFINED ATRINIK_RUNTIME_DIR OR
        NOT DEFINED ATRINIK_ARENA_PLUGIN)
    message(FATAL_ERROR "Missing server test runtime preparation input")
endif ()

cmake_path(ABSOLUTE_PATH ATRINIK_RUNTIME_DIR NORMALIZE
    OUTPUT_VARIABLE normalized_runtime_dir)
set(expected_runtime_dir "${ATRINIK_BINARY_DIR}/server-test-runtime-seed")
cmake_path(ABSOLUTE_PATH expected_runtime_dir NORMALIZE
    OUTPUT_VARIABLE normalized_expected_runtime_dir)
if (NOT normalized_runtime_dir STREQUAL normalized_expected_runtime_dir)
    message(FATAL_ERROR
        "Refusing to replace unexpected server test runtime: ${normalized_runtime_dir}")
endif ()

set(ATRINIK_RUNTIME_DIR "${normalized_runtime_dir}")
set(runtime_server "${ATRINIK_RUNTIME_DIR}/server")
if (NOT EXISTS "${ATRINIK_SOURCE_DIR}/resources/.atrinik-dependency.json" OR
        NOT EXISTS "${ATRINIK_SOURCE_DIR}/runtime/content/.atrinik-dependency.json")
    message(FATAL_ERROR
        "Locked runtime dependencies are missing; run "
        "'python3 tools/dependencies.py sync' from the repository root")
endif ()
file(REMOVE_RECURSE "${ATRINIK_RUNTIME_DIR}")
file(MAKE_DIRECTORY
    "${runtime_server}"
    "${runtime_server}/data/tmp"
    "${runtime_server}/lib")

file(COPY "${ATRINIK_SOURCE_DIR}/install_data/"
    DESTINATION "${runtime_server}/data")
file(COPY
    "${ATRINIK_SOURCE_DIR}/ca-bundle.crt"
    "${ATRINIK_SOURCE_DIR}/permissions.cfg"
    "${ATRINIK_SOURCE_DIR}/server.cfg"
    DESTINATION "${runtime_server}")
file(COPY
    "${ATRINIK_ARENA_PLUGIN}"
    DESTINATION "${runtime_server}")
if (DEFINED ATRINIK_PYTHON_PLUGIN)
    file(COPY "${ATRINIK_PYTHON_PLUGIN}" DESTINATION "${runtime_server}")
endif ()

file(COPY "${ATRINIK_SOURCE_DIR}/runtime/content/maps"
    DESTINATION "${runtime_server}")
file(COPY "${ATRINIK_SOURCE_DIR}/src/tests/data/content_benchmark"
    DESTINATION "${runtime_server}/maps/tests")
file(COPY "${ATRINIK_SOURCE_DIR}/runtime/content/lib/"
    DESTINATION "${runtime_server}/lib")
file(COPY "${ATRINIK_SOURCE_DIR}/resources"
    DESTINATION "${runtime_server}")

if (DEFINED ATRINIK_PYTHON_PLUGIN)
    include("${ATRINIK_SOURCE_DIR}/cmake/prepare_locked_content_python_unit.cmake")

    set(python_events "${runtime_server}/maps/python/events")
    configure_file(
        "${python_events}/python_unit.py"
        "${python_events}/python_unit_content.py"
        COPYONLY)
    foreach (fixture IN ITEMS pass fail calendar)
        configure_file(
            "${ATRINIK_SOURCE_DIR}/src/tests/data/plugin_python/python_unit_${fixture}.py"
            "${python_events}/python_unit_${fixture}.py"
            COPYONLY)
    endforeach ()
endif ()

file(TOUCH "${ATRINIK_RUNTIME_DIR}/.prepared")
