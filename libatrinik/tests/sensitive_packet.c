/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <toolkit/packet.h>
#define REQUIRE(x) do { if (!(x)) abort(); } while (0)
#ifdef PACKET_TEST_FREE
static void *watched;
static size_t watched_size;
static bool observed;
void __real_free(void *ptr);
void __wrap_free(void *ptr) {
    if (ptr != NULL && ptr == watched) {
        const unsigned char *p = ptr;
        for (size_t i = 0; i < watched_size; i++) REQUIRE(p[i] == 0);
        watched = NULL; observed = true;
    }
    __real_free(ptr);
}
#endif
int main(void) {
    toolkit_import(packet);
    packet_struct *p = packet_new(42, 1, 1);
    packet_mark_sensitive(p);
    packet_writer_write_string(p, "synthetic-secret");
    char *debug = packet_get_debug(p);
    REQUIRE(strstr(debug, "synthetic") == NULL); free(debug);
    packet_struct *copy = packet_dup(p);
    REQUIRE(copy->sensitive && copy->len == p->len);
    packet_struct *joined = packet_new(2, 0, 0);
    packet_writer_write_packet(joined, copy);
    REQUIRE(joined->sensitive);
    packet_compress(joined);
    REQUIRE(joined->type == 2);
    packet_writer_mark_t mark;
    packet_writer_mark(p, &mark);
    packet_writer_write_string(p, "tail");
    packet_writer_rollback(p, &mark);
    for (size_t i = 0; i < 4; i++) REQUIRE(p->data[p->len + i] == 0);
#ifdef PACKET_TEST_FREE
    watched = p->data; watched_size = p->size;
#endif
    packet_free(p);
#ifdef PACKET_TEST_FREE
    REQUIRE(observed);
#endif
    packet_free(copy); packet_free(joined);
    toolkit_deinit();
    return 0;
}
