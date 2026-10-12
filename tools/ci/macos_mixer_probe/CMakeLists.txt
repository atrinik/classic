# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)
project(AtrinikMixerProbe C)
find_package(SDL3 3.4 CONFIG REQUIRED)
find_package(SDL3_mixer 3.2.4 CONFIG REQUIRED)
add_executable(mixer-probe main.c)
set_target_properties(mixer-probe PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(mixer-probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mixer-probe PRIVATE SDL3::SDL3 SDL3_mixer::SDL3_mixer)
