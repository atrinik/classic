#include <toolkit/packet.h>
#include <toolkit/math.h>

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

    rndm_seed(UINT64_C(196));
    uint64_t first = rndm_u64();
    uint64_t second = rndm_u64();
    rndm_seed(UINT64_C(196));
    if (rndm_u64() != first || rndm_u64() != second) {
        toolkit_deinit();
        return 2;
    }

    toolkit_deinit();
    return 0;
}
