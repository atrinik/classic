#include <discord_rpc.h>
#include <rich_presence.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
#include <windows.h>
#endif

#define require(condition)                                                                        \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(EXIT_FAILURE);                                                                   \
        }                                                                                         \
    } while (0)

typedef struct backend_state {
    unsigned int publishes;
    unsigned int clears;
    discord_rpc_activity_t activity;
} backend_state_t;

static void capture_publish(void *context, const discord_rpc_activity_t *activity) {
    backend_state_t *state = context;
    state->publishes++;
    state->activity = *activity;
}

static void capture_clear(void *context) {
    backend_state_t *state = context;
    state->clears++;
}

static void test_policy(void) {
    require(rich_presence_application_id_valid("123456789012345678"));
    require(!rich_presence_application_id_valid(""));
    require(!rich_presence_application_id_valid("0123"));
    require(!rich_presence_application_id_valid("18446744073709551616"));
    require(!rich_presence_application_id_valid("123-secret"));

    char normalized[DISCORD_RPC_TEXT_BYTES];
    require(rich_presence_normalize("  Hall\t of\n  Heroes  ", normalized));
    require(strcmp(normalized, "Hall of Heroes") == 0);
    require(!rich_presence_normalize("A\xc0\xaf", normalized));
    require(rich_presence_normalize("\xc3\x89toile", normalized));
    require(strcmp(normalized, "\xc3\x89toile") == 0);
    require(rich_presence_normalize("Hall\xc2\x85of\xe2\x80\xa8Heroes", normalized));
    require(strcmp(normalized, "Hall of Heroes") == 0);
    require(rich_presence_normalize("Hall\xc2\xa0of\xe3\x80\x80Heroes", normalized));
    require(strcmp(normalized, "Hall of Heroes") == 0);

    char long_value[300];
    memset(long_value, 'x', sizeof(long_value) - 1U);
    long_value[sizeof(long_value) - 1U] = '\0';
    require(rich_presence_normalize(long_value, normalized));
    require(strlen(normalized) == 128U);

    backend_state_t captured = {0};
    rich_presence_backend_t backend = {
        .context = &captured,
        .publish = capture_publish,
        .clear = capture_clear,
    };
    rich_presence_controller_t controller;
    rich_presence_controller_init(&controller);
    rich_presence_input_t input = {
        .playing = true,
        .privacy = RICH_PRESENCE_GAME,
        .public_server = true,
        .server = "Public Realm",
        .zone = "Crystal Caverns",
        .now_ms = 10,
        .now_unix = 1000,
    };
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 1U);
    require(strcmp(captured.activity.details, "Playing Atrinik Classic") == 0);
    require(captured.activity.state[0] == '\0');
    require(captured.activity.started_at == 1000U);

    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 1U);

    input.privacy = RICH_PRESENCE_SERVER_ZONE;
    input.now_ms = 100;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 1U);
    input.now_ms = 4110;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 2U);
    require(strcmp(captured.activity.details, "Exploring Crystal Caverns") == 0);
    require(strcmp(captured.activity.state, "On Public Realm") == 0);
    require(captured.activity.started_at == 1000U);

    input.public_server = false;
    input.server = "192.0.2.1:13327";
    input.now_ms = 8210;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 3U);
    require(strcmp(captured.activity.state, "On Private server") == 0);
    require(strstr(captured.activity.state, "192.0.2.1") == NULL);

    input.privacy = RICH_PRESENCE_GAME;
    input.now_ms = 8300;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.clears == 1U);
    require(captured.publishes == 3U);
    input.now_ms = 20010;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 4U);

    input.playing = false;
    input.now_ms = 20100;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.clears == 2U);
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.clears == 2U);

    input.playing = true;
    input.now_ms = 28300;
    input.now_unix = 2000;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.activity.started_at == 2000U);
    rich_presence_controller_stop(&controller, &backend, 28301);
    require(captured.clears == 3U);

    /* Off still observes play transitions, so enabling keeps session elapsed time. */
    memset(&captured, 0, sizeof(captured));
    rich_presence_controller_init(&controller);
    input.playing = true;
    input.privacy = RICH_PRESENCE_OFF;
    input.now_ms = 30000;
    input.now_unix = 3000;
    rich_presence_controller_tick(&controller, &input, NULL);
    input.privacy = RICH_PRESENCE_GAME;
    input.now_ms = 31000;
    input.now_unix = 4000;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.activity.started_at == 3000U);

    /* A queued B update is cancelled when the current state returns to A. */
    memset(&captured, 0, sizeof(captured));
    rich_presence_controller_init(&controller);
    input.privacy = RICH_PRESENCE_SERVER_ZONE;
    input.zone = "Zone A";
    input.now_ms = 40000;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 1U);
    input.zone = "Zone B";
    input.now_ms = 40100;
    rich_presence_controller_tick(&controller, &input, &backend);
    input.zone = "Zone A";
    input.now_ms = 40200;
    rich_presence_controller_tick(&controller, &input, &backend);
    input.now_ms = 44200;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 1U);

    /* Missing or invalid zone data uses the exact generic fallback. */
    input.zone = "";
    input.now_ms = 45100;
    rich_presence_controller_tick(&controller, &input, &backend);
    input.now_ms = 48300;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(strcmp(captured.activity.details, "Exploring") == 0);
    require(controller.command_count <= 5U);

    unsigned int clears_before_session = captured.clears;
    rich_presence_controller_begin_session(&controller, &backend, 50000);
    require(captured.clears == clears_before_session + 1U);
    input.zone = "";
    input.now_ms = 50000;
    input.now_unix = 5000;
    rich_presence_controller_tick(&controller, &input, &backend);
    input.now_ms = 54100;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.activity.started_at == 5000U);
    require(strcmp(captured.activity.details, "Exploring") == 0);

    /* Server-only publishes the public server without a zone, and an active
     * transition to Off clears exactly once without later republishing. */
    memset(&captured, 0, sizeof(captured));
    rich_presence_controller_init(&controller);
    input.playing = true;
    input.privacy = RICH_PRESENCE_SERVER;
    input.public_server = true;
    input.server = "Public Realm";
    input.zone = "Crystal Caverns";
    input.now_ms = 60000;
    input.now_unix = 6000;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 1U);
    require(strcmp(captured.activity.details, "Playing Atrinik Classic") == 0);
    require(strcmp(captured.activity.state, "On Public Realm") == 0);
    input.privacy = RICH_PRESENCE_OFF;
    input.now_ms = 60100;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.clears == 1U);
    require(captured.publishes == 1U);
    input.now_ms = 70000;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.clears == 1U);
    require(captured.publishes == 1U);

    /* Character disclosure is opt-in, session-local, and includes level
     * changes without changing the elapsed-play timestamp. */
    memset(&captured, 0, sizeof(captured));
    rich_presence_controller_init(&controller);
    input.privacy = RICH_PRESENCE_SERVER_ZONE_CHARACTER;
    input.server = "Public Realm";
    input.zone = "Crystal Caverns";
    input.character = "  Éowyn\t  ";
    input.level = 12U;
    input.character_available = true;
    input.playing = true;
    input.public_server = true;
    input.now_ms = 80000;
    input.now_unix = 8000;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 1U);
    require(strcmp(captured.activity.details, "Éowyn - Level 12") == 0);
    require(strcmp(captured.activity.state, "On Public Realm / Crystal Caverns") == 0);
    require(captured.activity.started_at == 8000U);
    input.privacy = RICH_PRESENCE_SERVER_ZONE;
    input.now_ms = 84100;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 1U);
    require(strstr(captured.activity.state, "Public Realm") != NULL);

    input.now_ms = 88200;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 2U);
    require(strstr(captured.activity.details, "Éowyn") == NULL);

    input.privacy = RICH_PRESENCE_SERVER_ZONE_CHARACTER;
    input.level = 13U;
    input.now_ms = 92300;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 3U);
    require(strcmp(captured.activity.details, "Éowyn - Level 13") == 0);
    require(captured.activity.started_at == 8000U);
    input.character_available = false;
    input.now_ms = 104600;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 4U);
    require(strstr(captured.activity.details, "Éowyn") == NULL);

    input.character = "AccountName";
    input.level = 99U;
    input.character_available = true;
    input.public_server = false;
    input.server = "192.0.2.1:13327";
    input.now_ms = 108700;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 5U);
    require(strcmp(captured.activity.state, "On Private server / Crystal Caverns") == 0);
    require(strstr(captured.activity.state, "192.0.2.1") == NULL);
}

typedef struct fake_io {
    unsigned int connects;
    unsigned int connect_failures;
    bool open;
    unsigned char incoming[70000];
    size_t incoming_size;
    size_t incoming_position;
    unsigned char outgoing[100000];
    size_t outgoing_size;
    bool write_blocked;
    unsigned int writes;
} fake_io_t;

static int fake_connect(void *context) {
    fake_io_t *io = context;
    io->connects++;
    if (io->connect_failures != 0U) {
        io->connect_failures--;
        return 0;
    }
    io->open = true;
    return 1;
}

static ptrdiff_t fake_read(void *context, void *buffer, size_t size) {
    fake_io_t *io = context;
    if (!io->open) {
        return -1;
    }
    if (io->incoming_position == io->incoming_size) {
        return -2;
    }
    size_t amount = io->incoming_size - io->incoming_position;
    if (amount > 3U) {
        amount = 3U;
    }
    if (amount > size) {
        amount = size;
    }
    memcpy(buffer, io->incoming + io->incoming_position, amount);
    io->incoming_position += amount;
    return (ptrdiff_t)amount;
}

static ptrdiff_t fake_write(void *context, const void *buffer, size_t size) {
    fake_io_t *io = context;
    io->writes++;
    if (!io->open) {
        return -1;
    }
    if (io->write_blocked) {
        return -2;
    }
    size_t amount = size > 5U ? 5U : size;
    require(io->outgoing_size + amount <= sizeof(io->outgoing));
    memcpy(io->outgoing + io->outgoing_size, buffer, amount);
    io->outgoing_size += amount;
    return (ptrdiff_t)amount;
}

static void fake_close(void *context) {
    fake_io_t *io = context;
    io->open = false;
}

static void append_frame(fake_io_t *io, uint32_t opcode, const char *payload) {
    size_t payload_size = strlen(payload);
    require(io->incoming_size + payload_size + 8U <= sizeof(io->incoming));
    unsigned char *frame = io->incoming + io->incoming_size;
    frame[0] = (unsigned char)opcode;
    frame[1] = (unsigned char)(opcode >> 8U);
    frame[2] = (unsigned char)(opcode >> 16U);
    frame[3] = (unsigned char)(opcode >> 24U);
    frame[4] = (unsigned char)payload_size;
    frame[5] = (unsigned char)(payload_size >> 8U);
    frame[6] = (unsigned char)(payload_size >> 16U);
    frame[7] = (unsigned char)(payload_size >> 24U);
    memcpy(frame + 8U, payload, payload_size);
    io->incoming_size += payload_size + 8U;
}

static bool contains(const unsigned char *haystack, size_t haystack_size, const char *needle) {
    size_t needle_size = strlen(needle);
    for (size_t i = 0; i + needle_size <= haystack_size; i++) {
        if (memcmp(haystack + i, needle, needle_size) == 0) {
            return true;
        }
    }
    return false;
}

static size_t
count_occurrences(const unsigned char *haystack, size_t haystack_size, const char *needle) {
    size_t count = 0;
    size_t needle_size = strlen(needle);
    for (size_t i = 0; i + needle_size <= haystack_size; i++) {
        if (memcmp(haystack + i, needle, needle_size) == 0) {
            count++;
            i += needle_size - 1U;
        }
    }
    return count;
}

static void pump_many(discord_rpc_t *rpc, uint64_t now, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        discord_rpc_pump(rpc, now + i);
    }
}

static void test_rpc(void) {
    fake_io_t fake = {.connect_failures = 2};
    discord_rpc_io_t operations = {
        .connect = fake_connect,
        .read = fake_read,
        .write = fake_write,
        .close = fake_close,
    };
    discord_rpc_t *rpc = discord_rpc_create_with_io("123456789012345678", &operations, &fake);
    require(rpc != NULL);
    discord_rpc_pump(rpc, 0);
    pump_many(rpc, 1, 20);
    require(fake.connects == 1U);
    discord_rpc_pump(rpc, 1000);
    require(fake.connects == 2U);
    discord_rpc_pump(rpc, 3000);
    pump_many(rpc, 3001, 20);
    require(fake.connects == 3U);
    require(fake.outgoing_size >= 8U);
    require(fake.outgoing[0] == 0U);
    require(contains(fake.outgoing, fake.outgoing_size, "\"client_id\":\"123456789012345678\""));

    append_frame(&fake, 1U, "{ \"cmd\":\"DISPATCH\",\"evt\" : \"READY\",\"nonce\":null }");
    pump_many(rpc, 3100, 20);
    require(discord_rpc_ready(rpc));

    discord_rpc_activity_t activity = {
        .details = "Exploring Crystal Caverns",
        .state = "On Public Realm",
        .started_at = 1234,
    };
    discord_rpc_set_activity(rpc, &activity);
    pump_many(rpc, 3200, 100);
    require(contains(fake.outgoing, fake.outgoing_size, "SET_ACTIVITY"));
    require(contains(fake.outgoing, fake.outgoing_size, "Exploring Crystal Caverns"));
    require(contains(fake.outgoing, fake.outgoing_size, "\"large_image\":\"atrinik\""));
    require(!contains(fake.outgoing, fake.outgoing_size, "password"));
    append_frame(&fake, 1U, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"1\",\"evt\":null,\"data\":{}}");
    pump_many(rpc, 3300, 30);

    size_t before_ping = fake.outgoing_size;
    append_frame(&fake, 3U, "ping-body");
    pump_many(rpc, 3400, 30);
    require(fake.outgoing_size >= before_ping + 8U + strlen("ping-body"));
    require(contains(fake.outgoing + before_ping, fake.outgoing_size - before_ping, "ping-body"));

    size_t before_pong = fake.outgoing_size;
    append_frame(&fake, 4U, "pong-body");
    pump_many(rpc, 3450, 10);
    require(discord_rpc_ready(rpc));
    require(fake.outgoing_size == before_pong);

    discord_rpc_clear_activity(rpc);
    pump_many(rpc, 3500, 60);
    require(contains(fake.outgoing, fake.outgoing_size, "\"activity\":null"));

    append_frame(&fake, 2U, "{}");
    pump_many(rpc, 3600, 10);
    require(!discord_rpc_ready(rpc));
    unsigned int connects = fake.connects;
    discord_rpc_pump(rpc, 5000);
    require(fake.connects == connects + 1U);
    discord_rpc_destroy(rpc, 2001);

    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    require(rpc != NULL);
    pump_many(rpc, 0, 20);
    append_frame(&fake, 1U, "not-json");
    pump_many(rpc, 100, 20);
    require(!discord_rpc_ready(rpc));
    discord_rpc_destroy(rpc, 101);

    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    require(rpc != NULL);
    pump_many(rpc, 0, 20);
    unsigned char oversized[8] = {1, 0, 0, 0, 1, 0, 1, 0};
    memcpy(fake.incoming, oversized, sizeof(oversized));
    fake.incoming_size = sizeof(oversized);
    pump_many(rpc, 100, 10);
    require(!discord_rpc_ready(rpc));
    require(!fake.open);
    discord_rpc_destroy(rpc, 101);

    const char *malformed[] = {
        "{\"evt\":\"READY\",}",
        "{\"evt\":\"READY\",\"data\":[}",
        "{\"evt\":\"REA\\qDY\"}",
        "{\"evt\":\"READY\",\"data\":\"\\udc00\"}",
        "{\"evt\":\"READY\",\"data\":\"\\ud800x\"}",
        "{\"evt\":\"READY\",\"data\":01}",
        "{\"evt\":\"READY\",\"data\":\"\xc0\xaf\"}",
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        memset(&fake, 0, sizeof(fake));
        rpc = discord_rpc_create_with_io("123", &operations, &fake);
        require(rpc != NULL);
        pump_many(rpc, 0, 20);
        append_frame(&fake, 1U, malformed[i]);
        pump_many(rpc, 100, 20);
        require(!discord_rpc_ready(rpc));
        require(!fake.open);
        discord_rpc_destroy(rpc, 101);
    }

    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    pump_many(rpc, 0, 20);
    append_frame(&fake, 1U, "{\"message\":\"fake \\\"evt\\\":\\\"READY\\\"\"}");
    pump_many(rpc, 100, 20);
    require(!discord_rpc_ready(rpc));
    require(fake.open);
    discord_rpc_destroy(rpc, 101);

    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    require(rpc != NULL);
    pump_many(rpc, 0, 20);
    append_frame(&fake, 1U, "{\"evt\":\"READY\"}");
    pump_many(rpc, 100, 20);
    require(discord_rpc_ready(rpc));
    append_frame(&fake, 1U, "{\"evt\":\"ERROR\",\"data\":{}}");
    pump_many(rpc, 200, 20);
    require(!discord_rpc_ready(rpc));
    require(discord_rpc_failure(rpc) == DISCORD_RPC_FAILURE_REMOTE_ERROR);
    discord_rpc_destroy(rpc, 201);

    /* A permanently blocked post-READY write reconnects and replays latest state. */
    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    require(rpc != NULL);
    pump_many(rpc, 0, 20);
    append_frame(&fake, 1U, "{\"evt\":\"READY\"}");
    pump_many(rpc, 100, 20);
    fake.write_blocked = true;
    snprintf(activity.details, sizeof(activity.details), "Latest Zone");
    discord_rpc_set_activity(rpc, &activity);
    discord_rpc_pump(rpc, 200);
    discord_rpc_pump(rpc, 5200);
    require(!fake.open);
    fake.write_blocked = false;
    discord_rpc_pump(rpc, 6200);
    append_frame(&fake, 1U, "{\"evt\":\"READY\"}");
    pump_many(rpc, 6300, 300);
    require(contains(fake.outgoing, fake.outgoing_size, "Latest Zone"));
    discord_rpc_destroy(rpc, 6600);

    /* A written activity without its matching response also reconnects. */
    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    require(rpc != NULL);
    pump_many(rpc, 0, 20);
    append_frame(&fake, 1U, "{\"evt\":\"READY\"}");
    pump_many(rpc, 100, 20);
    discord_rpc_set_activity(rpc, &activity);
    pump_many(rpc, 200, 300);
    require(discord_rpc_ready(rpc));
    discord_rpc_pump(rpc, 5300);
    require(!discord_rpc_ready(rpc));
    require(discord_rpc_failure(rpc) == DISCORD_RPC_FAILURE_TIMEOUT);
    discord_rpc_destroy(rpc, 5301);

    /* Destroy drains a complete privacy clear across legal partial writes. */
    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    require(rpc != NULL);
    pump_many(rpc, 0, 20);
    append_frame(&fake, 1U, "{\"evt\":\"READY\"}");
    pump_many(rpc, 100, 20);
    discord_rpc_set_activity(rpc, &activity);
    pump_many(rpc, 200, 300);
    append_frame(&fake, 1U, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"1\",\"data\":{}}");
    pump_many(rpc, 600, 30);
    fake.write_blocked = true;
    unsigned int writes_before_destroy = fake.writes;
    discord_rpc_destroy(rpc, 700);
    require(fake.writes - writes_before_destroy <= 2U);
    require(!fake.open);

    /* Missing READY is bounded by the handshake timeout. */
    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    require(rpc != NULL);
    pump_many(rpc, 0, 20);
    discord_rpc_pump(rpc, 5000);
    require(!fake.open);
    discord_rpc_destroy(rpc, 5001);

    /* A privacy clear closes instead of finishing a queued sensitive frame. */
    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    pump_many(rpc, 0, 20);
    append_frame(&fake, 1U, "{\"evt\":\"READY\"}");
    pump_many(rpc, 100, 20);
    snprintf(activity.details, sizeof(activity.details), "Sensitive Old Zone");
    discord_rpc_set_activity(rpc, &activity);
    discord_rpc_pump(rpc, 200);
    fake.write_blocked = true;
    discord_rpc_clear_activity(rpc);
    require(!fake.open);
    require(!contains(fake.outgoing, fake.outgoing_size, "Sensitive Old Zone"));
    discord_rpc_destroy(rpc, 201);

    /* READY/error churn cannot bypass the five-command wire window. */
    memset(&fake, 0, sizeof(fake));
    rpc = discord_rpc_create_with_io("123", &operations, &fake);
    discord_rpc_set_activity(rpc, &activity);
    const uint64_t connect_times[] = {0, 1500, 4000, 8500, 17000};
    for (size_t i = 0; i < sizeof(connect_times) / sizeof(connect_times[0]); i++) {
        discord_rpc_pump(rpc, connect_times[i]);
        append_frame(&fake, 1U, "{\"evt\":\"READY\"}");
        pump_many(rpc, connect_times[i] + 100U, 300);
        append_frame(&fake, 1U, "{\"evt\":\"ERROR\"}");
        pump_many(rpc, connect_times[i] + 500U, 20);
    }
    require(count_occurrences(fake.outgoing, fake.outgoing_size, "SET_ACTIVITY") == 4U);
    discord_rpc_destroy(rpc, 18000);
}

#ifdef WIN32
static void test_windows_pipe_identity(void) {
    char path[128];
    snprintf(path,
             sizeof(path),
             "\\\\.\\pipe\\atrinik-rich-presence-test-%lu",
             (unsigned long)GetCurrentProcessId());
    HANDLE server = CreateNamedPipeA(path,
                                     PIPE_ACCESS_DUPLEX,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
                                     1,
                                     1024,
                                     1024,
                                     5000,
                                     NULL);
    require(server != INVALID_HANDLE_VALUE);
    require(!ConnectNamedPipe(server, NULL));
    require(GetLastError() == ERROR_PIPE_LISTENING);
    HANDLE client =
        CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    require(client != INVALID_HANDLE_VALUE);
    require(discord_rpc_test_pipe_same_user(client));
    require(!discord_rpc_test_pipe_same_user(INVALID_HANDLE_VALUE));
    ULONGLONG started = GetTickCount64();
    require(!discord_rpc_test_try_pipe(path));
    require(GetTickCount64() - started < 1000U);
    CloseHandle(client);
    CloseHandle(server);
}
#endif

int main(void) {
    test_policy();
    test_rpc();
#ifdef WIN32
    test_windows_pipe_identity();
#endif
    return EXIT_SUCCESS;
}
