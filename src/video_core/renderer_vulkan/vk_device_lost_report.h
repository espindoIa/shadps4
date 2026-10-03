// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Pipeline;

/// What a recorded GPU work item was submitted as.
enum class WorkKind : u8 {
    Draw,
    DrawIndirect,
    Dispatch,
    DispatchIndirect,
};

/// Remembers the pipeline and shaders of the most recent draws and dispatches, so that a device
/// lost can be diagnosed. Recording costs a few stores into a fixed ring, nothing else.
void RecordGpuWork(WorkKind kind, const Pipeline& pipeline);

/// Writes device_lost_report.txt to the log directory. Never throws and writes only once, even
/// if several threads observe the failure.
void WriteDeviceLostReport(const Instance& instance, std::string_view context);

/// Writes the report and aborts the emulator. Does not return.
[[noreturn]] void OnDeviceLost(const Instance& instance, std::string_view context);

/// Replaces the former `ASSERT_MSG(result != eErrorDeviceLost)` checks.
inline void CheckDeviceLost(const Instance& instance, vk::Result result, std::string_view context) {
    if (result == vk::Result::eErrorDeviceLost) [[unlikely]] {
        OnDeviceLost(instance, context);
    }
}

} // namespace Vulkan
