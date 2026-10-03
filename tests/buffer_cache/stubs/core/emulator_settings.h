// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bit>
#include <mutex>

enum GpuReadbacksMode : int {
    Disabled,
    Relaxed,
    Precise,
};

struct RegionTestSettings {
    unsigned GetReadbacksMode() const {
        return Precise;
    }
};

inline constexpr RegionTestSettings EmulatorSettings{};
