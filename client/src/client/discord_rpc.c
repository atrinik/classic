#include <discord_rpc.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define DISCORD_RPC_FRAME_MAX 65536U
#define DISCORD_RPC_INPUT_MAX (DISCORD_RPC_FRAME_MAX + 8U)
#define DISCORD_RPC_OUTPUT_SLOTS 2U
#define DISCORD_RPC_HANDSHAKE_TIMEOUT_MS 5000U
#define DISCORD_RPC_BACKOFF_INITIAL_MS 1000U
#define DISCORD_RPC_BACKOFF_MAX_MS 60000U
#define DISCORD_RPC_FRAMES_PER_TICK 8U

enum {
    DISCORD_RPC_OPCODE_HANDSHAKE,
    DISCORD_RPC_OPCODE_FRAME,
    DISCORD_RPC_OPCODE_CLOSE,
    DISCORD_RPC_OPCODE_PING,
    DISCORD_RPC_OPCODE_PONG
};

typedef struct discord_rpc_output {
    unsigned char data[DISCORD_RPC_INPUT_MAX];
    size_t size;
    size_t position;
} discord_rpc_output_t;

typedef struct discord_rpc_platform {
#ifdef WIN32
    HANDLE pipe;
#else
    int socket;
#endif
} discord_rpc_platform_t;

struct discord_rpc {
    char application_id[21];
    discord_rpc_io_t io;
    void *io_context;
    discord_rpc_platform_t platform;
    bool owns_io;
    bool connected;
    bool ready;
    bool desired_set;
    bool desired_dirty;
    uint64_t connected_at;
    uint64_t reconnect_at;
    uint64_t backoff_ms;
    uint64_t nonce;
    discord_rpc_activity_t desired;
    unsigned char input[DISCORD_RPC_INPUT_MAX];
    size_t input_size;
    discord_rpc_output_t output[DISCORD_RPC_OUTPUT_SLOTS];
    size_t output_head;
    size_t output_count;
};

static uint32_t read_le32(const unsigned char *value) {
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) | ((uint32_t)value[2] << 16U) |
           ((uint32_t)value[3] << 24U);
}

static void write_le32(unsigned char *value, uint32_t number) {
    value[0] = (unsigned char)number;
    value[1] = (unsigned char)(number >> 8U);
    value[2] = (unsigned char)(number >> 16U);
    value[3] = (unsigned char)(number >> 24U);
}

#ifdef WIN32
static int platform_connect(void *context) {
    discord_rpc_platform_t *platform = context;
    if (platform->pipe != INVALID_HANDLE_VALUE) {
        return 1;
    }
    for (unsigned int i = 0; i < 10U; i++) {
        char path[64];
        snprintf(path, sizeof(path), "\\\\?\\pipe\\discord-ipc-%u", i);
        if (!WaitNamedPipeA(path, 0) && GetLastError() != ERROR_SEM_TIMEOUT) {
            continue;
        }
        HANDLE pipe =
            CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            continue;
        }
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
            CloseHandle(pipe);
            continue;
        }
        platform->pipe = pipe;
        return 1;
    }
    return 0;
}

static ptrdiff_t platform_read(void *context, void *buffer, size_t size) {
    discord_rpc_platform_t *platform = context;
    DWORD amount = 0;
    if (ReadFile(platform->pipe, buffer, (DWORD)size, &amount, NULL)) {
        return (ptrdiff_t)amount;
    }
    DWORD error = GetLastError();
    if (error == ERROR_NO_DATA || error == ERROR_PIPE_LISTENING) {
        return -2;
    }
    return -1;
}

static ptrdiff_t platform_write(void *context, const void *buffer, size_t size) {
    discord_rpc_platform_t *platform = context;
    DWORD amount = 0;
    if (WriteFile(platform->pipe, buffer, (DWORD)size, &amount, NULL)) {
        return (ptrdiff_t)amount;
    }
    return GetLastError() == ERROR_NO_DATA ? -2 : -1;
}

static void platform_close(void *context) {
    discord_rpc_platform_t *platform = context;
    if (platform->pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(platform->pipe);
        platform->pipe = INVALID_HANDLE_VALUE;
    }
}
#else
static int platform_try_path(discord_rpc_platform_t *platform, const char *directory, int index) {
    if (directory == NULL || directory[0] != '/') {
        return 0;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    int length =
        snprintf(address.sun_path, sizeof(address.sun_path), "%s/discord-ipc-%d", directory, index);
    if (length <= 0 || (size_t)length >= sizeof(address.sun_path)) {
        return 0;
    }
    int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return 0;
    }
    int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(descriptor);
        return 0;
    }
    if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) == 0) {
        platform->socket = descriptor;
        return 1;
    }
    close(descriptor);
    return 0;
}

static int platform_connect(void *context) {
    discord_rpc_platform_t *platform = context;
    if (platform->socket >= 0) {
        return 1;
    }
    const char *directories[] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), "/tmp"};
    for (size_t directory = 0; directory < sizeof(directories) / sizeof(directories[0]);
         directory++) {
        for (int index = 0; index < 10; index++) {
            if (platform_try_path(platform, directories[directory], index)) {
                return 1;
            }
        }
    }
    return 0;
}

static ptrdiff_t platform_read(void *context, void *buffer, size_t size) {
    discord_rpc_platform_t *platform = context;
    ssize_t amount = recv(platform->socket, buffer, size, 0);
    if (amount >= 0) {
        return amount;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -2 : -1;
}

static ptrdiff_t platform_write(void *context, const void *buffer, size_t size) {
    discord_rpc_platform_t *platform = context;
#ifdef MSG_NOSIGNAL
    ssize_t amount = send(platform->socket, buffer, size, MSG_NOSIGNAL);
#else
    ssize_t amount = send(platform->socket, buffer, size, 0);
#endif
    if (amount >= 0) {
        return amount;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -2 : -1;
}

static void platform_close(void *context) {
    discord_rpc_platform_t *platform = context;
    if (platform->socket >= 0) {
        close(platform->socket);
        platform->socket = -1;
    }
}
#endif

static bool
queue_frame(discord_rpc_t *rpc, uint32_t opcode, const void *payload, size_t payload_size) {
    if (payload_size > DISCORD_RPC_FRAME_MAX || rpc->output_count == DISCORD_RPC_OUTPUT_SLOTS) {
        return false;
    }
    size_t index = (rpc->output_head + rpc->output_count) % DISCORD_RPC_OUTPUT_SLOTS;
    discord_rpc_output_t *output = &rpc->output[index];
    write_le32(output->data, opcode);
    write_le32(output->data + 4U, (uint32_t)payload_size);
    if (payload_size != 0) {
        memcpy(output->data + 8U, payload, payload_size);
    }
    output->size = payload_size + 8U;
    output->position = 0;
    rpc->output_count++;
    return true;
}

static bool json_valid(const unsigned char *payload, size_t size) {
    if (size < 2 || payload[0] != '{' || payload[size - 1U] != '}') {
        return false;
    }
    bool string = false;
    bool escaped = false;
    unsigned int depth = 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char character = payload[i];
        if (string) {
            if (character < 0x20U) {
                return false;
            }
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                string = false;
            }
            continue;
        }
        if (character == '"') {
            string = true;
        } else if (character == '{' || character == '[') {
            depth++;
        } else if (character == '}' || character == ']') {
            if (depth == 0) {
                return false;
            }
            depth--;
        } else if (character < 0x20U && character != '\t' && character != '\n' &&
                   character != '\r') {
            return false;
        }
    }
    return !string && !escaped && depth == 0;
}

static bool json_event_is(const unsigned char *payload, size_t size, const char *event) {
    const char key[] = "\"evt\"";
    for (size_t i = 0; i + sizeof(key) - 1U < size; i++) {
        if (memcmp(payload + i, key, sizeof(key) - 1U) != 0) {
            continue;
        }
        size_t position = i + sizeof(key) - 1U;
        while (position < size && strchr(" \t\r\n", payload[position]) != NULL) {
            position++;
        }
        if (position == size || payload[position++] != ':') {
            continue;
        }
        while (position < size && strchr(" \t\r\n", payload[position]) != NULL) {
            position++;
        }
        size_t event_size = strlen(event);
        return position + event_size + 2U <= size && payload[position] == '"' &&
               memcmp(payload + position + 1U, event, event_size) == 0 &&
               payload[position + event_size + 1U] == '"';
    }
    return false;
}

static bool json_append(char *buffer, size_t capacity, size_t *position, const char *value) {
    for (const unsigned char *cp = (const unsigned char *)value; *cp != '\0'; cp++) {
        const char *escape = NULL;
        if (*cp == '"') {
            escape = "\\\"";
        } else if (*cp == '\\') {
            escape = "\\\\";
        }
        if (escape != NULL) {
            if (*position + 2U >= capacity) {
                return false;
            }
            buffer[(*position)++] = escape[0];
            buffer[(*position)++] = escape[1];
        } else {
            if (*position + 1U >= capacity) {
                return false;
            }
            buffer[(*position)++] = (char)*cp;
        }
    }
    buffer[*position] = '\0';
    return true;
}

static bool queue_activity(discord_rpc_t *rpc) {
    char payload[4096];
    int prefix;
#ifdef WIN32
    unsigned long process_id = GetCurrentProcessId();
#else
    unsigned long process_id = (unsigned long)getpid();
#endif
    rpc->nonce++;
    if (rpc->desired_set) {
        prefix = snprintf(payload,
                          sizeof(payload),
                          "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
                          "\"details\":\"",
                          process_id);
        if (prefix < 0 || (size_t)prefix >= sizeof(payload)) {
            return false;
        }
        size_t position = (size_t)prefix;
        if (!json_append(payload, sizeof(payload), &position, rpc->desired.details)) {
            return false;
        }
        int middle = snprintf(payload + position,
                              sizeof(payload) - position,
                              "\",\"timestamps\":{\"start\":%" PRIu64
                              "},\"assets\":{\"large_image\":\"atrinik\","
                              "\"large_text\":\"Atrinik Classic\"}",
                              rpc->desired.started_at);
        if (middle < 0 || (size_t)middle >= sizeof(payload) - position) {
            return false;
        }
        position += (size_t)middle;
        if (rpc->desired.state[0] != '\0') {
            static const char state_prefix[] = ",\"state\":\"";
            if (position + sizeof(state_prefix) >= sizeof(payload)) {
                return false;
            }
            memcpy(payload + position, state_prefix, sizeof(state_prefix) - 1U);
            position += sizeof(state_prefix) - 1U;
            if (!json_append(payload, sizeof(payload), &position, rpc->desired.state) ||
                position + 1U >= sizeof(payload)) {
                return false;
            }
            payload[position++] = '"';
        }
        int suffix = snprintf(payload + position,
                              sizeof(payload) - position,
                              "}},\"nonce\":\"%" PRIu64 "\"}",
                              rpc->nonce);
        if (suffix < 0 || (size_t)suffix >= sizeof(payload) - position) {
            return false;
        }
    } else {
        prefix = snprintf(payload,
                          sizeof(payload),
                          "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,"
                          "\"activity\":null},\"nonce\":\"%" PRIu64 "\"}",
                          process_id,
                          rpc->nonce);
        if (prefix < 0 || (size_t)prefix >= sizeof(payload)) {
            return false;
        }
    }
    return queue_frame(rpc, DISCORD_RPC_OPCODE_FRAME, payload, strlen(payload));
}

static void disconnect_rpc(discord_rpc_t *rpc, uint64_t now_ms) {
    rpc->io.close(rpc->io_context);
    rpc->connected = false;
    rpc->ready = false;
    rpc->input_size = 0;
    rpc->output_head = 0;
    rpc->output_count = 0;
    rpc->desired_dirty = true;
    rpc->reconnect_at = now_ms + rpc->backoff_ms;
    rpc->backoff_ms = rpc->backoff_ms < DISCORD_RPC_BACKOFF_MAX_MS / 2U
                          ? rpc->backoff_ms * 2U
                          : DISCORD_RPC_BACKOFF_MAX_MS;
}

static bool handle_frame(discord_rpc_t *rpc,
                         uint32_t opcode,
                         const unsigned char *payload,
                         size_t payload_size) {
    if (opcode == DISCORD_RPC_OPCODE_PING) {
        return queue_frame(rpc, DISCORD_RPC_OPCODE_PONG, payload, payload_size);
    }
    if (opcode == DISCORD_RPC_OPCODE_PONG) {
        return true;
    }
    if (opcode == DISCORD_RPC_OPCODE_CLOSE) {
        return false;
    }
    if (opcode != DISCORD_RPC_OPCODE_FRAME || !json_valid(payload, payload_size)) {
        return false;
    }
    if (json_event_is(payload, payload_size, "ERROR")) {
        return false;
    }
    if (json_event_is(payload, payload_size, "READY")) {
        rpc->ready = true;
        rpc->backoff_ms = DISCORD_RPC_BACKOFF_INITIAL_MS;
        rpc->desired_dirty = true;
    }
    return true;
}

static bool read_frames(discord_rpc_t *rpc) {
    if (rpc->input_size < sizeof(rpc->input)) {
        ptrdiff_t amount = rpc->io.read(rpc->io_context,
                                        rpc->input + rpc->input_size,
                                        sizeof(rpc->input) - rpc->input_size);
        if (amount == 0 || amount == -1) {
            return false;
        }
        if (amount > 0) {
            rpc->input_size += (size_t)amount;
        }
    }

    for (unsigned int count = 0; count < DISCORD_RPC_FRAMES_PER_TICK; count++) {
        if (rpc->input_size < 8U) {
            break;
        }
        uint32_t opcode = read_le32(rpc->input);
        uint32_t payload_size = read_le32(rpc->input + 4U);
        if (payload_size > DISCORD_RPC_FRAME_MAX) {
            return false;
        }
        size_t frame_size = (size_t)payload_size + 8U;
        if (rpc->input_size < frame_size) {
            break;
        }
        if (!handle_frame(rpc, opcode, rpc->input + 8U, payload_size)) {
            return false;
        }
        memmove(rpc->input, rpc->input + frame_size, rpc->input_size - frame_size);
        rpc->input_size -= frame_size;
    }
    return rpc->input_size != sizeof(rpc->input);
}

static bool write_frames(discord_rpc_t *rpc) {
    if (rpc->output_count == 0) {
        return true;
    }
    discord_rpc_output_t *output = &rpc->output[rpc->output_head];
    ptrdiff_t amount = rpc->io.write(rpc->io_context,
                                     output->data + output->position,
                                     output->size - output->position);
    if (amount == -1 || amount == 0) {
        return false;
    }
    if (amount > 0) {
        output->position += (size_t)amount;
        if (output->position == output->size) {
            memset(output, 0, sizeof(*output));
            rpc->output_head = (rpc->output_head + 1U) % DISCORD_RPC_OUTPUT_SLOTS;
            rpc->output_count--;
        }
    }
    return true;
}

discord_rpc_t *discord_rpc_create_with_io(const char *application_id,
                                          const discord_rpc_io_t *io,
                                          void *io_context) {
    if (application_id == NULL || strlen(application_id) > 20U || io == NULL ||
        io->connect == NULL || io->read == NULL || io->write == NULL || io->close == NULL) {
        return NULL;
    }
    discord_rpc_t *rpc = calloc(1, sizeof(*rpc));
    if (rpc == NULL) {
        return NULL;
    }
    snprintf(rpc->application_id, sizeof(rpc->application_id), "%s", application_id);
    rpc->io = *io;
    rpc->io_context = io_context;
    rpc->backoff_ms = DISCORD_RPC_BACKOFF_INITIAL_MS;
    return rpc;
}

discord_rpc_t *discord_rpc_create(const char *application_id) {
    discord_rpc_io_t io = {
        .connect = platform_connect,
        .read = platform_read,
        .write = platform_write,
        .close = platform_close,
    };
    discord_rpc_t *rpc = discord_rpc_create_with_io(application_id, &io, NULL);
    if (rpc == NULL) {
        return NULL;
    }
#ifdef WIN32
    rpc->platform.pipe = INVALID_HANDLE_VALUE;
#else
    rpc->platform.socket = -1;
#endif
    rpc->io_context = &rpc->platform;
    rpc->owns_io = true;
    return rpc;
}

void discord_rpc_pump(discord_rpc_t *rpc, uint64_t now_ms) {
    if (rpc == NULL) {
        return;
    }
    if (!rpc->connected) {
        if (now_ms < rpc->reconnect_at) {
            return;
        }
        if (rpc->io.connect(rpc->io_context) != 1) {
            disconnect_rpc(rpc, now_ms);
            return;
        }
        rpc->connected = true;
        rpc->connected_at = now_ms;
        char handshake[64];
        int length = snprintf(handshake,
                              sizeof(handshake),
                              "{\"v\":1,\"client_id\":\"%s\"}",
                              rpc->application_id);
        if (length <= 0 || (size_t)length >= sizeof(handshake) ||
            !queue_frame(rpc, DISCORD_RPC_OPCODE_HANDSHAKE, handshake, (size_t)length)) {
            disconnect_rpc(rpc, now_ms);
            return;
        }
    }

    if ((!write_frames(rpc) || !read_frames(rpc)) ||
        (!rpc->ready && now_ms - rpc->connected_at >= DISCORD_RPC_HANDSHAKE_TIMEOUT_MS)) {
        disconnect_rpc(rpc, now_ms);
        return;
    }
    if (rpc->ready && rpc->desired_dirty && rpc->output_count < DISCORD_RPC_OUTPUT_SLOTS &&
        queue_activity(rpc)) {
        rpc->desired_dirty = false;
        if (!write_frames(rpc)) {
            disconnect_rpc(rpc, now_ms);
        }
    }
}

void discord_rpc_set_activity(discord_rpc_t *rpc, const discord_rpc_activity_t *activity) {
    if (rpc == NULL || activity == NULL) {
        return;
    }
    rpc->desired = *activity;
    rpc->desired_set = true;
    rpc->desired_dirty = true;
}

void discord_rpc_clear_activity(discord_rpc_t *rpc) {
    if (rpc == NULL) {
        return;
    }
    memset(&rpc->desired, 0, sizeof(rpc->desired));
    rpc->desired_set = false;
    rpc->desired_dirty = true;
}

bool discord_rpc_ready(const discord_rpc_t *rpc) {
    return rpc != NULL && rpc->ready;
}

void discord_rpc_destroy(discord_rpc_t *rpc, uint64_t now_ms) {
    if (rpc == NULL) {
        return;
    }
    if (rpc->connected) {
        if (rpc->desired_set) {
            discord_rpc_clear_activity(rpc);
        }
        for (unsigned int i = 0; i < 4U && rpc->connected; i++) {
            discord_rpc_pump(rpc, now_ms);
        }
        rpc->io.close(rpc->io_context);
    }
    free(rpc);
}
