#include <rich_presence.h>

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define RICH_PRESENCE_UPDATE_INTERVAL_MS 4100U
#define RICH_PRESENCE_RATE_WINDOW_MS 20000U
#define RICH_PRESENCE_RATE_CAPACITY 5U

static bool
utf8_character(const unsigned char *value, size_t remaining, size_t *width, uint32_t *codepoint) {
    if (remaining == 0) {
        return false;
    }
    if (value[0] < 0x80U) {
        *width = 1;
        *codepoint = value[0];
        return true;
    }

    size_t continuation;
    uint32_t decoded;
    if (value[0] >= 0xc2U && value[0] <= 0xdfU) {
        continuation = 1;
        decoded = value[0] & 0x1fU;
    } else if (value[0] >= 0xe0U && value[0] <= 0xefU) {
        continuation = 2;
        decoded = value[0] & 0x0fU;
    } else if (value[0] >= 0xf0U && value[0] <= 0xf4U) {
        continuation = 3;
        decoded = value[0] & 0x07U;
    } else {
        return false;
    }
    if (remaining <= continuation) {
        return false;
    }

    unsigned char leading = value[0];
    for (size_t i = 1; i <= continuation; i++) {
        if ((value[i] & 0xc0U) != 0x80U) {
            return false;
        }
        decoded = (decoded << 6U) | (value[i] & 0x3fU);
    }
    if ((leading == 0xe0U && decoded < 0x800U) || (leading == 0xedU && decoded >= 0xd800U) ||
        (leading == 0xf0U && decoded < 0x10000U) || decoded > 0x10ffffU) {
        return false;
    }
    *width = continuation + 1U;
    *codepoint = decoded;
    return true;
}

bool rich_presence_application_id_valid(const char *value) {
    if (value == NULL) {
        return false;
    }
    size_t length = strlen(value);
    if (length == 0 || length > 20 || value[0] == '0') {
        return false;
    }
    uint64_t parsed = 0;
    for (size_t i = 0; i < length; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(value[i] - '0');
        if (parsed > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    return parsed != 0;
}

bool rich_presence_normalize(const char *value, char output[DISCORD_RPC_TEXT_BYTES]) {
    if (value == NULL) {
        output[0] = '\0';
        return false;
    }

    const unsigned char *input = (const unsigned char *)value;
    size_t input_size = strlen(value);
    size_t input_pos = 0;
    size_t output_pos = 0;
    size_t characters = 0;
    bool whitespace = true;

    while (input_pos < input_size) {
        size_t width;
        uint32_t codepoint;
        if (!utf8_character(input + input_pos, input_size - input_pos, &width, &codepoint)) {
            output[0] = '\0';
            return false;
        }

        if (codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU)) {
            if (codepoint == '\t' || codepoint == '\n' || codepoint == '\r' ||
                codepoint == 0x85U) {
                whitespace = output_pos != 0;
            }
            input_pos += width;
            continue;
        }
        if (codepoint == ' ' || codepoint == 0x00a0U || codepoint == 0x1680U ||
            (codepoint >= 0x2000U && codepoint <= 0x200aU) || codepoint == 0x2028U ||
            codepoint == 0x2029U || codepoint == 0x202fU || codepoint == 0x205fU ||
            codepoint == 0x3000U) {
            whitespace = output_pos != 0;
            input_pos += width;
            continue;
        }
        if (whitespace && output_pos != 0) {
            if (characters == 128U || output_pos + 1U >= DISCORD_RPC_TEXT_BYTES) {
                break;
            }
            output[output_pos++] = ' ';
            characters++;
        }
        whitespace = false;
        if (characters == 128U || output_pos + width >= DISCORD_RPC_TEXT_BYTES) {
            break;
        }
        memcpy(output + output_pos, input + input_pos, width);
        output_pos += width;
        characters++;
        input_pos += width;
    }
    output[output_pos] = '\0';
    return characters >= 2U;
}

static void normalized_with_prefix(char output[DISCORD_RPC_TEXT_BYTES],
                                   const char *prefix,
                                   const char *value,
                                   const char *fallback) {
    char normalized[DISCORD_RPC_TEXT_BYTES];
    if (!rich_presence_normalize(value, normalized)) {
        snprintf(normalized, sizeof(normalized), "%s", fallback);
    }

    char combined[DISCORD_RPC_TEXT_BYTES * 2U];
    snprintf(combined, sizeof(combined), "%s%s", prefix, normalized);
    if (!rich_presence_normalize(combined, output)) {
        snprintf(output, DISCORD_RPC_TEXT_BYTES, "%s%s", prefix, fallback);
    }
}

static size_t copy_utf8_prefix(char *output,
                               size_t output_size,
                               const char *value,
                               size_t max_characters) {
    size_t input_size = strlen(value);
    size_t input_pos = 0;
    size_t output_pos = 0;
    size_t characters = 0;
    while (input_pos < input_size && characters < max_characters) {
        size_t width;
        uint32_t codepoint;
        if (!utf8_character((const unsigned char *)value + input_pos,
                            input_size - input_pos,
                            &width,
                            &codepoint) ||
            output_pos + width >= output_size) {
            break;
        }
        (void)codepoint;
        memcpy(output + output_pos, value + input_pos, width);
        output_pos += width;
        input_pos += width;
        characters++;
    }
    output[output_pos] = '\0';
    return output_pos;
}

static void character_details(char output[DISCORD_RPC_TEXT_BYTES],
                              const char *character,
                              uint32_t level) {
    char normalized[DISCORD_RPC_TEXT_BYTES];
    if (!rich_presence_normalize(character, normalized)) {
        output[0] = '\0';
        return;
    }
    char suffix[32];
    int suffix_size = snprintf(suffix, sizeof(suffix), " - Level %" PRIu32, level);
    if (suffix_size < 0 || (size_t)suffix_size >= sizeof(suffix)) {
        output[0] = '\0';
        return;
    }
    size_t name_limit = DISCORD_RPC_TEXT_BYTES - 1U - (size_t)suffix_size;
    char bounded[DISCORD_RPC_TEXT_BYTES];
    copy_utf8_prefix(bounded, sizeof(bounded), normalized, 96U);
    if (strlen(bounded) > name_limit) {
        bounded[name_limit] = '\0';
        while (name_limit != 0U &&
               ((unsigned char)bounded[name_limit] & 0xc0U) == 0x80U) {
            bounded[--name_limit] = '\0';
        }
    }
    if (bounded[0] == '\0') {
        output[0] = '\0';
        return;
    }
    size_t bounded_size = strlen(bounded);
    memcpy(output, bounded, bounded_size);
    memcpy(output + bounded_size, suffix, (size_t)suffix_size + 1U);
}

static void server_and_zone(char output[DISCORD_RPC_TEXT_BYTES],
                            const rich_presence_input_t *input) {
    char server[DISCORD_RPC_TEXT_BYTES];
    char zone[DISCORD_RPC_TEXT_BYTES];
    if (!rich_presence_normalize(input->public_server ? input->server : "Private server",
                                 server)) {
        snprintf(server, sizeof(server), "%s", input->public_server ? "Atrinik server"
                                                                        : "Private server");
    }
    if (!rich_presence_normalize(input->zone, zone)) {
        snprintf(zone, sizeof(zone), "Unknown zone");
    }
    char bounded_server[DISCORD_RPC_TEXT_BYTES];
    char bounded_zone[DISCORD_RPC_TEXT_BYTES];
    copy_utf8_prefix(bounded_server, sizeof(bounded_server), server, 56U);
    copy_utf8_prefix(bounded_zone, sizeof(bounded_zone), zone, 56U);
    char combined[DISCORD_RPC_TEXT_BYTES * 2U];
    snprintf(combined, sizeof(combined), "On %.*s / %.*s", 384, bounded_server, 384,
             bounded_zone);
    rich_presence_normalize(combined, output);
}

static void build_activity(const rich_presence_controller_t *controller,
                           const rich_presence_input_t *input,
                           discord_rpc_activity_t *activity) {
    memset(activity, 0, sizeof(*activity));
    activity->started_at = controller->session_started_at;
    snprintf(activity->details, sizeof(activity->details), "Playing Atrinik Classic");

    if (input->privacy >= RICH_PRESENCE_SERVER &&
        input->privacy < RICH_PRESENCE_SERVER_ZONE_CHARACTER) {
        normalized_with_prefix(activity->state,
                               "On ",
                               input->public_server ? input->server : "Private server",
                               input->public_server ? "Atrinik server" : "Private server");
    }
    if (input->privacy >= RICH_PRESENCE_SERVER_ZONE) {
        if (input->privacy >= RICH_PRESENCE_SERVER_ZONE_CHARACTER) {
            server_and_zone(activity->state, input);
        }
        if (input->privacy >= RICH_PRESENCE_SERVER_ZONE_CHARACTER &&
            input->character_available) {
            character_details(activity->details, input->character, input->level);
        } else {
            char zone[DISCORD_RPC_TEXT_BYTES];
            if (rich_presence_normalize(input->zone, zone)) {
                normalized_with_prefix(activity->details, "Exploring ", zone, "Atrinik");
            } else {
                snprintf(activity->details, sizeof(activity->details), "Exploring");
            }
        }
    }
}

static bool activity_equal(const discord_rpc_activity_t *left,
                           const discord_rpc_activity_t *right) {
    return left->started_at == right->started_at && strcmp(left->details, right->details) == 0 &&
           strcmp(left->state, right->state) == 0;
}

static void purge_commands(rich_presence_controller_t *controller, uint64_t now_ms) {
    while (controller->command_count != 0U &&
           now_ms - controller->command_times[controller->command_start] >=
               RICH_PRESENCE_RATE_WINDOW_MS) {
        controller->command_start = (controller->command_start + 1U) % RICH_PRESENCE_RATE_CAPACITY;
        controller->command_count--;
    }
}

static void record_command(rich_presence_controller_t *controller, uint64_t now_ms) {
    purge_commands(controller, now_ms);
    if (controller->command_count == RICH_PRESENCE_RATE_CAPACITY) {
        controller->command_start = (controller->command_start + 1U) % RICH_PRESENCE_RATE_CAPACITY;
        controller->command_count--;
    }
    size_t position =
        (controller->command_start + controller->command_count) % RICH_PRESENCE_RATE_CAPACITY;
    controller->command_times[position] = now_ms;
    controller->command_count++;
}

static bool can_publish(rich_presence_controller_t *controller, uint64_t now_ms) {
    purge_commands(controller, now_ms);
    /* Keep one slot available for an immediate privacy clear. */
    return controller->command_count < RICH_PRESENCE_RATE_CAPACITY - 1U;
}

void rich_presence_controller_init(rich_presence_controller_t *controller) {
    memset(controller, 0, sizeof(*controller));
    controller->last_privacy = RICH_PRESENCE_OFF;
}

void rich_presence_controller_tick(rich_presence_controller_t *controller,
                                   const rich_presence_input_t *input,
                                   const rich_presence_backend_t *backend) {
    int privacy = input->privacy;
    if (privacy < RICH_PRESENCE_OFF || privacy > RICH_PRESENCE_SERVER_ZONE_CHARACTER) {
        privacy = RICH_PRESENCE_GAME;
    }

    if (input->playing && !controller->was_playing) {
        controller->session_started_at = input->now_unix;
        controller->published.started_at = 0;
    }
    controller->was_playing = input->playing;

    bool active = input->playing && privacy != RICH_PRESENCE_OFF && backend != NULL;
    bool downgrade = active && privacy < controller->last_privacy;
    if ((!active || downgrade) && controller->owns_activity) {
        backend->clear(backend->context);
        record_command(controller, input->now_ms);
        controller->owns_activity = false;
        memset(&controller->published, 0, sizeof(controller->published));
        controller->next_publish_ms =
            downgrade ? input->now_ms + RICH_PRESENCE_UPDATE_INTERVAL_MS : input->now_ms;
    }

    controller->last_privacy = privacy;
    if (!active) {
        controller->pending = false;
        if (!input->playing) {
            controller->session_started_at = 0;
        }
        return;
    }

    rich_presence_input_t normalized_input = *input;
    normalized_input.privacy = privacy;
    discord_rpc_activity_t activity;
    build_activity(controller, &normalized_input, &activity);
    if (!controller->owns_activity || !activity_equal(&controller->published, &activity)) {
        controller->pending_activity = activity;
        controller->pending = true;
    } else {
        controller->pending = false;
    }

    if (controller->pending && input->now_ms >= controller->next_publish_ms &&
        can_publish(controller, input->now_ms)) {
        backend->publish(backend->context, &controller->pending_activity);
        record_command(controller, input->now_ms);
        controller->published = controller->pending_activity;
        controller->pending = false;
        controller->owns_activity = true;
        controller->next_publish_ms = input->now_ms + RICH_PRESENCE_UPDATE_INTERVAL_MS;
    }
}

void rich_presence_controller_begin_session(rich_presence_controller_t *controller,
                                            const rich_presence_backend_t *backend,
                                            uint64_t now_ms) {
    if (controller->owns_activity && backend != NULL) {
        backend->clear(backend->context);
        record_command(controller, now_ms);
        controller->next_publish_ms = now_ms + RICH_PRESENCE_UPDATE_INTERVAL_MS;
    }
    controller->was_playing = false;
    controller->owns_activity = false;
    controller->pending = false;
    controller->session_started_at = 0;
    memset(&controller->published, 0, sizeof(controller->published));
}

void rich_presence_controller_stop(rich_presence_controller_t *controller,
                                   const rich_presence_backend_t *backend,
                                   uint64_t now_ms) {
    if (controller->owns_activity) {
        backend->clear(backend->context);
        record_command(controller, now_ms);
    }
    rich_presence_controller_init(controller);
}
