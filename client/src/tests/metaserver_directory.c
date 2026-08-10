#include "../client/metaserver_directory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expression)                    \
    do {                                       \
        if (!(expression)) {                   \
            fprintf(stderr, "%d\n", __LINE__); \
            return 1;                          \
        }                                      \
    } while (0)

static char *read_fixture(const char *path, size_t *size) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL || fseek(fp, 0, SEEK_END) != 0) {
        return NULL;
    }
    long length = ftell(fp);
    if (length <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *body = malloc((size_t)length + 1);
    if (body == NULL || fread(body, 1, (size_t)length, fp) != (size_t)length) {
        free(body);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    body[length] = '\0';
    *size = (size_t)length;
    return body;
}

static char *replace_once(const char *body, const char *from, const char *to, size_t *size) {
    const char *position = strstr(body, from);
    if (position == NULL) {
        return NULL;
    }
    size_t prefix = (size_t)(position - body);
    size_t from_size = strlen(from);
    size_t to_size = strlen(to);
    size_t body_size = strlen(body);
    char *changed = malloc(body_size - from_size + to_size + 1);
    if (changed == NULL) {
        return NULL;
    }
    memcpy(changed, body, prefix);
    memcpy(changed + prefix, to, to_size);
    memcpy(changed + prefix + to_size, position + from_size, body_size - prefix - from_size + 1);
    *size = body_size - from_size + to_size;
    return changed;
}

static int reject_replacement(const char *body, const char *from, const char *to) {
    size_t size;
    char *changed = replace_once(body, from, to, &size);
    if (changed == NULL) {
        return 1;
    }
    metaserver_directory_snapshot_t *snapshot = NULL;
    bool accepted = metaserver_directory_parse(changed, size, &snapshot);
    metaserver_directory_free(snapshot);
    free(changed);
    return accepted;
}

static char *build_directory(size_t servers, size_t *body_size) {
    size_t capacity = 256U + servers * 512U;
    char *body = malloc(capacity);
    if (body == NULL) {
        return NULL;
    }
    size_t used = (size_t)snprintf(
        body,
        capacity,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Servers protocol=\"4\" schema=\"atrinik-classic-directory-v4\" generation=\"1\" "
        "generated-at=\"1000\" expires-at=\"2000\">\n");
    for (size_t i = 0; i < servers && used < capacity; i++) {
        int written =
            snprintf(body + used,
                     capacity - used,
                     "<Server><Id>%064zx</Id><Name>Server</Name><PlayersCount>0</PlayersCount>"
                     "<Version>1</Version><TextComment></TextComment>"
                     "<CertificateSha256>%064zx</CertificateSha256>"
                     "<PasswordRequired>false</PasswordRequired></Server>\n",
                     i,
                     i);
        if (written < 0 || (size_t)written >= capacity - used) {
            free(body);
            return NULL;
        }
        used += (size_t)written;
    }
    int written = snprintf(body + used, capacity - used, "</Servers>\n");
    if (written < 0 || (size_t)written >= capacity - used) {
        free(body);
        return NULL;
    }
    *body_size = used + (size_t)written;
    return body;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    size_t size;
    char *body = read_fixture(argv[1], &size);
    REQUIRE(body != NULL);

    metaserver_directory_snapshot_t *snapshot = NULL;
    REQUIRE(metaserver_directory_parse(body, size, &snapshot));
    REQUIRE(snapshot->generation == 42);
    REQUIRE(snapshot->generated_at == 1786219200);
    REQUIRE(snapshot->expires_at == 1786222800);
    REQUIRE(snapshot->servers_count == 2);
    REQUIRE(snapshot->servers[0].players_count == 2);
    REQUIRE(!snapshot->servers[0].has_endpoint);
    REQUIRE(snapshot->servers[1].players_count == UINT32_MAX);
    REQUIRE(snapshot->servers[1].has_endpoint);
    REQUIRE(strcmp(snapshot->servers[1].hostname, "play.example.net") == 0);
    REQUIRE(snapshot->servers[1].port == 13327);
    REQUIRE(snapshot->servers[1].password_required);
    REQUIRE(metaserver_directory_current(snapshot, 1786222799));
    REQUIRE(!metaserver_directory_current(snapshot, 1786222800));
    REQUIRE(!metaserver_directory_current(snapshot, 1786218899));
    metaserver_directory_snapshot_t maximum_clock = *snapshot;
    maximum_clock.generated_at = UINT64_MAX;
    maximum_clock.expires_at = UINT64_MAX;
    REQUIRE(!metaserver_directory_current(&maximum_clock, UINT64_MAX - 301U));
    REQUIRE(metaserver_directory_current(&maximum_clock, UINT64_MAX - 300U));
    REQUIRE(metaserver_directory_replacement_valid(snapshot, body, size, NULL, NULL, 0));
    metaserver_directory_snapshot_t older = *snapshot;
    older.generation = 41;
    REQUIRE(!metaserver_directory_replacement_valid(&older, body, size, snapshot, body, size));
    metaserver_directory_snapshot_t newer = *snapshot;
    newer.generation = 43;
    REQUIRE(metaserver_directory_replacement_valid(&newer, body, size, snapshot, body, size));
    char *different = strdup(body);
    REQUIRE(different != NULL);
    different[size - 2] = ' ';
    REQUIRE(
        !metaserver_directory_replacement_valid(snapshot, different, size, snapshot, body, size));
    free(different);
    metaserver_directory_free(snapshot);

    REQUIRE(reject_replacement(body, "protocol=\"4\"", "protocol=\"3\"") == 0);
    REQUIRE(reject_replacement(body,
                               "<Servers ",
                               "<!DOCTYPE Servers [<!ENTITY x \"secret\">]><Servers ") == 0);
    REQUIRE(reject_replacement(body, "<Servers ", "<!--comment--><Servers ") == 0);
    REQUIRE(reject_replacement(body, "<Servers ", "<?directory invalid?><Servers ") == 0);
    REQUIRE(reject_replacement(body, "<Server>", "<Server unexpected=\"1\">") == 0);
    REQUIRE(reject_replacement(body, "<Name>Classic", "<Unknown>Classic") == 0);
    REQUIRE(reject_replacement(body, "<PlayersCount>2", "<PlayersCount>02") == 0);
    REQUIRE(reject_replacement(body, "<Address>play.example.net", "<Address>192.0.2.1") == 0);
    REQUIRE(reject_replacement(body, "<Address>play.example.net", "<Address>xn--a.example.org") ==
            0);
    REQUIRE(reject_replacement(body, "<Name>Classic", "<Name>\xef\xbf\xbe") == 0);
    REQUIRE(reject_replacement(body, "<Name>Classic", "<Name>\xef\xbf\xbf") == 0);
    REQUIRE(reject_replacement(body,
                               "<CertificateSha256>ffffffff",
                               "<CertificateSha256>efffffff") == 0);
    REQUIRE(reject_replacement(
                body,
                "<Id>ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
                "<Id>0000000000000000000000000000000000000000000000000000000000000000") == 0);
    REQUIRE(!metaserver_directory_parse(body, METASERVER_DIRECTORY_BODY_MAX + 1U, &snapshot));

    size_t maximum_size;
    char *maximum = build_directory(METASERVER_DIRECTORY_SERVERS_MAX, &maximum_size);
    REQUIRE(maximum != NULL);
    REQUIRE(metaserver_directory_parse(maximum, maximum_size, &snapshot));
    REQUIRE(snapshot->servers_count == METASERVER_DIRECTORY_SERVERS_MAX);
    metaserver_directory_free(snapshot);
    free(maximum);
    char *overflow = build_directory(METASERVER_DIRECTORY_SERVERS_MAX + 1U, &maximum_size);
    REQUIRE(overflow != NULL);
    REQUIRE(!metaserver_directory_parse(overflow, maximum_size, &snapshot));
    free(overflow);

    free(body);
    return 0;
}
