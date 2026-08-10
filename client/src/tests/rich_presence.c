#include <discord_rpc.h>
#include <rich_presence.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    input.now_ms = 12400;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.publishes == 4U);

    input.playing = false;
    input.now_ms = 12500;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.clears == 2U);
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.clears == 2U);

    input.playing = true;
    input.now_ms = 13000;
    input.now_unix = 2000;
    rich_presence_controller_tick(&controller, &input, &backend);
    require(captured.activity.started_at == 2000U);
    rich_presence_controller_stop(&controller, &backend);
    require(captured.clears == 3U);
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
    if (!io->open) {
        return -1;
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

    append_frame(&fake, 1U, "{ \"evt\" : \"READY\" }");
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

    size_t before_ping = fake.outgoing_size;
    append_frame(&fake, 3U, "ping-body");
    pump_many(rpc, 3400, 30);
    require(fake.outgoing_size >= before_ping + 8U + strlen("ping-body"));
    require(contains(fake.outgoing + before_ping, fake.outgoing_size - before_ping, "ping-body"));

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
}

int main(void) {
    test_policy();
    test_rpc();
    return EXIT_SUCCESS;
}
