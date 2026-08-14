# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

if (NOT DEFINED ATRINIK_CLIENT OR NOT DEFINED PLAYER_VIEW_MANIFEST)
    message(FATAL_ERROR "player-view rejection verifier requires client and manifest")
endif ()

execute_process(
    COMMAND "${ATRINIK_CLIENT}" --player-view "${PLAYER_VIEW_MANIFEST}" -
    RESULT_VARIABLE player_view_result
    OUTPUT_VARIABLE player_view_output
    ERROR_VARIABLE player_view_error)

if (NOT player_view_result EQUAL 3)
    message(FATAL_ERROR
        "invalid manifest returned ${player_view_result}, expected parser rejection 3\n"
        "stdout: ${player_view_output}\n"
        "stderr: ${player_view_error}")
endif ()
