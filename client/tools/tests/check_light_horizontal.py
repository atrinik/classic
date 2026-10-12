#!/usr/bin/env python3
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the actual HLSL span selector as C++ against a linear oracle.

Run in the pinned CPU worker, never as a substitute for GPU conformance.
Only HLSL's out-parameter spelling is adapted; sample loads are instrumented.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

SHADER = Path(__file__).resolve().parents[2] / "shaders/map.hlsl"

PREAMBLE = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <vector>
using uint = uint32_t;
using std::min;
[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}
struct int2 { int x, y; int2(int a, int b): x(a), y(b) {} };
struct uint4 {
    uint x, y, z, w;
    uint4(uint a, uint b, uint c, uint d): x(a), y(b), z(c), w(d) {}
    bool operator==(const uint4 &b) const {
        return x == b.x && y == b.y && z == b.z && w == b.w;
    }
};
struct LightSpan { uint quad; int first_x, last_x; };
struct CheckedSpans {
    std::vector<LightSpan> values;
    uint offset = 0, count = 0, reads = 0;
    LightSpan operator[](uint index) {
        if (index < offset || index - offset >= count)
            fail("out-of-row span access");
        ++reads;
        return values.at(index - offset);
    }
} final_light_spans;
using Call = std::tuple<uint, int, int, bool>;
std::vector<Call> calls;
uint4 final_light_load(uint quad, int2 p, bool use_coverage, uint &coverage) {
    calls.emplace_back(quad, p.x, p.y, use_coverage);
    uint seed = (quad * 131u + uint(p.x) * 17u + uint(p.y) * 23u) & 65535u;
    coverage = use_coverage ? seed % 256u : 255u;
    return uint4(seed, 65535u - seed, seed / 7u, (seed * 31u) & 65535u);
}
uint light_mul_divide_floor(uint value, uint progress, uint divisor) {
    if (!divisor || progress > divisor) fail("bad ratio");
    return uint(uint64_t(value) * progress / divisor);
}
'''

ORACLE = r'''
// Linear selection and signed interpolation are independent of the production
// upper-bound search, branch layout and unsigned increasing/decreasing helper.
uint mix(uint a, uint b, int progress, int width) {
    return uint(int64_t(a) + (int64_t(b) - a) * progress / width);
}
uint4 reference(int x, int y, bool covered, uint &coverage) {
    const auto &spans = final_light_spans.values;
    size_t left = 0;
    if (x < spans.front().first_x)
        return final_light_load(spans.front().quad, int2(spans.front().first_x, y), covered, coverage);
    for (size_t i = 0; i < spans.size(); ++i)
        if (spans[i].first_x <= x) left = i;
    const auto &a = spans[left];
    if (x <= a.last_x)
        return final_light_load(a.quad, int2(x, y), covered, coverage);
    uint4 av = final_light_load(a.quad, int2(a.last_x, y), covered, coverage);
    if (left + 1 == spans.size()) return av;
    const auto &b = spans[left + 1];
    uint bc;
    uint4 bv = final_light_load(b.quad, int2(b.first_x, y), covered, bc);
    int width = b.first_x - a.last_x, progress = x - a.last_x;
    coverage = mix(coverage, bc, progress, width);
    return uint4(mix(av.x, bv.x, progress, width), mix(av.y, bv.y, progress, width),
                 mix(av.z, bv.z, progress, width), mix(av.w, bv.w, progress, width));
}
uint64_t cases = 0;
void check_row(const std::vector<LightSpan> &row) {
    final_light_spans.values = row;
    final_light_spans.count = uint(row.size());
    for (uint offset : {0u, 3u, 103u}) {
        final_light_spans.offset = offset;
        for (int y : {-71, 0, 93}) for (bool covered : {false, true}) {
            for (int x = row.front().first_x - 2; x <= row.back().last_x + 2; ++x) {
                uint expected_coverage = 0, actual_coverage = 0;
                calls.clear();
                uint4 expected = reference(x, y, covered, expected_coverage);
                auto expected_calls = calls;
                calls.clear(); final_light_spans.reads = 0;
                uint4 actual = final_light_horizontal(offset, uint(row.size()), y, x,
                                                       covered, actual_coverage);
                uint search_budget = 0;
                for (uint n = uint(row.size()); n; n >>= 1u) ++search_budget;
                if (!(actual == expected) || actual_coverage != expected_coverage ||
                    calls != expected_calls || calls.empty() || calls.size() > 2 ||
                    final_light_spans.reads > search_budget + calls.size())
                    fail("selector, coverage, samples or lookup budget mismatch");
                ++cases;
            }
        }
    }
}
void rows(std::vector<LightSpan> &row, unsigned remaining, int next) {
    if (!row.empty()) check_row(row);
    if (!remaining) return;
    for (int width = 0; width <= 2; ++width) for (int gap = 1; gap <= 3; ++gap) {
        int first = next + gap;
        row.push_back({uint(row.size() * 97u + 11u), first, first + width});
        rows(row, remaining - 1, first + width);
        row.pop_back();
    }
}
int main() {
        std::vector<LightSpan> row;
        rows(row, 4, -9);
        for (int start : {-1000000, 1000000}) {
            row.clear();
            for (int i = 0; i < 17; ++i)
                row.push_back({uint(i * 53 + 7), start + i * 4, start + i * 4 + i % 3});
            check_row(row);
        }
        std::printf("light horizontal equivalence: %llu cases passed\n", (unsigned long long)cases);
        return 0;
}
'''


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--shader", type=Path, default=SHADER)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.shader.read_text(encoding="utf-8")
    selector = function(source, "uint4 final_light_horizontal(").replace("out uint ", "uint &")
    interpolation = function(source, "uint light_extrapolate(uint left, uint right, uint progress, uint divisor) {")
    variants = {"production": selector}
    # Controls change behavior, not just expected source spelling. Each must
    # compile successfully and then fail the oracle or checked buffer access.
    mutations = {
        "edge": ("span.first_x <= x", "span.first_x < x"),
        "gap_progress": ("uint(x - sample_span.last_x)", "uint(x - sample_span.last_x + 1)"),
        "coverage": ("use_coverage, coverage)", "false, coverage)"),
        "right_bound": ("right < count", "right <= count"),
    }
    for name, (old, new) in mutations.items():
        if old not in selector:
            raise ValueError(f"missing mutation control site: {name}")
        variants[name] = selector.replace(old, new)
    with tempfile.TemporaryDirectory(prefix="light-horizontal-") as temporary:
        root = Path(temporary)
        for name, body in variants.items():
            cpp, binary = root / f"{name}.cpp", root / name
            cpp.write_text(PREAMBLE + interpolation + body + ORACLE, encoding="utf-8")
            command = [str(args.compiler), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer"]
            subprocess.run([*command, str(cpp), "-o", str(binary)], check=True, timeout=120)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
            expected_status = 0 if name == "production" else 1
            if result.returncode != expected_status:
                raise RuntimeError(f"{name}: unexpected exit {result.returncode}: {result.stderr}")
            if name == "production" and result.stderr:
                raise RuntimeError(f"production emitted diagnostics: {result.stderr}")
            if name != "production":
                expected_error = (
                    "out-of-row span access" if name == "right_bound"
                    else "selector, coverage, samples or lookup budget mismatch"
                )
                if result.stderr.strip() != expected_error:
                    raise RuntimeError(f"{name}: uncontrolled failure: {result.stderr}")
            print(result.stdout.strip() if name == "production" else f"negative control {name}: rejected")


if __name__ == "__main__":
    main()
