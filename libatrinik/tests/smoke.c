#include <toolkit/packet.h>
#include <toolkit/math.h>
#include <toolkit/mempool.h>

#include <atrinik/protocol/game_commands.h>

static int test_mempool_reclamation(void) {
    mempool_struct *pool = mempool_create("reclamation test",
                                         2,
                                         sizeof(uint64_t),
                                         MEMPOOL_ALLOW_FREEING,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL);
    uint64_t epoch = mempool_reclamation_epoch(pool);
    if (epoch != 0 || mempool_reclaim(pool) != 0 ||
        !mempool_reclamation_is_current(pool, epoch)) {
        return 3;
    }

    void *first = mempool_get(pool);
    void *second = mempool_get(pool);
    void *third = mempool_get(pool);
    mempool_return(pool, first);
    /* Each puddle still contains a live chunk, so nothing can be reclaimed. */
    if (mempool_reclaim(pool) != 0 || !mempool_reclamation_is_current(pool, epoch)) {
        return 4;
    }

    mempool_return(pool, second);
    if (!mempool_reclamation_is_current(pool, epoch) || mempool_reclaim(pool) != 1 ||
        mempool_reclamation_epoch(pool) != epoch + 1 ||
        mempool_reclamation_is_current(pool, epoch)) {
        return 5;
    }

    epoch = mempool_reclamation_epoch(pool);
    mempool_return(pool, third);
    if (mempool_reclaim(pool) != 1 || mempool_reclamation_epoch(pool) != epoch + 1 ||
        mempool_reclamation_is_current(pool, epoch)) {
        return 6;
    }

    /* Exercise the otherwise unreachable boundary through the public struct. */
    pool->storage_reclamation_epoch = UINT64_MAX - 1;
    epoch = mempool_reclamation_epoch(pool);
    if (!mempool_reclamation_is_current(pool, epoch)) {
        return 7;
    }
    first = mempool_get(pool);
    mempool_return(pool, first);
    if (mempool_reclaim(pool) != 1 || mempool_reclamation_epoch(pool) != UINT64_MAX ||
        mempool_reclamation_is_current(pool, epoch) ||
        mempool_reclamation_is_current(pool, UINT64_MAX)) {
        return 8;
    }
    first = mempool_get(pool);
    mempool_return(pool, first);
    if (mempool_reclaim(pool) != 1 || mempool_reclamation_epoch(pool) != UINT64_MAX ||
        mempool_reclamation_is_current(pool, UINT64_MAX) ||
        mempool_reclamation_is_current(pool, 0)) {
        return 9;
    }
    return 0;
}

static int test_mempool_bypass_reclamation(void) {
    mempool_struct *pool = mempool_create("bypass reclamation test",
                                         2,
                                         sizeof(uint64_t),
                                         MEMPOOL_BYPASS_POOLS | MEMPOOL_ALLOW_FREEING,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL);
    mempool_struct *other = mempool_create("unreclaimed test",
                                          1,
                                          sizeof(uint64_t),
                                          0,
                                          NULL,
                                          NULL,
                                          NULL,
                                          NULL);
    uint64_t epoch = mempool_reclamation_epoch(pool);
    uint64_t other_epoch = mempool_reclamation_epoch(other);
    void *first = mempool_get(pool);
    void *second = mempool_get(pool);
    void *other_chunk = mempool_get(other);
    mempool_return(other, other_chunk);
    if (epoch != 0 || mempool_reclaim(other) != 0 || mempool_reclaim(pool) != 0 ||
        !mempool_reclamation_is_current(pool, epoch) ||
        !mempool_reclamation_is_current(other, other_epoch)) {
        return 10;
    }

    mempool_return(pool, first);
    if (mempool_reclamation_epoch(pool) != epoch + 1 ||
        mempool_reclamation_is_current(pool, epoch) ||
        !mempool_reclamation_is_current(other, other_epoch)) {
        return 11;
    }
    pool->storage_reclamation_epoch = UINT64_MAX - 1;
    mempool_return(pool, second);
    if (mempool_reclamation_epoch(pool) != UINT64_MAX ||
        mempool_reclamation_is_current(pool, UINT64_MAX)) {
        return 12;
    }
    first = mempool_get(pool);
    mempool_return(pool, first);
    if (mempool_reclamation_epoch(pool) != UINT64_MAX ||
        mempool_reclamation_is_current(pool, 0) ||
        mempool_reclamation_is_current(pool, UINT64_MAX) ||
        !mempool_reclamation_is_current(other, other_epoch)) {
        return 13;
    }
    return 0;
}

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

    toolkit_import(mempool);
    int result = test_mempool_reclamation();
    if (result == 0) {
        result = test_mempool_bypass_reclamation();
    }
    toolkit_deinit();
    return result;
}
