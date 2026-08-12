#include <global.h>
#include <discord_rpc.h>
#include <rich_presence.h>
#include <wrapper.h>

static discord_rpc_t *presence_rpc;
static rich_presence_controller_t presence_controller;
static bool presence_initialized;
static bool presence_zone_fresh;
static char presence_application_id[21];
static discord_rpc_failure_t presence_failure;

static void backend_publish(void *context, const discord_rpc_activity_t *activity) {
    discord_rpc_set_activity(context, activity);
}

static void backend_clear(void *context) {
    discord_rpc_clear_activity(context);
}

static rich_presence_backend_t backend(void) {
    return (rich_presence_backend_t){
        .context = presence_rpc,
        .publish = backend_publish,
        .clear = backend_clear,
    };
}

static void log_transport_failure(void) {
    discord_rpc_failure_t failure = discord_rpc_failure(presence_rpc);
    if (failure == presence_failure) {
        return;
    }
    presence_failure = failure;
    const char *reason = NULL;
    switch (failure) {
        case DISCORD_RPC_FAILURE_CONNECT:
            reason = "Discord desktop IPC is unavailable";
            break;
        case DISCORD_RPC_FAILURE_IO:
            reason = "Discord desktop IPC closed during I/O";
            break;
        case DISCORD_RPC_FAILURE_TIMEOUT:
            reason = "Discord desktop IPC timed out";
            break;
        case DISCORD_RPC_FAILURE_PROTOCOL:
            reason = "Discord desktop IPC returned malformed data";
            break;
        case DISCORD_RPC_FAILURE_REMOTE_CLOSE:
            reason = "Discord desktop IPC requested closure";
            break;
        case DISCORD_RPC_FAILURE_REMOTE_ERROR:
            reason = "Discord desktop IPC returned an error";
            break;
        case DISCORD_RPC_FAILURE_NONE:
            break;
    }
    if (reason != NULL) {
        LOG(INFO, "Discord Rich Presence: %s; retrying in the background", reason);
    }
}

static bool read_application_id(char output[21]) {
    const char *environment = getenv("ATRINIK_DISCORD_APPLICATION_ID");
    bool configured = environment != NULL && environment[0] != '\0';
    if (rich_presence_application_id_valid(environment)) {
        snprintf(output, 21, "%s", environment);
        return true;
    }

    FILE *file = client_fopen_wrapper("data/discord-application-id", "r");
    if (file == NULL) {
        if (configured) {
            LOG(INFO, "Discord Rich Presence: configured Application ID is invalid; disabled");
        }
        return false;
    }
    char line[64];
    bool valid = fgets(line, sizeof(line), file) != NULL;
    if (valid) {
        char *newline = strchr(line, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
        int character = fgetc(file);
        valid = (character == EOF) && rich_presence_application_id_valid(line);
    }
    if (fclose(file) != 0) {
        valid = false;
    }
    if (valid) {
        size_t length = strlen(line);
        memcpy(output, line, length + 1U);
    }
    if (!valid) {
        LOG(INFO, "Discord Rich Presence: configured Application ID file is invalid; disabled");
    }
    return valid;
}

static void normalized_markup(const char *value, char output[DISCORD_RPC_TEXT_BYTES]) {
    if (value == NULL) {
        output[0] = '\0';
        return;
    }
    char *copy = xstrdup(value);
    char *plain = text_strip_markup(copy, NULL, 1);
    if (!rich_presence_normalize(plain, output)) {
        output[0] = '\0';
    }
    free(plain);
}

void rich_presence_init(void) {
    if (presence_initialized) {
        return;
    }
    rich_presence_controller_init(&presence_controller);
    presence_initialized = true;

    read_application_id(presence_application_id);
}

void rich_presence_tick(void) {
    if (!presence_initialized) {
        return;
    }

    uint64_t now_ms = SDL_GetTicks();
    int privacy = (int)setting_get_int(OPT_CAT_CLIENT, OPT_DISCORD_PRESENCE);
    if (presence_application_id[0] != '\0' && privacy != RICH_PRESENCE_OFF &&
        presence_rpc == NULL) {
        presence_rpc = discord_rpc_create(presence_application_id);
        if (presence_rpc == NULL) {
            /* Lifecycle tracking below remains active without a transport. */
        }
    }
    char server[DISCORD_RPC_TEXT_BYTES];
    normalized_markup(selected_server != NULL ? selected_server->name : NULL, server);
    char zone[DISCORD_RPC_TEXT_BYTES];
    const char *zone_source =
        MapData.name_new[0] != '\0'
            ? MapData.name_new
            : (MapData.name[0] != '\0' ? MapData.name : MapData.region_longname);
    normalized_markup(presence_zone_fresh ? zone_source : NULL, zone);
    char character[DISCORD_RPC_TEXT_BYTES];
    normalized_markup(cpl.name, character);

    time_t wall_time = time(NULL);
    rich_presence_input_t input = {
        .playing = cpl.state == ST_PLAY,
        .privacy = privacy,
        .public_server = selected_server != NULL && selected_server->is_meta,
        .server = server,
        .zone = zone,
        .character = character,
        .level = cpl.stats.level,
        .character_available = character[0] != '\0' && cpl.stats.level != 0U,
        .now_ms = now_ms,
        .now_unix = wall_time > 0 ? (uint64_t)wall_time : 1U,
    };
    rich_presence_backend_t operations = backend();
    rich_presence_controller_tick(&presence_controller,
                                  &input,
                                  presence_rpc != NULL ? &operations : NULL);
    if (presence_rpc != NULL) {
        discord_rpc_pump(presence_rpc, now_ms);
        log_transport_failure();
    }
    if (privacy == RICH_PRESENCE_OFF && presence_rpc != NULL) {
        discord_rpc_destroy(presence_rpc, now_ms);
        presence_rpc = NULL;
        presence_failure = DISCORD_RPC_FAILURE_NONE;
    }
}

void rich_presence_session_start(void) {
    presence_zone_fresh = false;
    if (presence_initialized) {
        rich_presence_backend_t operations = backend();
        rich_presence_controller_begin_session(&presence_controller,
                                               presence_rpc != NULL ? &operations : NULL,
                                               SDL_GetTicks());
    }
}

void rich_presence_zone_changed(void) {
    presence_zone_fresh = true;
}

void rich_presence_deinit(void) {
    if (!presence_initialized) {
        return;
    }
    if (presence_rpc != NULL) {
        rich_presence_backend_t operations = backend();
        uint64_t now_ms = SDL_GetTicks();
        rich_presence_controller_stop(&presence_controller, &operations, now_ms);
        discord_rpc_destroy(presence_rpc, now_ms);
        presence_rpc = NULL;
    }
    memset(presence_application_id, 0, sizeof(presence_application_id));
    presence_zone_fresh = false;
    presence_failure = DISCORD_RPC_FAILURE_NONE;
    presence_initialized = false;
}
