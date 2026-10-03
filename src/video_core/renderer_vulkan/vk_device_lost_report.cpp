// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_device_lost_report.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace Vulkan {

namespace {

constexpr size_t HistorySize = 256;
static_assert((HistorySize & (HistorySize - 1)) == 0, "History size must be a power of two");

struct StageRecord {
    u64 pgm_hash;
    Shader::SwStage stage;
    bool uses_dma;
};

struct WorkRecord {
    u64 sequence;
    u64 pipeline_hash;
    WorkKind kind;
    u32 num_stages;
    std::array<StageRecord, Shader::MaxStageTypes> stages;
};

// Written only by the GPU thread. The report may read it while that thread is still running,
// which can tear the newest entries. That is acceptable for a diagnostic of a fatal error.
std::array<WorkRecord, HistorySize> history;
std::atomic<u64> work_counter{0};
std::atomic<bool> report_written{false};

const char* KindName(WorkKind kind) {
    switch (kind) {
    case WorkKind::Draw:
        return "Draw";
    case WorkKind::DrawIndirect:
        return "DrawIndirect";
    case WorkKind::Dispatch:
        return "Dispatch";
    case WorkKind::DispatchIndirect:
        return "DispatchIndirect";
    }
    return "Unknown";
}

const char* StageName(Shader::SwStage stage) {
    switch (stage) {
    case Shader::SwStage::Fragment:
        return "Fragment";
    case Shader::SwStage::TessellationControl:
        return "TessellationControl";
    case Shader::SwStage::TessellationEval:
        return "TessellationEval";
    case Shader::SwStage::Vertex:
        return "Vertex";
    case Shader::SwStage::Geometry:
        return "Geometry";
    case Shader::SwStage::Compute:
        return "Compute";
    default:
        return "Unknown";
    }
}

const char* ReadbacksModeName(u32 mode) {
    switch (mode) {
    case GpuReadbacksMode::Disabled:
        return "Disabled";
    case GpuReadbacksMode::Relaxed:
        return "Relaxed";
    case GpuReadbacksMode::Precise:
        return "Precise";
    default:
        return "Unknown";
    }
}

void AppendWorkHistory(std::string& out) {
    const u64 total = work_counter.load(std::memory_order_acquire);
    const u64 count = std::min<u64>(total, HistorySize);
    fmt::format_to(std::back_inserter(out),
                   "== Last {} of {} draws/dispatches (newest first) ==\n"
                   "Recorded when the commands were recorded, which can be well before the GPU\n"
                   "ran them, so the culprit may be older than the newest entries.\n",
                   count, total);
    for (u64 i = 0; i < count; ++i) {
        const WorkRecord& rec = history[(total - 1 - i) & (HistorySize - 1)];
        bool any_dma = false;
        for (u32 s = 0; s < rec.num_stages; ++s) {
            any_dma |= rec.stages[s].uses_dma;
        }
        fmt::format_to(std::back_inserter(out), "#{} {} pipeline={:#018x} dma={}\n", rec.sequence,
                       KindName(rec.kind), rec.pipeline_hash, any_dma ? "yes" : "no");
        for (u32 s = 0; s < rec.num_stages; ++s) {
            const StageRecord& stage = rec.stages[s];
            fmt::format_to(std::back_inserter(out), "    {:<20} shader={:#018x} dma={}\n",
                           StageName(stage.stage), stage.pgm_hash, stage.uses_dma ? "yes" : "no");
        }
    }
    out += '\n';
}

void AppendConfiguration(std::string& out, const Instance& instance) {
    auto it = std::back_inserter(out);
    const auto& elf_info = Common::ElfInfo::Instance();
    fmt::format_to(it, "== Game ==\nserial: {}\n\n", elf_info.GameSerial());

    fmt::format_to(it, "== GPU ==\nname: {}\nvendor: {} ({:#x})\ndevice id: {:#x}\n",
                   instance.GetModelName(), instance.GetVendorName(), instance.GetVendorID(),
                   instance.GetDeviceID());
    fmt::format_to(it, "driver id: {}\ndriver version: {:#x}\nvulkan api: {}.{}.{}\n\n",
                   vk::to_string(instance.GetDriverID()), instance.GetDriverVersion(),
                   VK_API_VERSION_MAJOR(instance.ApiVersion()),
                   VK_API_VERSION_MINOR(instance.ApiVersion()),
                   VK_API_VERSION_PATCH(instance.ApiVersion()));

    fmt::format_to(it, "== Active GPU configuration ==\n");
    fmt::format_to(it, "direct_memory_access_enabled: {}\n",
                   EmulatorSettings.IsDirectMemoryAccessEnabled());
    fmt::format_to(it, "readbacks_mode: {}\n",
                   ReadbacksModeName(EmulatorSettings.GetReadbacksMode()));
    fmt::format_to(it, "readback_linear_images_enabled: {}\n",
                   EmulatorSettings.IsReadbackLinearImagesEnabled());
    fmt::format_to(it, "copy_gpu_buffers: {}\n", EmulatorSettings.IsCopyGpuBuffers());
    fmt::format_to(it, "async_pipeline_compilation: {}\n",
                   EmulatorSettings.IsAsyncPipelineCompilation());
    fmt::format_to(it, "inline_fetch_shader: {}\n", EmulatorSettings.IsInlineFetchShader());
    fmt::format_to(it, "patch_shaders: {}\n", EmulatorSettings.IsPatchShaders());
    fmt::format_to(it, "userfaultfd: {}\n", EmulatorSettings.IsUserfaultfdTracking());
    fmt::format_to(it, "null_gpu: {}\n", EmulatorSettings.IsNullGPU());
    fmt::format_to(it, "internal_screen: {}x{}\n", EmulatorSettings.GetInternalScreenWidth(),
                   EmulatorSettings.GetInternalScreenHeight());
    fmt::format_to(it, "present_mode: {}\n", EmulatorSettings.GetPresentMode());
    fmt::format_to(it, "pipeline_cache_enabled: {}\n", EmulatorSettings.IsPipelineCacheEnabled());
    fmt::format_to(it, "pipeline_cache_archived: {}\n", EmulatorSettings.IsPipelineCacheArchived());
    fmt::format_to(it, "vkvalidation_enabled: {}\n", EmulatorSettings.IsVkValidationEnabled());
    fmt::format_to(it, "vkcrash_diagnostic_enabled: {}\n\n",
                   EmulatorSettings.IsVkCrashDiagnosticEnabled());
}

void AppendDeviceFault(std::string& out, const Instance& instance) {
    auto it = std::back_inserter(out);
    fmt::format_to(it, "== VK_EXT_device_fault ==\n");
    if (!instance.IsDeviceFaultSupported()) {
        fmt::format_to(it, "not supported by this device or driver\n\n");
        return;
    }

    const vk::Device device = instance.GetDevice();
    vk::DeviceFaultCountsEXT counts{};
    vk::Result result = device.getFaultInfoEXT(&counts, nullptr);
    if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
        fmt::format_to(it, "vkGetDeviceFaultInfoEXT failed: {}\n\n", vk::to_string(result));
        return;
    }

    std::vector<vk::DeviceFaultAddressInfoEXT> address_infos(counts.addressInfoCount);
    std::vector<vk::DeviceFaultVendorInfoEXT> vendor_infos(counts.vendorInfoCount);
    vk::DeviceFaultInfoEXT info{};
    info.pAddressInfos = address_infos.data();
    info.pVendorInfos = vendor_infos.data();
    // Vendor binary crash dumps are not requested, they can be very large.
    result = device.getFaultInfoEXT(&counts, &info);
    if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
        fmt::format_to(it, "vkGetDeviceFaultInfoEXT failed: {}\n\n", vk::to_string(result));
        return;
    }

    fmt::format_to(it, "description: {}\n", info.description.data());
    fmt::format_to(it, "address infos: {}\n", counts.addressInfoCount);
    for (const auto& address : address_infos) {
        fmt::format_to(it, "    type={} address={:#x} precision={:#x}\n",
                       vk::to_string(address.addressType), address.reportedAddress,
                       address.addressPrecision);
    }
    fmt::format_to(it, "vendor infos: {}\n", counts.vendorInfoCount);
    for (const auto& vendor : vendor_infos) {
        fmt::format_to(it, "    {} code={:#x} data={:#x}\n", vendor.description.data(),
                       vendor.vendorFaultCode, vendor.vendorFaultData);
    }
    out += '\n';
}

} // namespace

void RecordGpuWork(WorkKind kind, const Pipeline& pipeline) {
    const u64 sequence = work_counter.load(std::memory_order_relaxed);
    WorkRecord& rec = history[sequence & (HistorySize - 1)];
    rec.sequence = sequence;
    rec.pipeline_hash = pipeline.GetDebugHash();
    rec.kind = kind;
    rec.num_stages = 0;
    for (const Shader::Info* stage : pipeline.GetStages()) {
        if (stage) {
            rec.stages[rec.num_stages++] = {stage->pgm_hash, stage->sw_stage, stage->uses_dma};
        }
    }
    work_counter.store(sequence + 1, std::memory_order_release);
}

void WriteDeviceLostReport(const Instance& instance, std::string_view context) {
    if (report_written.exchange(true)) {
        return;
    }
    try {
        std::string out;
        const auto now = std::chrono::system_clock::now();
        fmt::format_to(std::back_inserter(out),
                       "shadPS4 device lost report\ntime: {:%Y-%m-%d %H:%M:%S} UTC\n"
                       "detected in: {}\n\n",
                       std::chrono::floor<std::chrono::seconds>(now), context);
        AppendConfiguration(out, instance);
        AppendWorkHistory(out);
        AppendDeviceFault(out, instance);

        const auto path =
            Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "device_lost_report.txt";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        file.flush();
        if (file) {
            LOG_CRITICAL(Render_Vulkan, "Device lost, report written to {}", path.string());
        } else {
            LOG_CRITICAL(Render_Vulkan, "Device lost, could not write {}", path.string());
        }
    } catch (const std::exception& e) {
        LOG_CRITICAL(Render_Vulkan, "Device lost, failed to build the report: {}", e.what());
    }
}

void OnDeviceLost(const Instance& instance, std::string_view context) {
    WriteDeviceLostReport(instance, context);
    UNREACHABLE_MSG("Device lost {}", context);
}

} // namespace Vulkan
