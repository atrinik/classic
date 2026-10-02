/* Copyright 2026 The Atrinik Project */
#include <region_exploration.h>
#include <stdlib.h>
#include <string.h>

typedef struct exploration_record {
    char path[256];
    unsigned width, height;
    uint8_t *bits;
} exploration_record_t;
static exploration_record_t records[4096];
static size_t records_num;

void region_exploration_clear(void) {
    for (size_t i = 0; i < records_num; i++) {
        free(records[i].bits);
    }
    memset(records, 0, sizeof(records));
    records_num = 0;
}

const uint8_t *region_exploration_find(const char *path, unsigned *width, unsigned *height) {
    for (size_t i = 0; i < records_num; i++) {
        if (strcmp(records[i].path, path) == 0) {
            *width = records[i].width;
            *height = records[i].height;
            return records[i].bits;
        }
    }
    return NULL;
}

bool region_exploration_receive(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) {
        return false;
    }
    if (data[0] == 0) {
        if (len != 1) {
            return false;
        }
        region_exploration_clear();
        return true;
    }
    if (data[0] != 1 || len < 8) {
        return false;
    }
    const uint8_t *end = memchr(data + 1, 0, len - 1 < 256 ? len - 1 : 256);
    if (end == NULL || end == data + 1 || data[1] != '/') {
        return false;
    }
    const uint8_t *segment = data + 2;
    for (const uint8_t *p = segment; p <= end; p++) {
        if (p != end && (*p < 32 || *p == 127 || *p == '\\' || *p == ':')) {
            return false;
        }
        if (p == end || *p == '/') {
            size_t n = (size_t)(p - segment);
            if (n == 0 || (n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            segment = p + 1;
        }
    }
    size_t offset = (size_t)(end - data) + 1;
    if (len - offset < 4) {
        return false;
    }
    unsigned width = ((unsigned)data[offset] << 8) | data[offset + 1];
    unsigned height = ((unsigned)data[offset + 2] << 8) | data[offset + 3];
    if (width == 0 || height == 0 || width > 256 || height > 256) {
        return false;
    }
    size_t bytes = ((size_t)width * height + 7) / 8;
    if (len - offset - 4 != bytes) {
        return false;
    }
    unsigned tail = (width * height) % 8;
    if (tail != 0 && (data[len - 1] & (uint8_t)(0xffU << tail)) != 0) {
        return false;
    }
    size_t index = 0;
    while (index < records_num && strcmp(records[index].path, (const char *)data + 1) != 0) {
        index++;
    }
    if (index == sizeof(records) / sizeof(records[0])) {
        return false;
    }
    uint8_t *bits = malloc(bytes);
    if (bits == NULL) {
        return false;
    }
    memcpy(bits, data + offset + 4, bytes);
    free(records[index].bits);
    memcpy(records[index].path, data + 1, offset - 1);
    records[index].width = width;
    records[index].height = height;
    records[index].bits = bits;
    if (index == records_num) {
        records_num++;
    }
    return true;
}
