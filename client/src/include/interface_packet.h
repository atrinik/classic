#ifndef INTERFACE_PACKET_H
#define INTERFACE_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool interface_packet_validate(const uint8_t *data, size_t len, size_t pos);

#endif
