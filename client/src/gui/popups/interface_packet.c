#include <interface_packet.h>
#include <item_packet.h>
#include <client.h>
#include <commands.h>
#include <interface.h>
#include <item.h>
#include <main.h>
#include <toolkit/packet.h>
#include <toolkit/socket.h>
#include <toolkit/toolkit.h>

static void interface_packet_read_string(packet_reader_t *reader, size_t maximum) {
    (void)packet_reader_read_string_view(reader, maximum);
}

bool interface_packet_validate(const uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);
    bool restore_seen = false;

    while (packet_reader_error(&reader) == PACKET_ERROR_NONE && reader.pos < reader.len) {
        size_t iteration_start = reader.pos;
        uint8_t type = packet_reader_read_uint8(&reader);
        switch (type) {
            case CMD_INTERFACE_TEXT:
            case CMD_INTERFACE_APPEND_TEXT:
                interface_packet_read_string(&reader, PACKET_PAYLOAD_MAX);
                break;

            case CMD_INTERFACE_LINK:
            case CMD_INTERFACE_TITLE:
            case CMD_INTERFACE_INPUT:
            case CMD_INTERFACE_INPUT_PREPEND:
            case CMD_INTERFACE_AUTOCOMPLETE:
                interface_packet_read_string(&reader, HUGE_BUF - 1);
                break;

            case CMD_INTERFACE_ICON:
                interface_packet_read_string(&reader, MAX_BUF - 1);
                break;

            case CMD_INTERFACE_ANIM:
                (void)packet_reader_read_uint16(&reader);
                (void)packet_reader_read_uint8(&reader);
                (void)packet_reader_read_uint8(&reader);
                break;

            case CMD_INTERFACE_OBJECT: {
                uint16_t flags = packet_reader_read_uint16(&reader);
                object base = {.tag = packet_reader_read_uint32(&reader)};
                item_packet_update_t update;
                item_packet_parse_update(&reader, flags, &base, &update);
                break;
            }

            case CMD_INTERFACE_ALLOW_TAB:
            case CMD_INTERFACE_INPUT_CLEANUP_DISABLE:
            case CMD_INTERFACE_INPUT_ALLOW_EMPTY:
            case CMD_INTERFACE_SCROLL_BOTTOM:
                break;

            case CMD_INTERFACE_RESTORE:
                if (restore_seen) {
                    packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
                }
                restore_seen = true;
                break;

            default:
                packet_reader_set_error(&reader, PACKET_ERROR_UNSUPPORTED);
                break;
        }

        if (reader.pos == iteration_start && packet_reader_error(&reader) == PACKET_ERROR_NONE) {
            packet_reader_set_error(&reader, PACKET_ERROR_INVALID_ENCODING);
        }
    }

    return packet_reader_finish(&reader);
}
