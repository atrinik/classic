#include <toolkit/packet.h>

#include <atrinik/pathfinding.h>
#include <atrinik/protocol/game_commands.h>

int main(void) {
    atrinik_pf_options pathfinding_options;
    atrinik_pf_options_init(&pathfinding_options);
    if (pathfinding_options.algorithm != ATRINIK_PF_ASTAR) {
        return 1;
    }

    toolkit_import(packet);
    packet_struct *packet = packet_new(SERVER_CMD_KEEPALIVE, 0, 0);
    if (packet == NULL) {
        toolkit_deinit();
        return 1;
    }

    packet_free(packet);
    toolkit_deinit();
    return 0;
}
