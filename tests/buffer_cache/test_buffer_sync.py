# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""CPU-only regression checks for buffer acquisition and image synchronization.

Run with Python 3 and a C++23 compiler on Linux. CXX and CXXFLAGS are honored.
The two production method definitions are compiled unchanged into a small harness;
Vulkan, memory, and cache dependencies are replaced with byte arrays and counters.
This checks routing, copied contents, and duplicate work, not Vulkan execution.
An optional source-file argument allows checking an older revision.
"""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[2]
source = Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else (root / 'src/video_core/buffer_cache/buffer_cache.cpp').read_text()
def method(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

preamble = r'''
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using VAddr = u64;
namespace vk { struct BufferCopy { u64 srcOffset, dstOffset, size; }; }
namespace boost::container { template<class T, size_t N> using small_vector = std::vector<T>; }
enum class MemoryType { HostUncached };
struct Buffer {
    std::vector<u8> bytes = std::vector<u8>(65536, 0);
    VAddr cpu_addr = 0x100000;
    u64 Offset(VAddr address) const { return address - cpu_addr; }
    std::pair<u8*, u64> Map(u32 size, u64) { return {bytes.data(), 0}; }
    void Commit() {}
};
struct Memory {
    std::vector<u8> bytes = std::vector<u8>(65536, 0x11);
    void CopySparseMemory(u64 address, u8* data, u64 size) {
        std::memcpy(data, bytes.data() + address - 0x100000, size);
    }
};
struct Tracker {
    bool dirty = true;
    template<class F> void ForEachUploadRange(u64 address, u64 size, bool, F&& f) {
        if (dirty) { f(address, size); dirty = false; }
    }
};
struct Staging {
    u64 offset; u8* mapped; Buffer* buffer;
    void Flush() const {}
};
struct Pool {
    Buffer buffer;
    Staging Request(u64, MemoryType) { return {0, buffer.bytes.data(), &buffer}; }
};
struct Runtime {
    void CopyBuffer(const Buffer* source, const Buffer* dest,
                    const std::vector<vk::BufferCopy>& copies) {
        for (const auto& copy : copies) {
            std::memcpy(const_cast<Buffer*>(dest)->bytes.data() + copy.dstOffset,
                        source->bytes.data() + copy.srcOffset, copy.size);
        }
    }
};
struct Ranges { unsigned writes = 0; void Add(u64, u64) { ++writes; } };
struct Instance { u64 UniformMinAlignment() const { return 16; } };
class BufferCache {
public:
    static constexpr u64 STREAM_THRESHOLD = 16384;
    u64 block_shift = 16;
    Instance instance;
    Buffer stream_buffer, arena;
    Memory memory_storage;
    Memory* memory = &memory_storage;
    Tracker tracker;
    Tracker* memory_tracker = &tracker;
    Pool staging_pool;
    Runtime runtime;
    Ranges gpu_modified_ranges;
    bool gpu_modified = false, has_image = true;
    unsigned image_syncs = 0, resident_calls = 0;
    bool IsRegionGpuModified(u64, u64) { return gpu_modified; }
    const Buffer* GetArena(u64, u64) { return &arena; }
    void EnsureResident(const Buffer*, u64, u64) { ++resident_calls; }
    bool SynchronizeMemoryFromImage(const Buffer* buffer, VAddr address, u32 size) {
        if (!has_image) return false;
        ++image_syncs;
        auto begin = const_cast<Buffer*>(buffer)->bytes.begin() + buffer->Offset(address);
        std::fill(begin, begin + size, 0x77);
        return true;
    }
    std::pair<const Buffer*, u64> ObtainBuffer(VAddr, u32, bool, bool);
    bool SynchronizeMemory(const Buffer*, VAddr, u32, bool, bool);
};
'''
checks = r'''
int main() {
    unsigned failed = 0;
    auto check = [&](bool pass, const char* name) {
        std::cout << (pass ? "PASS " : "FAIL ") << name << '\n';
        failed += !pass;
    };
    for (u32 size : {4u, 4096u, 16384u, 16385u, 32768u}) {
        BufferCache cache;
        auto [buffer, offset] = cache.ObtainBuffer(0x100000, size, false, true);
        check(std::all_of(buffer->bytes.begin() + offset,
                          buffer->bytes.begin() + offset + size,
                          [](u8 b) { return b == 0x77; }),
              "texel source contains GPU image data");
        check(cache.image_syncs == 1, "image synchronized exactly once");
    }
    {
        BufferCache cache;
        auto [buffer, offset] = cache.ObtainBuffer(0x100000, 4096, false, false);
        check(buffer == &cache.stream_buffer && buffer->bytes[offset] == 0x11 &&
              cache.resident_calls == 0 && cache.image_syncs == 0,
              "ordinary small read retains streaming fast path");
    }
    {
        BufferCache cache;
        auto [buffer, offset] = cache.ObtainBuffer(0x100000, 4096, true, true);
        check(buffer == &cache.arena && cache.image_syncs == 0 &&
              cache.gpu_modified_ranges.writes == 1,
              "writable texel destination tracks GPU writes without image read");
    }
    {
        BufferCache cache;
        cache.gpu_modified = true;
        auto [buffer, offset] = cache.ObtainBuffer(0x100000, 4096, false, true);
        check(buffer->bytes[offset] == 0x77 && cache.image_syncs == 1,
              "GPU-modified source synchronizes image exactly once");
    }
    {
        BufferCache cache;
        cache.has_image = false;
        auto [buffer, offset] = cache.ObtainBuffer(0x100000, 4096, false, true);
        check(buffer->bytes[offset] == 0x11 && cache.image_syncs == 0,
              "texel source without cached image preserves CPU data");
    }
    return failed ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='shadps4-buffer-sync-') as tmp:
    test = Path(tmp) / 'test.cpp'
    test.write_text(preamble + method('std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(')
                    + method('bool BufferCache::SynchronizeMemory(') + checks)
    binary = Path(tmp) / 'test'
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    flags = shlex.split(os.environ.get(
        'CXXFLAGS', '-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'))
    subprocess.run(compiler + ['-std=c++23'] + flags +
                   [str(test), '-o', str(binary)], check=True)
    sys.exit(subprocess.run([str(binary)]).returncode)
