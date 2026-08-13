if (NOT DEFINED ATRINIK_SERVER OR NOT DEFINED ATRINIK_RUNTIME_DIR OR
        NOT DEFINED ATRINIK_UNIT_SCRIPT OR
        NOT DEFINED ATRINIK_EXPECT_FAILURE)
    message(FATAL_ERROR "Missing Python plugin unit-test input")
endif ()

set(python_events "${ATRINIK_RUNTIME_DIR}/maps/python/events")
set(content_script "${python_events}/python_unit_content.py")
set(unit_script "${python_events}/${ATRINIK_UNIT_SCRIPT}")
set(entrypoint "${python_events}/python_unit.py")
foreach (path IN ITEMS
        "${ATRINIK_SERVER}" "${content_script}" "${unit_script}")
    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing Python plugin unit-test input: ${path}")
    endif ()
endforeach ()

# Restore the content entry point before and after every invocation. This also
# recovers deterministically if an earlier test driver was interrupted.
configure_file("${content_script}" "${entrypoint}" COPYONLY)
configure_file("${unit_script}" "${entrypoint}" COPYONLY)
if (DEFINED ATRINIK_CLOCKDATA)
    if (NOT ATRINIK_CLOCKDATA MATCHES "^[0-9]+$")
        message(FATAL_ERROR "Invalid plugin unit-test clockdata")
    endif ()
    file(WRITE "${ATRINIK_RUNTIME_DIR}/data/clockdata" "${ATRINIK_CLOCKDATA}")
endif ()
execute_process(
    COMMAND "${ATRINIK_SERVER}" --plugin_unit --port_mapping=off
    WORKING_DIRECTORY "${ATRINIK_RUNTIME_DIR}"
    RESULT_VARIABLE server_result
    OUTPUT_VARIABLE server_stdout
    ERROR_VARIABLE server_stderr)
configure_file("${content_script}" "${entrypoint}" COPYONLY)

if (server_stdout)
    message("${server_stdout}")
endif ()
if (server_stderr)
    message("${server_stderr}")
endif ()

if (ATRINIK_EXPECT_FAILURE)
    if (NOT server_result EQUAL 1 OR
            NOT "${server_stdout}${server_stderr}" MATCHES
                "Controlled Python plugin unit failure")
        message(FATAL_ERROR
            "Expected controlled server failure, got: ${server_result}")
    endif ()
elseif (NOT server_result EQUAL 0)
    message(FATAL_ERROR "Expected server success, got: ${server_result}")
endif ()
