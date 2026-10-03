# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

shadPS4 is a PlayStation 4 emulator (C++23, Windows/Linux/macOS). This repo is the **emulator core, a CLI
without a GUI**; the Qt launcher lives in another repo. `origin` is a fork (`espindoIa/shadps4`) of
`shadps4-emu/shadPS4`; sync by merging upstream `main`, not rebasing.

## Build

- Needs **Clang 19** (what CI uses). Clang 18 with libstdc++ 13 fails in `common/logging/log.cpp` because
  `std::ranges::to` is missing. CMake also needs `clang-scan-deps` (package `clang-tools-19` on Ubuntu).
- Submodules are required and some are nested, so use `git submodule update --init --recursive`. Linux
  system packages are listed in `documents/building-linux.md`.
- Configure and build (Release is the default, and `-march=x86-64-v3` is forced):
  `cmake -S . -B build-emu -G Ninja -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF && ninja -C build-emu shadps4`
- In the cloud sandbox GitHub downloads are blocked, so spdlog's `FetchContent` of fmt fails. Add
  `-DFETCHCONTENT_SOURCE_DIR_FMT=$PWD/externals/fmt` (and `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/usr/src/googletest`
  with the `googletest` apt package for tests). The sandbox has no GPU, so games cannot be run there.
- Every source file is listed explicitly in `CMakeLists.txt`; a new `.cpp`/`.h` must be added to the right
  list (`CORE`, `VIDEO_CORE`, `SHADER_RECOMPILER`, ...).

## Tests

`-DENABLE_TESTS=ON` **removes the `shadps4` target** (see `if(NOT ENABLE_TESTS)` in `CMakeLists.txt`), so tests
and the emulator need separate build directories. Targets: `shadps4_settings_test`, `shadps4_ngs2_test`,
`shadps4_gcn_test`, `shadps4_http_test`.

```
cmake -S . -B build -G Ninja -DENABLE_TESTS=ON <same compiler/FETCHCONTENT flags>
ninja -C build shadps4_settings_test
./build/tests/shadps4_settings_test --gtest_filter='EmulatorSettingsTest.LoadSerial*'
```

Tests compile the code under test directly, with stubs from `tests/stubs/` instead of linking the emulator.
If code under test gains a new dependency, add its sources or a stub to `tests/CMakeLists.txt`. The GCN test
lists the shader recompiler sources one by one, so new recompiler files must be added there too.

## Lint (all run in CI)

- `clang-format-19` on everything under `src/` (`.github/workflows/scripts/clang-format.sh`), and no trailing
  whitespace in `src`, `*.md`, `*.txt`, `*.yml`.
- `reuse lint`: every file needs an SPDX header. Files that cannot carry one (JSON, binaries) go in `REUSE.toml`.
- Style (`CONTRIBUTING.md`): 100 columns, `PascalCase` functions/classes, `lower_case_underscored` variables
  and files, no new external dependencies in Core, no C-style casts.
- `CONTRIBUTING.md` requires **disclosing AI use** in upstream PRs and says descriptions and comments must be
  human-written. Raise this with the user before proposing a change to `shadps4-emu/shadPS4`.

## Running

`shadps4 CUSA00001` (looks the game up in the install dirs) or `shadps4 /path/to/eboot.bin`; `--help` lists
flags. Running with no arguments only shows a message box. User data lives in a `user/` folder:
`config.json`, `custom_configs/<serial>.json`, `patches/`, `log/shad_log.txt`. Games need firmware `.sprx`
modules from a real console in `sys_modules`.

## Architecture

**Boot and guest code.** `main.cpp` parses the CLI and calls `Emulator::Run` (`emulator.cpp`), which mounts
the game, loads settings, then loads the ELF and modules through `core/linker` and `core/module`. Guest x86-64
code runs **natively on the host**. It is not interpreted. `core/cpu_patches.cpp` and `core/signals.cpp`
patch or trap what the host cannot run as is. PS4 OS and system libraries are reimplemented
as HLE under `core/libraries/*` and registered with `LIB_FUNCTION(nid, lib, version, module, fn)`. Guest
calls resolve by NID to those host functions.

**Settings.** `core/emulator_settings.{h,cpp}` holds grouped settings (`General`, `GPU`, `Vulkan`, ...), each a
`Setting<T>` with a global `value` and an optional `game_specific_value`. `Load(serial)` clears the previous
game overrides, applies built-in per-game defaults (`FindBuiltInGameConfig`), then `custom_configs/<serial>.json`
on top. Only fields registered in a group's `GetOverrideableFields()` can be overridden per game. Anything
that changes generated shader code must also be part of `Shader::Profile` (`shader_recompiler/profile.h`),
otherwise the on-disk pipeline cache serves stale shaders.

**GPU path.** Guest submits PM4 command buffers through `core/libraries/gnmdriver` to `video_core/amdgpu/liverpool`,
which parses them on a GPU thread and drives `renderer_vulkan` (`vk_rasterizer` binds resources and issues
draws; `vk_pipeline_cache` compiles and caches pipelines). Guest GCN shaders go through
`shader_recompiler` (frontend decode/translate to IR, `ir/passes`, SPIR-V backend via sirit). Flips go
through `videoout`, and `GfxFlip`/`GfxEop` interrupts are raised from Liverpool.

**Memory the GPU reads.** `video_core/buffer_cache` backs guest ranges with sparse "arena" buffers.
With `direct_memory_access_enabled` shaders reach guest memory through a BDA page table, and
`FaultManager` makes pages the shader touched resident on first access. Dynamic `ReadConst` loads are only valid with DMA on. CPU
writes are tracked per page by `MemoryTracker`/`RegionManager` (`page_manager` write-protects guest
memory), and `SynchronizeDmaBuffers` re-uploads CPU-modified pages.

**Patches.** `common/memory_patcher.cpp` applies XML patches from `user/patches` (`bytes`, `mask`,
`mask_jump32`). Patches are version-pinned by `AppVer` except pattern-based ones.

## Uncharted: The Nathan Drake Collection

Work in progress, not playable yet. `documents/Uncharted-NDC.md` covers the built-in settings
(DMA, red zone patching, pipeline cache) and the EOP-assert patch in `documents/patches/uncharted-ndc/`. The
remaining crash (`Device lost` after the first cutscene) needs logs from a real run. The branch
`uncharted-validation` has an unmerged change to when Liverpool signals the flip interrupt.

## Workflow preference

The owner wants PRs opened against this fork merged automatically once the build and tests pass and the PR
is mergeable with no failing checks. The fork has no CI, so verify locally first.
