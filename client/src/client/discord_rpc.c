#if !defined(WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <discord_rpc.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
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
#define DISCORD_RPC_IO_TIMEOUT_MS 5000U
#define DISCORD_RPC_ACK_TIMEOUT_MS 5000U
#define DISCORD_RPC_BACKOFF_INITIAL_MS 1000U
#define DISCORD_RPC_BACKOFF_MAX_MS 60000U
#define DISCORD_RPC_FRAMES_PER_TICK 8U
#define DISCORD_RPC_COMMAND_WINDOW_MS 20000U
#define DISCORD_RPC_COMMAND_CAPACITY 5U

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
    bool tracks_ack;
    uint64_t nonce;
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
    bool desired_known;
    bool desired_dirty;
    uint64_t connected_at;
    uint64_t ready_at;
    uint64_t input_progress_at;
    uint64_t output_progress_at;
    uint64_t ack_started_at;
    uint64_t reconnect_at;
    uint64_t backoff_ms;
    uint64_t nonce;
    uint64_t pending_nonce;
    bool pending_ack;
    discord_rpc_failure_t failure;
    uint64_t now_ms;
    discord_rpc_activity_t desired;
    unsigned char input[DISCORD_RPC_INPUT_MAX];
    size_t input_size;
    discord_rpc_output_t output[DISCORD_RPC_OUTPUT_SLOTS];
    size_t output_head;
    size_t output_count;
    size_t queued_activities;
    uint64_t command_times[DISCORD_RPC_COMMAND_CAPACITY];
    size_t command_start;
    size_t command_count;
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
static bool platform_pipe_same_user(HANDLE pipe) {
    ULONG process_id;
    if (!GetNamedPipeServerProcessId(pipe, &process_id)) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == NULL) {
        return false;
    }
    HANDLE process_token = NULL;
    HANDLE current_token = NULL;
    bool same = false;
    if (!OpenProcessToken(process, TOKEN_QUERY, &process_token) ||
        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current_token)) {
        goto cleanup;
    }
    DWORD process_size = 0;
    DWORD current_size = 0;
    GetTokenInformation(process_token, TokenUser, NULL, 0, &process_size);
    GetTokenInformation(current_token, TokenUser, NULL, 0, &current_size);
    if (process_size == 0U || current_size == 0U || process_size > 4096U || current_size > 4096U) {
        goto cleanup;
    }
    TOKEN_USER *process_user = malloc(process_size);
    TOKEN_USER *current_user = malloc(current_size);
    if (process_user == NULL || current_user == NULL) {
        free(process_user);
        free(current_user);
        goto cleanup;
    }
    if (GetTokenInformation(process_token, TokenUser, process_user, process_size, &process_size) &&
        GetTokenInformation(current_token, TokenUser, current_user, current_size, &current_size)) {
        same = EqualSid(process_user->User.Sid, current_user->User.Sid) != FALSE;
    }
    free(process_user);
    free(current_user);

cleanup:
    if (process_token != NULL) {
        CloseHandle(process_token);
    }
    if (current_token != NULL) {
        CloseHandle(current_token);
    }
    CloseHandle(process);
    return same;
}

#ifdef DISCORD_RPC_TESTING
bool discord_rpc_test_pipe_same_user(void *pipe) {
    return platform_pipe_same_user((HANDLE)pipe);
}
#endif

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
        if (!platform_pipe_same_user(pipe)) {
            CloseHandle(pipe);
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
#ifdef __linux__
        struct ucred credentials;
        socklen_t credentials_size = sizeof(credentials);
        if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0 ||
            credentials_size != sizeof(credentials) || credentials.uid != geteuid()) {
            close(descriptor);
            return 0;
        }
#endif
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
    output->tracks_ack = false;
    output->nonce = 0;
    if (rpc->output_count == 0U) {
        rpc->output_progress_at = rpc->now_ms;
    }
    rpc->output_count++;
    return true;
}

typedef struct json_parser {
    const unsigned char *data;
    size_t size;
    size_t position;
} json_parser_t;

typedef struct json_string {
    const unsigned char *data;
    size_t size;
    bool escaped;
} json_string_t;

typedef struct json_message {
    bool ready;
    bool error;
    bool set_activity;
    bool has_nonce;
    uint64_t nonce;
} json_message_t;

static void json_space(json_parser_t *parser) {
    while (parser->position < parser->size &&
           strchr(" \t\r\n", parser->data[parser->position]) != NULL) {
        parser->position++;
    }
}

static bool json_hex4(json_parser_t *parser, uint32_t *value) {
    if (parser->size - parser->position < 4U) {
        return false;
    }
    uint32_t result = 0;
    for (unsigned int i = 0; i < 4U; i++) {
        unsigned char character = parser->data[parser->position++];
        unsigned int digit;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10U;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10U;
        } else {
            return false;
        }
        result = (result << 4U) | digit;
    }
    *value = result;
    return true;
}

static bool json_utf8(json_parser_t *parser, unsigned char leading) {
    size_t continuation;
    uint32_t decoded;
    if (leading >= 0xc2U && leading <= 0xdfU) {
        continuation = 1U;
        decoded = leading & 0x1fU;
    } else if (leading >= 0xe0U && leading <= 0xefU) {
        continuation = 2U;
        decoded = leading & 0x0fU;
    } else if (leading >= 0xf0U && leading <= 0xf4U) {
        continuation = 3U;
        decoded = leading & 0x07U;
    } else {
        return false;
    }
    if (parser->size - parser->position < continuation) {
        return false;
    }
    for (size_t i = 0; i < continuation; i++) {
        unsigned char character = parser->data[parser->position++];
        if ((character & 0xc0U) != 0x80U) {
            return false;
        }
        decoded = (decoded << 6U) | (character & 0x3fU);
    }
    return !((leading == 0xe0U && decoded < 0x800U) || (leading == 0xedU && decoded >= 0xd800U) ||
             (leading == 0xf0U && decoded < 0x10000U) || decoded > 0x10ffffU);
}

static bool json_string_parse(json_parser_t *parser, json_string_t *string) {
    if (parser->position == parser->size || parser->data[parser->position++] != '"') {
        return false;
    }
    size_t start = parser->position;
    bool escaped = false;
    while (parser->position < parser->size) {
        unsigned char character = parser->data[parser->position++];
        if (character == '"') {
            if (string != NULL) {
                string->data = parser->data + start;
                string->size = parser->position - start - 1U;
                string->escaped = escaped;
            }
            return true;
        }
        if (character < 0x20U) {
            return false;
        }
        if (character >= 0x80U && !json_utf8(parser, character)) {
            return false;
        }
        if (character != '\\') {
            continue;
        }
        escaped = true;
        if (parser->position == parser->size) {
            return false;
        }
        character = parser->data[parser->position++];
        if (strchr("\"\\/bfnrt", character) != NULL) {
            continue;
        }
        if (character != 'u') {
            return false;
        }
        uint32_t codepoint;
        if (!json_hex4(parser, &codepoint) || (codepoint >= 0xdc00U && codepoint <= 0xdfffU)) {
            return false;
        }
        if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
            if (parser->size - parser->position < 6U || parser->data[parser->position++] != '\\' ||
                parser->data[parser->position++] != 'u' || !json_hex4(parser, &codepoint) ||
                codepoint < 0xdc00U || codepoint > 0xdfffU) {
                return false;
            }
        }
    }
    return false;
}

static bool json_value(json_parser_t *parser, unsigned int depth);

static bool
json_compound(json_parser_t *parser, unsigned char open, unsigned char close, unsigned int depth) {
    if (depth >= 32U || parser->position == parser->size ||
        parser->data[parser->position++] != open) {
        return false;
    }
    json_space(parser);
    if (parser->position < parser->size && parser->data[parser->position] == close) {
        parser->position++;
        return true;
    }
    for (;;) {
        if (open == '{' && !json_string_parse(parser, NULL)) {
            return false;
        }
        if (open == '{') {
            json_space(parser);
            if (parser->position == parser->size || parser->data[parser->position++] != ':') {
                return false;
            }
            json_space(parser);
        }
        if (!json_value(parser, depth + 1U)) {
            return false;
        }
        json_space(parser);
        if (parser->position == parser->size) {
            return false;
        }
        unsigned char separator = parser->data[parser->position++];
        if (separator == close) {
            return true;
        }
        if (separator != ',') {
            return false;
        }
        json_space(parser);
        if (parser->position == parser->size || parser->data[parser->position] == close) {
            return false;
        }
    }
}

static bool json_number(json_parser_t *parser) {
    size_t start = parser->position;
    if (parser->data[parser->position] == '-') {
        parser->position++;
    }
    if (parser->position == parser->size) {
        return false;
    }
    if (parser->data[parser->position] == '0') {
        parser->position++;
        if (parser->position < parser->size && isdigit(parser->data[parser->position])) {
            return false;
        }
    } else {
        if (parser->data[parser->position] < '1' || parser->data[parser->position] > '9') {
            return false;
        }
        while (parser->position < parser->size && isdigit(parser->data[parser->position])) {
            parser->position++;
        }
    }
    if (parser->position < parser->size && parser->data[parser->position] == '.') {
        parser->position++;
        size_t digits = parser->position;
        while (parser->position < parser->size && isdigit(parser->data[parser->position])) {
            parser->position++;
        }
        if (digits == parser->position) {
            return false;
        }
    }
    if (parser->position < parser->size &&
        (parser->data[parser->position] == 'e' || parser->data[parser->position] == 'E')) {
        parser->position++;
        if (parser->position < parser->size &&
            (parser->data[parser->position] == '+' || parser->data[parser->position] == '-')) {
            parser->position++;
        }
        size_t digits = parser->position;
        while (parser->position < parser->size && isdigit(parser->data[parser->position])) {
            parser->position++;
        }
        if (digits == parser->position) {
            return false;
        }
    }
    return parser->position != start;
}

static bool json_value(json_parser_t *parser, unsigned int depth) {
    if (parser->position == parser->size) {
        return false;
    }
    unsigned char character = parser->data[parser->position];
    if (character == '"') {
        return json_string_parse(parser, NULL);
    }
    if (character == '{') {
        return json_compound(parser, '{', '}', depth);
    }
    if (character == '[') {
        return json_compound(parser, '[', ']', depth);
    }
    static const char *const literals[] = {"true", "false", "null"};
    for (size_t i = 0; i < sizeof(literals) / sizeof(literals[0]); i++) {
        size_t length = strlen(literals[i]);
        if (parser->size - parser->position >= length &&
            memcmp(parser->data + parser->position, literals[i], length) == 0) {
            parser->position += length;
            return true;
        }
    }
    return character == '-' || isdigit(character) ? json_number(parser) : false;
}

static bool json_string_is(const json_string_t *string, const char *value) {
    size_t length = strlen(value);
    return !string->escaped && string->size == length && memcmp(string->data, value, length) == 0;
}

static bool json_nonce(const json_string_t *string, uint64_t *nonce) {
    if (string->escaped || string->size == 0U || string->size > 20U) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < string->size; i++) {
        if (!isdigit(string->data[i])) {
            return false;
        }
        uint64_t digit = string->data[i] - '0';
        if (value > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    *nonce = value;
    return true;
}

static bool json_message_parse(const unsigned char *payload, size_t size, json_message_t *message) {
    json_parser_t parser = {.data = payload, .size = size};
    memset(message, 0, sizeof(*message));
    json_space(&parser);
    if (parser.position == parser.size || parser.data[parser.position++] != '{') {
        return false;
    }
    json_space(&parser);
    bool seen_evt = false;
    bool seen_cmd = false;
    bool seen_nonce = false;
    if (parser.position < parser.size && parser.data[parser.position] == '}') {
        parser.position++;
    } else {
        for (;;) {
            json_string_t key;
            if (!json_string_parse(&parser, &key)) {
                return false;
            }
            json_space(&parser);
            if (parser.position == parser.size || parser.data[parser.position++] != ':') {
                return false;
            }
            json_space(&parser);
            bool critical = json_string_is(&key, "evt") || json_string_is(&key, "cmd") ||
                            json_string_is(&key, "nonce");
            if (critical && parser.position < parser.size && parser.data[parser.position] == '"') {
                json_string_t value;
                if (!json_string_parse(&parser, &value)) {
                    return false;
                }
                if (json_string_is(&key, "evt")) {
                    if (seen_evt) {
                        return false;
                    }
                    seen_evt = true;
                    message->ready = json_string_is(&value, "READY");
                    message->error = json_string_is(&value, "ERROR");
                } else if (json_string_is(&key, "cmd")) {
                    if (seen_cmd) {
                        return false;
                    }
                    seen_cmd = true;
                    message->set_activity = json_string_is(&value, "SET_ACTIVITY");
                } else {
                    if (seen_nonce || !json_nonce(&value, &message->nonce)) {
                        return false;
                    }
                    seen_nonce = true;
                    message->has_nonce = true;
                }
            } else if ((json_string_is(&key, "nonce") || json_string_is(&key, "evt")) &&
                       parser.size - parser.position >= 4U &&
                       memcmp(parser.data + parser.position, "null", 4U) == 0) {
                if (json_string_is(&key, "nonce")) {
                    if (seen_nonce) {
                        return false;
                    }
                    seen_nonce = true;
                } else {
                    if (seen_evt) {
                        return false;
                    }
                    seen_evt = true;
                }
                parser.position += 4U;
            } else if (critical || !json_value(&parser, 1U)) {
                return false;
            }
            json_space(&parser);
            if (parser.position == parser.size) {
                return false;
            }
            unsigned char separator = parser.data[parser.position++];
            if (separator == '}') {
                break;
            }
            if (separator != ',') {
                return false;
            }
            json_space(&parser);
            if (parser.position == parser.size || parser.data[parser.position] == '}') {
                return false;
            }
        }
    }
    json_space(&parser);
    return parser.position == parser.size;
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

static void command_purge(discord_rpc_t *rpc) {
    while (rpc->command_count != 0U &&
           rpc->now_ms - rpc->command_times[rpc->command_start] >= DISCORD_RPC_COMMAND_WINDOW_MS) {
        rpc->command_start = (rpc->command_start + 1U) % DISCORD_RPC_COMMAND_CAPACITY;
        rpc->command_count--;
    }
}

static bool command_allowed(discord_rpc_t *rpc) {
    command_purge(rpc);
    size_t limit =
        rpc->desired_set ? DISCORD_RPC_COMMAND_CAPACITY - 1U : DISCORD_RPC_COMMAND_CAPACITY;
    return rpc->command_count < limit;
}

static void command_record(discord_rpc_t *rpc) {
    size_t position = (rpc->command_start + rpc->command_count) % DISCORD_RPC_COMMAND_CAPACITY;
    rpc->command_times[position] = rpc->now_ms;
    rpc->command_count++;
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
    if (!queue_frame(rpc, DISCORD_RPC_OPCODE_FRAME, payload, strlen(payload))) {
        return false;
    }
    size_t index = (rpc->output_head + rpc->output_count - 1U) % DISCORD_RPC_OUTPUT_SLOTS;
    rpc->output[index].tracks_ack = true;
    rpc->output[index].nonce = rpc->nonce;
    rpc->queued_activities++;
    command_record(rpc);
    return true;
}

static void disconnect_rpc(discord_rpc_t *rpc, uint64_t now_ms) {
    rpc->io.close(rpc->io_context);
    rpc->connected = false;
    rpc->ready = false;
    rpc->input_size = 0;
    rpc->output_head = 0;
    rpc->output_count = 0;
    rpc->queued_activities = 0;
    rpc->pending_ack = false;
    rpc->desired_dirty = rpc->desired_known && rpc->desired_set;
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
        if (!queue_frame(rpc, DISCORD_RPC_OPCODE_PONG, payload, payload_size)) {
            rpc->failure = DISCORD_RPC_FAILURE_IO;
            return false;
        }
        return true;
    }
    if (opcode == DISCORD_RPC_OPCODE_PONG) {
        return true;
    }
    if (opcode == DISCORD_RPC_OPCODE_CLOSE) {
        rpc->failure = DISCORD_RPC_FAILURE_REMOTE_CLOSE;
        return false;
    }
    if (opcode != DISCORD_RPC_OPCODE_FRAME) {
        rpc->failure = DISCORD_RPC_FAILURE_PROTOCOL;
        return false;
    }
    json_message_t message;
    if (!json_message_parse(payload, payload_size, &message)) {
        rpc->failure = DISCORD_RPC_FAILURE_PROTOCOL;
        return false;
    }
    if (message.error) {
        rpc->failure = DISCORD_RPC_FAILURE_REMOTE_ERROR;
        return false;
    }
    if (message.ready) {
        rpc->ready = true;
        rpc->ready_at = rpc->now_ms;
        rpc->failure = DISCORD_RPC_FAILURE_NONE;
        rpc->desired_dirty = rpc->desired_known && rpc->desired_set;
    }
    if (message.set_activity && message.has_nonce && rpc->pending_ack &&
        message.nonce == rpc->pending_nonce) {
        rpc->pending_ack = false;
        rpc->backoff_ms = DISCORD_RPC_BACKOFF_INITIAL_MS;
    }
    return true;
}

static bool read_frames(discord_rpc_t *rpc) {
    if (rpc->input_size < sizeof(rpc->input)) {
        ptrdiff_t amount = rpc->io.read(rpc->io_context,
                                        rpc->input + rpc->input_size,
                                        sizeof(rpc->input) - rpc->input_size);
        if (amount == 0 || amount == -1) {
            rpc->failure = DISCORD_RPC_FAILURE_IO;
            return false;
        }
        if (amount > 0) {
            rpc->input_size += (size_t)amount;
            rpc->input_progress_at = rpc->now_ms;
        }
    }

    for (unsigned int count = 0; count < DISCORD_RPC_FRAMES_PER_TICK; count++) {
        if (rpc->input_size < 8U) {
            break;
        }
        uint32_t opcode = read_le32(rpc->input);
        uint32_t payload_size = read_le32(rpc->input + 4U);
        if (payload_size > DISCORD_RPC_FRAME_MAX) {
            rpc->failure = DISCORD_RPC_FAILURE_PROTOCOL;
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
    if (rpc->input_size == sizeof(rpc->input)) {
        rpc->failure = DISCORD_RPC_FAILURE_PROTOCOL;
        return false;
    }
    return true;
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
        rpc->failure = DISCORD_RPC_FAILURE_IO;
        return false;
    }
    if (amount > 0) {
        rpc->output_progress_at = rpc->now_ms;
        output->position += (size_t)amount;
        if (output->position == output->size) {
            if (output->tracks_ack) {
                rpc->queued_activities--;
                rpc->pending_ack = true;
                rpc->pending_nonce = output->nonce;
                rpc->ack_started_at = rpc->now_ms;
            }
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
    rpc->now_ms = now_ms;
    if (!rpc->connected) {
        if (now_ms < rpc->reconnect_at) {
            return;
        }
        if (rpc->io.connect(rpc->io_context) != 1) {
            rpc->failure = DISCORD_RPC_FAILURE_CONNECT;
            disconnect_rpc(rpc, now_ms);
            return;
        }
        rpc->connected = true;
        rpc->connected_at = now_ms;
        rpc->input_progress_at = now_ms;
        rpc->output_progress_at = now_ms;
        char handshake[64];
        int length = snprintf(handshake,
                              sizeof(handshake),
                              "{\"v\":1,\"client_id\":\"%s\"}",
                              rpc->application_id);
        if (length <= 0 || (size_t)length >= sizeof(handshake) ||
            !queue_frame(rpc, DISCORD_RPC_OPCODE_HANDSHAKE, handshake, (size_t)length)) {
            rpc->failure = DISCORD_RPC_FAILURE_IO;
            disconnect_rpc(rpc, now_ms);
            return;
        }
    }

    if (!write_frames(rpc) || !read_frames(rpc)) {
        disconnect_rpc(rpc, now_ms);
        return;
    }
    if ((!rpc->ready && now_ms - rpc->connected_at >= DISCORD_RPC_HANDSHAKE_TIMEOUT_MS) ||
        (rpc->input_size != 0U && now_ms - rpc->input_progress_at >= DISCORD_RPC_IO_TIMEOUT_MS) ||
        (rpc->output_count != 0U &&
         now_ms - rpc->output_progress_at >= DISCORD_RPC_IO_TIMEOUT_MS) ||
        (rpc->pending_ack && now_ms - rpc->ack_started_at >= DISCORD_RPC_ACK_TIMEOUT_MS)) {
        rpc->failure = DISCORD_RPC_FAILURE_TIMEOUT;
        disconnect_rpc(rpc, now_ms);
        return;
    }
    if (rpc->ready && now_ms - rpc->ready_at >= DISCORD_RPC_HANDSHAKE_TIMEOUT_MS) {
        rpc->backoff_ms = DISCORD_RPC_BACKOFF_INITIAL_MS;
    }
    if (rpc->ready && !rpc->pending_ack && rpc->queued_activities == 0U && rpc->desired_dirty &&
        rpc->output_count < DISCORD_RPC_OUTPUT_SLOTS && command_allowed(rpc) &&
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
    rpc->desired_known = true;
    rpc->desired_dirty = true;
}

void discord_rpc_clear_activity(discord_rpc_t *rpc) {
    if (rpc == NULL) {
        return;
    }
    memset(&rpc->desired, 0, sizeof(rpc->desired));
    rpc->desired_set = false;
    rpc->desired_known = true;
    rpc->desired_dirty = true;
    if (rpc->connected && rpc->queued_activities != 0U) {
        /* Never finish a superseded sensitive frame after a privacy change. */
        disconnect_rpc(rpc, rpc->now_ms);
        return;
    }
    /* A privacy clear supersedes any acknowledgement for older activity. */
    rpc->pending_ack = false;
    rpc->queued_activities = 0U;
    for (size_t i = 0; i < rpc->output_count; i++) {
        size_t index = (rpc->output_head + i) % DISCORD_RPC_OUTPUT_SLOTS;
        rpc->output[index].tracks_ack = false;
    }
}

bool discord_rpc_ready(const discord_rpc_t *rpc) {
    return rpc != NULL && rpc->ready;
}

discord_rpc_failure_t discord_rpc_failure(const discord_rpc_t *rpc) {
    return rpc != NULL ? rpc->failure : DISCORD_RPC_FAILURE_NONE;
}

void discord_rpc_destroy(discord_rpc_t *rpc, uint64_t now_ms) {
    if (rpc == NULL) {
        return;
    }
    if (rpc->connected) {
        if (rpc->desired_set) {
            discord_rpc_clear_activity(rpc);
        }
        size_t previous_pending = 0U;
        for (size_t i = 0; i < rpc->output_count; i++) {
            size_t index = (rpc->output_head + i) % DISCORD_RPC_OUTPUT_SLOTS;
            previous_pending += rpc->output[index].size - rpc->output[index].position;
        }
        for (unsigned int i = 0;
             i < 32U && rpc->connected && (rpc->desired_dirty || rpc->output_count != 0U);
             i++) {
            discord_rpc_pump(rpc, now_ms);
            size_t pending = 0U;
            for (size_t j = 0; j < rpc->output_count; j++) {
                size_t index = (rpc->output_head + j) % DISCORD_RPC_OUTPUT_SLOTS;
                pending += rpc->output[index].size - rpc->output[index].position;
            }
            if (pending == previous_pending) {
                break;
            }
            previous_pending = pending;
        }
        rpc->io.close(rpc->io_context);
    }
    free(rpc);
}
