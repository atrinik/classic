# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.21)
include(../../../cmake/AtrinikLLVM.cmake)
project(AtrinikMacOSLLVMProbe LANGUAGES C CXX)
atrinik_require_llvm()
add_executable(llvm-c-probe main.c)
add_executable(llvm-cxx-probe main.cpp)
