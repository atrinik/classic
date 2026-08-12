#ifndef ITEM_PACKET_H
#define ITEM_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL_surface.h>
#include <item.h>
#include <toolkit/packet.h>

typedef enum item_packet_extra_type {
    ITEM_PACKET_EXTRA_NONE,
    ITEM_PACKET_EXTRA_SPELL,
    ITEM_PACKET_EXTRA_SKILL,
} item_packet_extra_type_t;

typedef struct item_packet_update {
    object item;
    item_packet_extra_type_t extra_type;
    uint16_t spell_cost;
    uint32_t spell_path;
    uint32_t spell_flags;
    uint8_t skill_level;
    int64_t skill_exp;
    char extra_message[HUGE_BUF];
} item_packet_update_t;

bool item_packet_parse_update(packet_reader_t *reader,
                              uint32_t flags,
                              const object *base,
                              item_packet_update_t *update);

void item_packet_apply_update(const item_packet_update_t *update, object *target);

bool item_packet_validate_command(const uint8_t *data, size_t len, size_t pos);

#endif
