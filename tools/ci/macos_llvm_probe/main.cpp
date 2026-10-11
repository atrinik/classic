/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string>
int main()
{
    return std::string("LLVM").size() == 4 ? 0 : 1;
}
