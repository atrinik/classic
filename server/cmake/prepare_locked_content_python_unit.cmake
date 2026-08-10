# Compatibility fixture for the checksum-locked content v1.2.0 runtime. Its
# Player test hard-codes the former item-mark command ID. The owning content
# branch corrected this in atrinik/content@0eea49ef65f21b7b02defda2fdaaf7f8292c17fd,
# after the latest compatible published artifact. Remove this fixture when the
# server dependency lock advances to an artifact containing that correction.
if (NOT DEFINED ATRINIK_GAME_COMMANDS_HEADER OR
        NOT EXISTS "${ATRINIK_GAME_COMMANDS_HEADER}")
    message(FATAL_ERROR "Missing generated game-command header")
endif ()

set(content_marker
    "${ATRINIK_SOURCE_DIR}/runtime/content/.atrinik-dependency.json")
file(READ "${content_marker}" content_marker_data)
if (content_marker_data MATCHES "\"workspace_source\"[ ]*:")
    return()
elseif (NOT content_marker_data MATCHES "\"tag\"[ ]*:[ ]*\"v1\\.2\\.0\"")
    message(FATAL_ERROR
        "Reassess the locked-content Python unit compatibility fixture")
endif ()

file(STRINGS "${ATRINIK_GAME_COMMANDS_HEADER}"
    item_mark_definition
    REGEX "^[ ]*SERVER_CMD_ITEM_MARK = [0-9]+,$")
if (NOT item_mark_definition MATCHES
        "^[ ]*SERVER_CMD_ITEM_MARK = ([0-9]+),$")
    message(FATAL_ERROR "Could not resolve SERVER_CMD_ITEM_MARK")
endif ()
set(item_mark_command "${CMAKE_MATCH_1}")

set(player_tests "${runtime_server}/maps/python/tests/Atrinik_tests/Player.py")
file(READ "${player_tests}" player_tests_content)
string(REGEX MATCHALL
    "data = struct.pack\\(\"!HBI\", 5, [0-9]+, obj.count\\)"
    item_mark_packets "${player_tests_content}")
list(LENGTH item_mark_packets item_mark_packet_count)
if (NOT item_mark_packet_count EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one item-mark packet in ${player_tests}")
endif ()
string(REGEX REPLACE
    "data = struct.pack\\(\"!HBI\", 5, [0-9]+, obj.count\\)"
    "data = struct.pack(\"!HBI\", 5, ${item_mark_command}, obj.count)"
    normalized_player_tests "${player_tests_content}")
file(WRITE "${player_tests}" "${normalized_player_tests}")
