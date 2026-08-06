#include <toolkit/packet.h>

#include <atrinik/protocol/game_commands.h>

int main(void) {
    toolkit_import(packet);
    packet_struct *packet = packet_new(SERVER_CMD_KEEPALIVE, 0, 0);
    if (packet == NULL || packet->type != SERVER_CMD_KEEPALIVE || packet->size != 0) {
        packet_free(packet);
        toolkit_deinit();
        return 1;
    }

    packet_free(packet);
    toolkit_deinit();
    return 0;
}
