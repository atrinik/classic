/**
 * @file
 * Transactional classic static-directory protocol 4 parser.
 */

#include "metaserver_directory.h"

#include <toolkit/memory.h>
#include <toolkit/metaserver_url.h>
#include <toolkit/string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#define XML_NAME(node, value) xmlStrEqual((node)->name, (const xmlChar *)(value))

static bool
directory_decimal(const char *value, uint64_t minimum, uint64_t maximum, uint64_t *result) {
    return value != NULL && *value != '\0' && !(value[0] == '0' && value[1] != '\0') &&
           string_parse_uint64(value, 10, minimum, maximum, result);
}

static bool directory_text_valid(const char *value, size_t minimum, size_t maximum) {
    size_t size = strlen(value);
    if (size < minimum || size > maximum) {
        return false;
    }
    for (const unsigned char *cp = (const unsigned char *)value; *cp != '\0'; cp++) {
        if (*cp < 0x20U || *cp == 0x7fU) {
            return false;
        }
    }
    return true;
}

static bool directory_identity_valid(const char *value) {
    if (strlen(value) != 64) {
        return false;
    }
    for (const char *cp = value; *cp != '\0'; cp++) {
        if (!(*cp >= '0' && *cp <= '9') && !(*cp >= 'a' && *cp <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool
directory_node_exact(xmlNodePtr node, const char *name, bool allow_empty, char **value) {
    if (node == NULL || node->type != XML_ELEMENT_NODE || node->ns != NULL ||
        node->properties != NULL || !XML_NAME(node, name)) {
        return false;
    }
    if (node->children != NULL &&
        (node->children != node->last || node->children->type != XML_TEXT_NODE)) {
        return false;
    }
    xmlChar *content = xmlNodeGetContent(node);
    if (content == NULL || (!allow_empty && *content == '\0')) {
        xmlFree(content);
        return false;
    }
    *value = (char *)content;
    return true;
}

static bool directory_copy(char *destination,
                           size_t destination_size,
                           const char *value,
                           size_t minimum,
                           size_t maximum) {
    if (!directory_text_valid(value, minimum, maximum) || strlen(value) >= destination_size) {
        return false;
    }
    snprintf(destination, destination_size, "%s", value);
    return true;
}

static bool directory_parse_server(xmlNodePtr node, metaserver_directory_entry_t *entry) {
    if (node == NULL || node->type != XML_ELEMENT_NODE || node->ns != NULL ||
        node->properties != NULL || !XML_NAME(node, "Server")) {
        return false;
    }

    static const char *const initial_fields[] = {
        "Id",
        "Name",
        "PlayersCount",
        "Version",
        "TextComment",
    };
    xmlNodePtr field = node->children;
    char *values[arraysize(initial_fields)] = {0};
    bool ok = true;
    for (size_t i = 0; i < arraysize(initial_fields); i++) {
        ok = ok && directory_node_exact(field, initial_fields[i], i == 4, &values[i]);
        field = field != NULL ? field->next : NULL;
    }
    if (!ok) {
        goto out;
    }

    uint64_t players = 0;
    ok = directory_identity_valid(values[0]) &&
         directory_copy(entry->server_id, sizeof(entry->server_id), values[0], 64, 64) &&
         directory_copy(entry->name, sizeof(entry->name), values[1], 1, 80) &&
         directory_decimal(values[2], 0, UINT32_MAX, &players) &&
         directory_copy(entry->version, sizeof(entry->version), values[3], 1, 32) &&
         directory_copy(entry->text_comment, sizeof(entry->text_comment), values[4], 0, 256);
    entry->players_count = (uint32_t)players;
    if (!ok) {
        goto out;
    }

    if (field != NULL && XML_NAME(field, "Address")) {
        char *hostname = NULL;
        char *port = NULL;
        ok = directory_node_exact(field, "Address", false, &hostname);
        field = field->next;
        ok = ok && directory_node_exact(field, "Port", false, &port);
        field = field != NULL ? field->next : NULL;
        uint64_t port_number = 0;
        ok = ok && strlen(hostname) < sizeof(entry->hostname) &&
             metaserver_hostname_valid(hostname) &&
             directory_decimal(port, 1, UINT16_MAX, &port_number);
        if (ok) {
            snprintf(VS(entry->hostname), "%s", hostname);
            entry->port = (uint16_t)port_number;
            entry->has_endpoint = true;
        }
        xmlFree(hostname);
        xmlFree(port);
        if (!ok) {
            goto out;
        }
    }

    char *certificate = NULL;
    char *password_required = NULL;
    ok = directory_node_exact(field, "CertificateSha256", false, &certificate);
    field = field != NULL ? field->next : NULL;
    ok = ok && directory_node_exact(field, "PasswordRequired", false, &password_required);
    field = field != NULL ? field->next : NULL;
    ok = ok && field == NULL && directory_identity_valid(certificate) &&
         strcmp(certificate, entry->server_id) == 0 &&
         (strcmp(password_required, "true") == 0 || strcmp(password_required, "false") == 0);
    if (ok) {
        entry->password_required = strcmp(password_required, "true") == 0;
    }
    xmlFree(certificate);
    xmlFree(password_required);

out:
    for (size_t i = 0; i < arraysize(values); i++) {
        xmlFree(values[i]);
    }
    return ok;
}

static bool
directory_attribute(xmlNodePtr root, const char *name, xmlAttrPtr *attribute, char **value) {
    if (*attribute == NULL || (*attribute)->ns != NULL ||
        !xmlStrEqual((*attribute)->name, (const xmlChar *)name) || (*attribute)->children == NULL ||
        (*attribute)->children != (*attribute)->last ||
        (*attribute)->children->type != XML_TEXT_NODE) {
        return false;
    }
    xmlChar *content = xmlNodeListGetString(root->doc, (*attribute)->children, 1);
    if (content == NULL) {
        return false;
    }
    *value = (char *)content;
    *attribute = (*attribute)->next;
    return true;
}

static bool directory_parse_root(xmlNodePtr root, metaserver_directory_snapshot_t *snapshot) {
    if (root == NULL || root->type != XML_ELEMENT_NODE || root->ns != NULL ||
        !XML_NAME(root, "Servers")) {
        return false;
    }
    static const char *const names[] = {
        "protocol",
        "schema",
        "generation",
        "generated-at",
        "expires-at",
    };
    char *values[arraysize(names)] = {0};
    xmlAttrPtr attribute = root->properties;
    bool ok = true;
    for (size_t i = 0; i < arraysize(names); i++) {
        ok = ok && directory_attribute(root, names[i], &attribute, &values[i]);
    }
    uint64_t generation = 0;
    uint64_t generated_at = 0;
    uint64_t expires_at = 0;
    ok = ok && attribute == NULL && strcmp(values[0], "4") == 0 &&
         strcmp(values[1], "atrinik-classic-directory-v4") == 0 &&
         directory_decimal(values[2], 1, UINT64_MAX, &generation) &&
         directory_decimal(values[3], 0, UINT64_MAX, &generated_at) &&
         directory_decimal(values[4], 1, UINT64_MAX, &expires_at) && expires_at > generated_at &&
         expires_at - generated_at <= 14400U;
    if (ok) {
        snapshot->generation = generation;
        snapshot->generated_at = generated_at;
        snapshot->expires_at = expires_at;
    }
    for (size_t i = 0; i < arraysize(values); i++) {
        xmlFree(values[i]);
    }
    return ok;
}

bool metaserver_directory_parse(const char *body,
                                size_t body_size,
                                metaserver_directory_snapshot_t **snapshot) {
    if (snapshot == NULL) {
        return false;
    }
    *snapshot = NULL;
    if (body == NULL || body_size == 0 || body_size > METASERVER_DIRECTORY_BODY_MAX ||
        body_size > INT_MAX || memchr(body, '\0', body_size) != NULL) {
        return false;
    }

    xmlDocPtr doc = xmlReadMemory(body,
                                  (int)body_size,
                                  "classic-directory-v4.xml",
                                  NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
                                      XML_PARSE_NOWARNING);
    if (doc == NULL || doc->intSubset != NULL || doc->extSubset != NULL) {
        xmlFreeDoc(doc);
        return false;
    }
    metaserver_directory_snapshot_t *parsed = xcalloc(1, sizeof(*parsed));
    xmlNodePtr root = xmlDocGetRootElement(doc);
    bool ok = doc->children == root && doc->last == root && directory_parse_root(root, parsed);
    for (xmlNodePtr node = ok ? root->children : NULL; node != NULL; node = node->next) {
        if (parsed->servers_count == METASERVER_DIRECTORY_SERVERS_MAX ||
            !directory_parse_server(node, &parsed->servers[parsed->servers_count]) ||
            (parsed->servers_count > 0 &&
             strcmp(parsed->servers[parsed->servers_count - 1].server_id,
                    parsed->servers[parsed->servers_count].server_id) >= 0)) {
            ok = false;
            break;
        }
        parsed->servers_count++;
    }
    xmlFreeDoc(doc);
    if (!ok) {
        free(parsed);
        return false;
    }
    *snapshot = parsed;
    return true;
}

bool metaserver_directory_current(const metaserver_directory_snapshot_t *snapshot, uint64_t now) {
    return snapshot != NULL && now < snapshot->expires_at &&
           (snapshot->generated_at <= now ||
            snapshot->generated_at - now <= METASERVER_DIRECTORY_CLOCK_SKEW_SECONDS);
}

bool metaserver_directory_replacement_valid(const metaserver_directory_snapshot_t *candidate,
                                            const char *candidate_body,
                                            size_t candidate_body_size,
                                            const metaserver_directory_snapshot_t *cached,
                                            const char *cached_body,
                                            size_t cached_body_size) {
    if (candidate == NULL || candidate_body == NULL) {
        return false;
    }
    if (cached == NULL) {
        return true;
    }
    if (candidate->generation != cached->generation) {
        return candidate->generation > cached->generation;
    }
    return cached_body != NULL && candidate_body_size == cached_body_size &&
           memcmp(candidate_body, cached_body, candidate_body_size) == 0;
}

void metaserver_directory_free(metaserver_directory_snapshot_t *snapshot) {
    free(snapshot);
}
