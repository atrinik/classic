/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <curl/curl.h>

#if LIBCURL_VERSION_NUM >= 0x075600
CURLcode __wrap_curl_ws_recv(CURL *curl,
                           void *buffer,
                           size_t buflen,
                           size_t *recv,
                           const struct curl_ws_frame **meta);
#define curl_ws_recv __wrap_curl_ws_recv
#endif
#include "../../socket_direct.c"
