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

# The source content's newer repeat-quest test reaches this legacy assertion;
# None is a value rather than a type and therefore cannot appear in the second
# argument to isinstance(). Normalize the copied test runtime without mutating
# either the locked dependency or the workspace source checkout.
set(quest_manager "${runtime_server}/maps/python/QuestManager.py")
file(READ "${quest_manager}" quest_manager_content)
string(REGEX MATCHALL
    "isinstance\\(delay, \\(int, None\\)\\)"
    invalid_delay_checks "${quest_manager_content}")
list(LENGTH invalid_delay_checks invalid_delay_check_count)
string(REGEX MATCHALL
    "isinstance\\(delay, \\(int, type\\(None\\)\\)\\)"
    valid_delay_checks "${quest_manager_content}")
list(LENGTH valid_delay_checks valid_delay_check_count)
if (invalid_delay_check_count EQUAL 1 AND valid_delay_check_count EQUAL 0)
    string(REPLACE
        "isinstance(delay, (int, None))"
        "isinstance(delay, (int, type(None)))"
        quest_manager_content "${quest_manager_content}")
elseif (NOT invalid_delay_check_count EQUAL 0 OR
        NOT valid_delay_check_count EQUAL 1)
    message(FATAL_ERROR
        "Reassess the content repeat-quest compatibility fixture")
endif ()
file(WRITE "${quest_manager}" "${quest_manager_content}")

# The source-only repeat-failure regression expects an immediate reset but the
# default test player has only one quest point, which fail() consumes. Restore
# that point in the copied test before constructing the manager that exercises
# the reset branch.
set(quest_manager_tests
    "${runtime_server}/maps/python/tests/QuestManager.py")
file(READ "${quest_manager_tests}" quest_manager_tests_content)
string(CONCAT repeat_failure_setup
    "        self.assertTrue(qm.failed())\n"
    "\n"
    "        qm = QuestManager(activator, quest)")
string(CONCAT repeat_failure_setup_fixed
    "        self.assertTrue(qm.failed())\n"
    "        qm.quest_container.magic = 0\n"
    "\n"
    "        qm = QuestManager(activator, quest)")
string(FIND "${quest_manager_tests_content}"
    "${repeat_failure_setup}" repeat_failure_setup_pos)
string(FIND "${quest_manager_tests_content}"
    "${repeat_failure_setup_fixed}" repeat_failure_setup_fixed_pos)
if (NOT repeat_failure_setup_pos EQUAL -1 AND
        repeat_failure_setup_fixed_pos EQUAL -1)
    string(REPLACE
        "${repeat_failure_setup}"
        "${repeat_failure_setup_fixed}"
        quest_manager_tests_content "${quest_manager_tests_content}")
elseif (repeat_failure_setup_pos EQUAL -1 AND
        repeat_failure_setup_fixed_pos EQUAL -1 AND
        content_is_locked_v1_2_0)
    # This older locked test suite predates the repeat-failure regression.
elseif (NOT repeat_failure_setup_pos EQUAL -1 OR
        repeat_failure_setup_fixed_pos EQUAL -1)
    message(FATAL_ERROR
        "Reassess the content repeat-quest test compatibility fixture")
endif ()
file(WRITE "${quest_manager_tests}" "${quest_manager_tests_content}")
