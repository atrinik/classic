if (NOT DEFINED ATRINIK_CLIENT OR NOT DEFINED PLAYER_VIEW_MANIFEST)
    message(FATAL_ERROR "player-view verification inputs are incomplete")
endif ()

set(temp_root "$ENV{TMPDIR}")
if (temp_root STREQUAL "")
    set(temp_root "$ENV{TEMP}")
endif ()
if (temp_root STREQUAL "")
    set(temp_root "$ENV{TMP}")
endif ()
if (temp_root STREQUAL "" AND UNIX)
    set(temp_root "/tmp")
endif ()
if (temp_root STREQUAL "")
    message(FATAL_ERROR "no temporary directory environment variable is set")
endif ()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef temp_suffix)
set(output_dir "${temp_root}/atrinik-player-view-${temp_suffix}")
file(MAKE_DIRECTORY "${output_dir}")
set(first "${output_dir}/first.png")
set(second "${output_dir}/second.png")

function (player_view_fail reason)
    file(REMOVE_RECURSE "${output_dir}")
    message(FATAL_ERROR "${reason}")
endfunction ()

execute_process(
    COMMAND "${ATRINIK_CLIENT}" --player-view "${PLAYER_VIEW_MANIFEST}" "${first}"
    RESULT_VARIABLE first_result
    OUTPUT_VARIABLE first_output
    ERROR_VARIABLE first_error)
if (NOT first_result EQUAL 0 OR NOT EXISTS "${first}")
    player_view_fail("first player-view render failed: ${first_output}${first_error}")
endif ()
file(SHA256 "${first}" first_digest)

execute_process(
    COMMAND "${ATRINIK_CLIENT}" --player-view "${PLAYER_VIEW_MANIFEST}" "${first}"
    RESULT_VARIABLE overwrite_result
    OUTPUT_QUIET
    ERROR_QUIET)
if (overwrite_result EQUAL 0)
    player_view_fail("player-view unexpectedly overwrote an existing output")
endif ()
file(SHA256 "${first}" unchanged_digest)
if (NOT unchanged_digest STREQUAL first_digest)
    player_view_fail("refused output changed after overwrite attempt")
endif ()

execute_process(
    COMMAND "${ATRINIK_CLIENT}" --player-view "${PLAYER_VIEW_MANIFEST}" "${second}"
    RESULT_VARIABLE second_result
    OUTPUT_VARIABLE second_output
    ERROR_VARIABLE second_error)
if (NOT second_result EQUAL 0 OR NOT EXISTS "${second}")
    player_view_fail("second player-view render failed: ${second_output}${second_error}")
endif ()
file(SHA256 "${second}" second_digest)
if (NOT second_digest STREQUAL first_digest)
    player_view_fail("repeated player-view PNG output is not byte-identical")
endif ()

file(REMOVE_RECURSE "${output_dir}")
