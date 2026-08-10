#include <global.h>
#include <discord_rpc.h>
#include <rich_presence.h>
#include <wrapper.h>

static discord_rpc_t *presence_rpc;
static rich_presence_controller_t presence_controller;
static bool presence_initialized;
static char presence_application_id[21];

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

static bool read_application_id(char output[21]) {
    const char *environment = getenv("ATRINIK_DISCORD_APPLICATION_ID");
    if (rich_presence_application_id_valid(environment)) {
        snprintf(output, 21, "%s", environment);
        return true;
    }

    FILE *file = client_fopen_wrapper("data/discord-application-id", "r");
    if (file == NULL) {
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
    if (!presence_initialized || presence_application_id[0] == '\0') {
        return;
    }

    uint64_t now_ms = SDL_GetTicks();
    int privacy = (int)setting_get_int(OPT_CAT_CLIENT, OPT_DISCORD_PRESENCE);
    if (privacy != RICH_PRESENCE_OFF && presence_rpc == NULL) {
        presence_rpc = discord_rpc_create(presence_application_id);
        if (presence_rpc == NULL) {
            return;
        }
    }
    if (presence_rpc == NULL) {
        return;
    }
    discord_rpc_pump(presence_rpc, now_ms);

    char server[DISCORD_RPC_TEXT_BYTES];
    normalized_markup(selected_server != NULL ? selected_server->name : NULL, server);
    char zone[DISCORD_RPC_TEXT_BYTES];
    const char *zone_source =
        MapData.name_new[0] != '\0'
            ? MapData.name_new
            : (MapData.name[0] != '\0' ? MapData.name : MapData.region_longname);
    normalized_markup(zone_source, zone);

    time_t wall_time = time(NULL);
    rich_presence_input_t input = {
        .playing = cpl.state == ST_PLAY,
        .privacy = privacy,
        .public_server = selected_server != NULL && selected_server->is_meta,
        .server = server,
        .zone = zone,
        .now_ms = now_ms,
        .now_unix = wall_time > 0 ? (uint64_t)wall_time : 1U,
    };
    rich_presence_backend_t operations = backend();
    rich_presence_controller_tick(&presence_controller, &input, &operations);
    discord_rpc_pump(presence_rpc, now_ms);
    if (privacy == RICH_PRESENCE_OFF) {
        discord_rpc_destroy(presence_rpc, now_ms);
        presence_rpc = NULL;
    }
}

void rich_presence_deinit(void) {
    if (!presence_initialized) {
        return;
    }
    if (presence_rpc != NULL) {
        rich_presence_backend_t operations = backend();
        rich_presence_controller_stop(&presence_controller, &operations);
        discord_rpc_destroy(presence_rpc, SDL_GetTicks());
        presence_rpc = NULL;
    }
    memset(presence_application_id, 0, sizeof(presence_application_id));
    presence_initialized = false;
}
