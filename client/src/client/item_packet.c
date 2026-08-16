#include <global.h>
#include <item_packet.h>

#define ITEM_UPDATE_FLAGS                                                                     \
    (UPD_LOCATION | UPD_FLAGS | UPD_WEIGHT | UPD_FACE | UPD_NAME | UPD_ANIM | UPD_ANIMSPEED | \
     UPD_NROF | UPD_DIRECTION | UPD_TYPE | UPD_EXTRA | UPD_GLOW)

bool item_packet_parse_update(packet_reader_t *reader,
                              uint32_t flags,
                              const object *base,
                              item_packet_update_t *update) {
    HARD_ASSERT(reader != NULL);
    HARD_ASSERT(base != NULL);
    HARD_ASSERT(update != NULL);

    *update = (item_packet_update_t){.item = *base};
    if ((flags & ~ITEM_UPDATE_FLAGS) != 0) {
        packet_reader_set_error(reader, PACKET_ERROR_UNSUPPORTED);
        return false;
    }

    if (flags & UPD_LOCATION) {
        (void)packet_reader_read_uint32(reader);
    }
    if (flags & UPD_FLAGS) {
        update->item.flags = packet_reader_read_uint32(reader);
    }
    if (flags & UPD_WEIGHT) {
        update->item.weight = packet_reader_read_uint32(reader) / 1000.0;
    }
    if (flags & UPD_FACE) {
        update->item.face = packet_reader_read_uint16(reader);
    }
    if (flags & UPD_DIRECTION) {
        update->item.direction = packet_reader_read_uint8(reader);
    }
    if (flags & UPD_TYPE) {
        update->item.itype = packet_reader_read_uint8(reader);
        update->item.stype = packet_reader_read_uint8(reader);
        update->item.item_qua = packet_reader_read_uint8(reader);
        if (update->item.item_qua != 255) {
            update->item.item_con = packet_reader_read_uint8(reader);
            update->item.item_level = packet_reader_read_uint8(reader);
            update->item.item_skill_tag = packet_reader_read_uint32(reader);
        }
    }
    if (flags & UPD_NAME) {
        packet_reader_read_string(reader, VS(update->item.s_name));
    }
    if (flags & UPD_ANIM) {
        update->item.animation_id = packet_reader_read_uint16(reader);
    }
    if (flags & UPD_ANIMSPEED) {
        update->item.anim_speed = packet_reader_read_uint8(reader);
    }
    if (flags & UPD_NROF) {
        update->item.nrof = packet_reader_read_uint32(reader);
        if (update->item.nrof == 0) {
            update->item.nrof = 1;
        }
    }
    if (flags & UPD_EXTRA) {
        if (update->item.itype == TYPE_SPELL) {
            update->extra_type = ITEM_PACKET_EXTRA_SPELL;
            update->spell_cost = packet_reader_read_uint16(reader);
            update->spell_path = packet_reader_read_uint32(reader);
            update->spell_flags = packet_reader_read_uint32(reader);
            packet_reader_read_string(
                reader, update->extra_message, ATRINIK_PROTOCOL_ITEM_EXTRA_MESSAGE_SIZE);
        } else if (update->item.itype == TYPE_SKILL) {
            update->extra_type = ITEM_PACKET_EXTRA_SKILL;
            update->skill_level = packet_reader_read_uint8(reader);
            update->skill_exp = packet_reader_read_int64(reader);
            packet_reader_read_string(
                reader, update->extra_message, ATRINIK_PROTOCOL_ITEM_EXTRA_MESSAGE_SIZE);
        }
    }
    if (flags & UPD_GLOW) {
        packet_reader_read_string(reader, VS(update->item.glow));
        update->item.glow_speed = packet_reader_read_uint8(reader);
    }

    return packet_reader_error(reader) == PACKET_ERROR_NONE;
}

void item_packet_apply_update(const item_packet_update_t *update, object *target) {
    HARD_ASSERT(update != NULL);
    HARD_ASSERT(target != NULL);

    object *next = target->next;
    object *prev = target->prev;
    object *env = target->env;
    object *inv = target->inv;
    *target = update->item;
    target->next = next;
    target->prev = prev;
    target->env = env;
    target->inv = inv;
}

bool item_packet_validate_command(const uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);

    uint8_t delete_env = packet_reader_read_uint8(&reader);
    if (delete_env > 1) {
        packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
        return false;
    }
    if (delete_env != 0) {
        (void)packet_reader_read_uint32(&reader);
        if (packet_reader_error(&reader) != PACKET_ERROR_NONE || reader.pos == len) {
            return packet_reader_finish(&reader);
        }
    }

    uint32_t location = packet_reader_read_uint32(&reader);
    uint8_t bflag = packet_reader_read_uint8(&reader);
    if (bflag > 1) {
        packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
        return false;
    }

    uint32_t flags = UPD_FLAGS | UPD_WEIGHT | UPD_FACE | UPD_DIRECTION | UPD_NAME | UPD_ANIM |
                     UPD_ANIMSPEED | UPD_NROF | UPD_GLOW;
    if (location > 0) {
        flags |= UPD_TYPE | UPD_EXTRA;
    }

    while (packet_reader_error(&reader) == PACKET_ERROR_NONE && reader.pos < reader.len) {
        size_t iteration_start = reader.pos;
        object base = {0};
        base.tag = packet_reader_read_uint32(&reader);
        if (base.tag == 0) {
            base.apply_action = packet_reader_read_uint8(&reader);
            if (base.apply_action < CMD_APPLY_ACTION_NONE ||
                base.apply_action > CMD_APPLY_ACTION_BELOW_PREV) {
                packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
            }
        }

        item_packet_update_t update;
        item_packet_parse_update(&reader, flags, &base, &update);
        if (reader.pos == iteration_start && packet_reader_error(&reader) == PACKET_ERROR_NONE) {
            packet_reader_set_error(&reader, PACKET_ERROR_INVALID_ENCODING);
        }
    }

    return packet_reader_finish(&reader);
}
