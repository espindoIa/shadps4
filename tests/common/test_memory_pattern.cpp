// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "common/memory_patcher.h"

namespace MemoryPatcher {

uintptr_t g_eboot_address{};
uint64_t g_eboot_image_size{};

}

namespace {

struct PatternCase {
    std::string_view name;
    std::vector<uint8_t> image;
    std::string signature;
    std::optional<size_t> expected_offset;
};

bool RunCase(const PatternCase& test) {
    MemoryPatcher::g_eboot_address = reinterpret_cast<uintptr_t>(test.image.data());
    MemoryPatcher::g_eboot_image_size = test.image.size();
    const uintptr_t expected = test.expected_offset
                                   ? MemoryPatcher::g_eboot_address + *test.expected_offset
                                   : 0;
    const uintptr_t actual = MemoryPatcher::PatternScan(test.signature);
    if (actual != expected) {
        std::cerr << "FAIL " << test.name << ": expected " << expected << ", got " << actual
                  << '\n';
        return false;
    }
    std::cout << "PASS " << test.name << '\n';
    return true;
}

}

int main(int argc, char* argv[]) {
    const PatternCase tests[] = {
        {"oversized_signature", {0xAA}, "AA BB", std::nullopt},
        {"oversized_wildcards", {0xAA}, "? ??", std::nullopt},
        {"exact_size_match", {0xAA, 0xBB}, "AA BB", 0},
        {"final_offset_match", {0x00, 0xAA, 0xBB}, "AA BB", 1},
        {"empty_signature", {0xAA, 0xBB}, "", std::nullopt},
        {"empty_image", {}, "AA", std::nullopt},
        {"empty_image_and_signature", {}, "", std::nullopt},
        {"no_match", {0xAA, 0xBB, 0xCC}, "AA CC", std::nullopt},
        {"exact_size_no_match", {0xAA, 0xBB}, "AA CC", std::nullopt},
        {"first_match", {0xAA, 0xBB, 0x00, 0xAA, 0xBB}, "AA BB", 0},
        {"middle_match", {0x00, 0xAA, 0xBB, 0xCC}, "AA BB", 1},
        {"single_byte_final_match", {0x00, 0xAA}, "AA", 1},
        {"single_wildcard", {0xAA, 0x12, 0xBB, 0x00}, "AA ? BB", 0},
        {"double_wildcard", {0xAA, 0x12, 0xBB, 0x00}, "AA ?? BB", 0},
        {"trailing_single_wildcard", {0x00, 0xAA, 0x12}, "AA ?", 1},
        {"trailing_double_wildcard", {0x00, 0xAA, 0x12}, "AA ??", 1},
        {"exact_size_trailing_wildcard", {0xAA, 0x12}, "AA ??", 0},
        {"all_wildcards", {0xAA, 0xBB}, "? ??", 0},
        {"one_wildcard", {0xAA}, "?", 0},
        {"one_double_wildcard", {0xAA}, "??", 0},
        {"wildcard_no_match", {0xAA, 0x12, 0xCC, 0x00}, "AA ?? BB", std::nullopt},
        {"lowercase_hex", {0x00, 0xAB, 0xCD, 0x00}, "ab cd", 1},
    };

    bool selected = false;
    bool passed = true;
    for (const auto& test : tests) {
        if (argc > 1 && test.name != argv[1]) {
            continue;
        }
        selected = true;
        passed = RunCase(test) && passed;
    }
    if (!selected) {
        std::cerr << "Unknown test case\n";
        return 2;
    }
    return passed ? 0 : 1;
}
