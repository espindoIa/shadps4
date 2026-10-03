// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "video_core/buffer_cache/memory_tracker.h"

namespace {

using namespace VideoCore;

std::function<void(const Bounds&, const RegionBits&, const RegionBits&, PageOp, PageOp)> on_watch;

void Check(bool result, const char* message) {
    if (!result) {
        throw std::runtime_error(message);
    }
}

void Clear(RegionManager& manager, u64 offset = 0, u64 size = HIGHER_PAGE_SIZE) {
    manager.ChangeRegionState<StateOp::Clear, StateOp::None>(offset, size);
}

void Dirty(RegionManager& manager, u64 offset = 0, u64 size = HIGHER_PAGE_SIZE) {
    manager.ChangeRegionState<StateOp::Set, StateOp::None>(offset, size);
}

void MultiwordEpoch(PageManager& tracker) {
    RegionManager manager{&tracker, 0};
    Clear(manager);
    const auto before = RegionManager::CpuModifiedEpoch();
    Dirty(manager);
    Check(RegionManager::CpuModifiedEpoch() == before + 1, "multiword epoch must advance once");
    for (u64 page = 0; page < NUM_REGION_PAGES; ++page) {
        Check(manager.IsRegionModified<Type::CPU>(page * BYTES_PER_PAGE, 1), "dirty page missing");
    }
}

void UnchangedEpoch(PageManager& tracker) {
    RegionManager manager{&tracker, 0};
    auto before = RegionManager::CpuModifiedEpoch();
    Dirty(manager);
    Check(RegionManager::CpuModifiedEpoch() == before, "already dirty must not advance");
    Clear(manager);
    manager.ChangeRegionState<StateOp::None, StateOp::Set>(0, HIGHER_PAGE_SIZE);
    manager.ChangeRegionState<StateOp::None, StateOp::Clear>(0, HIGHER_PAGE_SIZE);
    manager.ChangeRegionState<StateOp::None, StateOp::None>(0, HIGHER_PAGE_SIZE);
    Check(RegionManager::CpuModifiedEpoch() == before, "clear and GPU changes must not advance");
}

void DirtyLeadingWords(PageManager& tracker) {
    RegionManager manager{&tracker, 0};
    Clear(manager, BYTES_PER_WORD, HIGHER_PAGE_SIZE - BYTES_PER_WORD);
    const auto before = RegionManager::CpuModifiedEpoch();
    Dirty(manager);
    Check(RegionManager::CpuModifiedEpoch() == before + 1, "skip already-dirty leading word");
}

void WatcherPublication(PageManager& tracker) {
    RegionManager manager{&tracker, 0};
    Clear(manager);
    const auto before = RegionManager::CpuModifiedEpoch();
    unsigned calls{};
    on_watch = [&](const Bounds& bounds, const RegionBits& write, const RegionBits&, PageOp op,
                   PageOp) {
        ++calls;
        Check(RegionManager::CpuModifiedEpoch() == before + 1, "publish before unprotecting");
        Check(op == PageOp::Untrack, "unexpected protection operation");
        Check(bounds.start_word == 0 && bounds.end_word == 1, "incorrect word bounds");
        Check(bounds.start_page == 63 && bounds.end_page == 0, "incorrect page bounds");
        for (u64 page = 0; page < 2 * PAGES_PER_WORD; ++page) {
            Check(write.GetPage(page) == (page == 63 || page == 64), "incorrect write mask");
        }
    };
    Dirty(manager, BYTES_PER_WORD - 1, 2);
    on_watch = {};
    Check(calls == 1, "watcher must run once");
    Check(!manager.IsRegionModified<Type::CPU>(0, BYTES_PER_WORD - BYTES_PER_PAGE),
          "neighboring clean pages changed");
}

void CallbackPublication(PageManager& tracker) {
    RegionManager manager{&tracker, 0};
    Clear(manager);
    manager.ChangeRegionState<StateOp::None, StateOp::Set>(0, BYTES_PER_PAGE);
    manager.ChangeRegionState<StateOp::None, StateOp::Set>(2 * BYTES_PER_WORD, BYTES_PER_PAGE);
    const auto before = RegionManager::CpuModifiedEpoch();
    unsigned calls{};
    manager.ForEachModifiedRange<Type::GPU, StateOp::Set, StateOp::None>(
        0, 3 * BYTES_PER_WORD, [&](VAddr address, u64 size) {
            Check(RegionManager::CpuModifiedEpoch() == before + 1, "publish before callbacks");
            Check(address == calls * 2 * BYTES_PER_WORD, "incorrect callback address");
            Check(size == BYTES_PER_PAGE, "incorrect callback size");
            ++calls;
        });
    Check(calls == 2, "both disjoint GPU ranges must be visited");
    Check(RegionManager::CpuModifiedEpoch() == before + 1, "callback operation advances once");
}

void UploadThenDirty(PageManager& tracker) {
    RegionManager manager{&tracker, 0};
    unsigned calls{};
    const auto before = RegionManager::CpuModifiedEpoch();
    manager.ForEachModifiedRange<Type::CPU, StateOp::Clear, StateOp::None>(
        0, HIGHER_PAGE_SIZE, [&](VAddr address, u64 size) {
            Check(address == 0 && size == HIGHER_PAGE_SIZE, "full upload range must merge");
            ++calls;
        });
    Check(calls == 1, "one contiguous upload");
    Check(RegionManager::CpuModifiedEpoch() == before, "upload must not advance epoch");
    Dirty(manager);
    Check(RegionManager::CpuModifiedEpoch() == before + 1, "post-upload writes must invalidate");
    Dirty(manager);
    Check(RegionManager::CpuModifiedEpoch() == before + 1, "repeated write must not invalidate");
}

void ManuallyLocked(PageManager& tracker) {
    RegionManager manager{&tracker, 0};
    Clear(manager);
    const auto before = RegionManager::CpuModifiedEpoch();
    const auto bounds = RegionManager::GetBounds(0, HIGHER_PAGE_SIZE);
    manager.Lock(bounds);
    manager.ChangeRegionState<StateOp::Set, StateOp::None, false>(0, HIGHER_PAGE_SIZE);
    manager.Unlock(bounds);
    Check(RegionManager::CpuModifiedEpoch() == before + 1, "unlocked variant advances once");
}

void IndependentRegions(PageManager& tracker) {
    RegionManager first{&tracker, 0};
    RegionManager second{&tracker, HIGHER_PAGE_SIZE};
    Clear(first);
    Clear(second);
    const auto before = RegionManager::CpuModifiedEpoch();
    Dirty(first);
    Dirty(second);
    Check(RegionManager::CpuModifiedEpoch() == before + 2, "regions invalidate independently");
    Clear(first);
    Dirty(first);
    Check(RegionManager::CpuModifiedEpoch() == before + 3, "later writes invalidate again");
}

void NewRegionAndBoundary(PageManager& tracker) {
    auto memory_tracker = std::make_unique<MemoryTracker>(tracker);
    const VAddr address = HIGHER_PAGE_SIZE - BYTES_PER_WORD;
    const u64 size = 2 * BYTES_PER_WORD;
    const auto before = RegionManager::CpuModifiedEpoch();
    unsigned uploads{};
    memory_tracker->ForEachUploadRange(address, size, false, [&](VAddr, u64 bytes) {
        Check(bytes == BYTES_PER_WORD, "upload must stop at region boundary");
        ++uploads;
    });
    Check(uploads == 2, "both newly created regions must upload");
    Check(RegionManager::CpuModifiedEpoch() == before + 2, "creation must invalidate both regions");
    memory_tracker->MarkRegionAsCpuModified(address, size);
    Check(RegionManager::CpuModifiedEpoch() == before + 4, "boundary writes invalidate each region");
    memory_tracker->MarkRegionAsCpuModified(address, size);
    Check(RegionManager::CpuModifiedEpoch() == before + 4, "repeated boundary write is unchanged");
}

void ConcurrentRegions(PageManager& tracker) {
    RegionManager first{&tracker, 0};
    RegionManager second{&tracker, HIGHER_PAGE_SIZE};
    constexpr unsigned iterations = 1000;
    const auto before = RegionManager::CpuModifiedEpoch();
    auto work = [](RegionManager& manager) {
        for (unsigned i = 0; i < iterations; ++i) {
            Clear(manager);
            Dirty(manager);
        }
    };
    std::thread one{work, std::ref(first)};
    std::thread two{work, std::ref(second)};
    one.join();
    two.join();
    Check(RegionManager::CpuModifiedEpoch() == before + 2 * iterations,
          "concurrent region invalidations lost");
}

} // namespace

namespace VideoCore {

struct PageManager::Impl {};
PageManager::PageManager(Vulkan::Rasterizer*) {}
PageManager::~PageManager() = default;

void PageManager::UpdatePageWatchersForRegion(VAddr, const Bounds& bounds,
                                              const RegionBits& write, const RegionBits& read,
                                              PageOp write_op, PageOp read_op) const {
    if (on_watch) {
        on_watch(bounds, write, read, write_op, read_op);
    }
}

} // namespace VideoCore

int main(int argc, char** argv) {
    VideoCore::PageManager tracker{nullptr};
    if (argc > 1 && std::string{argv[1]} == "--benchmark") {
        VideoCore::RegionManager manager{&tracker, 0};
        constexpr unsigned iterations = 200000;
        const auto before = VideoCore::RegionManager::CpuModifiedEpoch();
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < iterations; ++i) {
            Clear(manager);
            Dirty(manager);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        std::cout << "iterations=" << iterations << " epoch_updates="
                  << VideoCore::RegionManager::CpuModifiedEpoch() - before << " elapsed_us="
                  << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() << '\n';
        return 0;
    }
    const std::pair<const char*, void (*)(VideoCore::PageManager&)> tests[] = {
        {"MultiwordEpoch", MultiwordEpoch},
        {"UnchangedEpoch", UnchangedEpoch},
        {"DirtyLeadingWords", DirtyLeadingWords},
        {"WatcherPublication", WatcherPublication},
        {"CallbackPublication", CallbackPublication},
        {"UploadThenDirty", UploadThenDirty},
        {"ManuallyLocked", ManuallyLocked},
        {"IndependentRegions", IndependentRegions},
        {"ConcurrentRegions", ConcurrentRegions},
        {"NewRegionAndBoundary", NewRegionAndBoundary},
    };
    unsigned failures{};
    for (const auto& [name, test] : tests) {
        try {
            test(tracker);
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            on_watch = {};
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests) << " passed\n";
    return failures ? 1 : 0;
}
