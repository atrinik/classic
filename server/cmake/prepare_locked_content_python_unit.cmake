# Compatibility fixture for the checksum-locked content v1.2.0 runtime. Its
# Player test hard-codes the former item-mark command ID. The owning content
# branch corrected this in atrinik/content@0eea49ef65f21b7b02defda2fdaaf7f8292c17fd,
# after the latest compatible published artifact. A workspace source checkout
# can therefore already contain the current ID and needs no rewrite. Remove
# this fixture when the server dependency lock advances to an artifact
# containing that correction.
if (NOT DEFINED ATRINIK_GAME_COMMANDS_HEADER OR
        NOT EXISTS "${ATRINIK_GAME_COMMANDS_HEADER}")
    message(FATAL_ERROR "Missing generated game-command header")
endif ()

set(content_marker
    "${ATRINIK_SOURCE_DIR}/runtime/content/.atrinik-dependency.json")
file(READ "${content_marker}" content_marker_data)
set(content_is_locked_v1_2_0 FALSE)
if (content_marker_data MATCHES "\"tag\"[ ]*:[ ]*\"v1\\.2\\.0\"")
    set(content_is_locked_v1_2_0 TRUE)
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
if (NOT item_mark_packets MATCHES
        "data = struct.pack\\(\"!HBI\", 5, ([0-9]+), obj.count\\)")
    message(FATAL_ERROR "Could not resolve the content item-mark command")
endif ()
set(content_item_mark_command "${CMAKE_MATCH_1}")
set(normalized_player_tests "${player_tests_content}")

if (content_item_mark_command STREQUAL item_mark_command)
    # The selected source content already contains the owning-repository fix.
elseif (content_is_locked_v1_2_0)
    string(REGEX REPLACE
        "data = struct.pack\\(\"!HBI\", 5, [0-9]+, obj.count\\)"
        "data = struct.pack(\"!HBI\", 5, ${item_mark_command}, obj.count)"
        normalized_player_tests "${player_tests_content}")
else ()
    message(FATAL_ERROR
        "Reassess the locked-content Python unit compatibility fixture")
endif ()

# Raw packet mutation bypasses the movement-queue metadata introduced in game
# protocol v1076. Keep the locked and workspace content tests compatible by
# routing their synthetic client command through the queue-aware plugin API.
string(REGEX MATCHALL
    "self\\.pl\\.s_packet_recv_cmd \\+= data"
    raw_queue_writes "${normalized_player_tests}")
list(LENGTH raw_queue_writes raw_queue_write_count)
string(REGEX MATCHALL
    "self\\.pl\\.QueueCommand\\(data\\)"
    safe_queue_writes "${normalized_player_tests}")
list(LENGTH safe_queue_writes safe_queue_write_count)
if (raw_queue_write_count EQUAL 3 AND safe_queue_write_count EQUAL 0)
    string(REPLACE
        "self.pl.s_packet_recv_cmd += data"
        "self.pl.QueueCommand(data)"
        normalized_player_tests "${normalized_player_tests}")
elseif (NOT raw_queue_write_count EQUAL 0 OR
        NOT safe_queue_write_count EQUAL 3)
    message(FATAL_ERROR
        "Reassess the content command-queue compatibility fixture")
endif ()
file(WRITE "${player_tests}" "${normalized_player_tests}")

# Direction is a semantic 0..8 value even though its storage is a signed byte.
# Adapt the external content field-width test to the server's stricter runtime
# boundary until the selected content release carries the semantic test.
set(object_tests "${runtime_server}/maps/python/tests/Atrinik_tests/Object.py")
file(READ "${object_tests}" object_tests_content)
set(old_direction_test
    "    def test_direction(self):\n        self.field_test_int(\"direction\", 8)\n")
set(new_direction_test
    "    def test_direction(self):\n        with self.assertRaises(ValueError):\n            self.obj.direction = -1\n        with self.assertRaises(ValueError):\n            self.obj.direction = 9\n        with self.assertRaises(ValueError):\n            self.obj.direction = 127\n        for direction in (0, 1, 8):\n            self.obj.direction = direction\n            self.assertEqual(self.obj.direction, direction)\n")
string(FIND "${object_tests_content}" "${old_direction_test}" direction_test_offset)
if (direction_test_offset EQUAL -1)
    string(FIND "${object_tests_content}" "${new_direction_test}" normalized_direction_test_offset)
    if (normalized_direction_test_offset EQUAL -1)
        message(FATAL_ERROR
            "Reassess the content direction-field compatibility fixture")
    endif ()
else ()
    string(REPLACE "${old_direction_test}" "${new_direction_test}"
        object_tests_content "${object_tests_content}")
    file(WRITE "${object_tests}" "${object_tests_content}")
endif ()
