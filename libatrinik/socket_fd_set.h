/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TOOLKIT_SOCKET_FD_SET_H
#define TOOLKIT_SOCKET_FD_SET_H

#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

/* Private select() adapter. Callers retain their existing descriptor-validity
 * checks. Winsock's FD_SET compares against SOCKET before assigning it, so use
 * the same signed-int-to-SOCKET conversion as its assignment and the socket
 * APIs. An unsigned-int intermediate would change negative values on Win64. */
static inline void socket_fd_set(int handle, fd_set *set) {
#ifdef WIN32
    FD_SET((SOCKET)handle, set);
#else
    FD_SET(handle, set);
#endif
}

#endif
