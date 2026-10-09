# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
if (NOT DEFINED SERVER_EXECUTABLE OR NOT EXISTS "${SERVER_EXECUTABLE}" OR
        IS_DIRECTORY "${SERVER_EXECUTABLE}" OR IS_SYMLINK "${SERVER_EXECUTABLE}")
    message(FATAL_ERROR "Server capability producer requires a regular executable")
endif ()
file(SHA256 "${SERVER_EXECUTABLE}" server_sha256)
get_filename_component(server_directory "${SERVER_EXECUTABLE}" DIRECTORY)
set(output "${server_directory}/atrinik-server-capabilities.json")
if (IS_SYMLINK "${output}" OR IS_DIRECTORY "${output}" OR
        IS_SYMLINK "${output}.tmp" OR IS_DIRECTORY "${output}.tmp")
    message(FATAL_ERROR "Unsafe server capability output")
endif ()
file(WRITE "${output}.tmp"
    "{\"schema_version\":1,\"datapath_fd\":true,\"server_sha256\":\"${server_sha256}\"}\n")
file(RENAME "${output}.tmp" "${output}")
