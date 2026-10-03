<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Uncharted: The Nathan Drake Collection

Serials: `CUSA02320` (US), `CUSA02343` (EU), `CUSA02344` (EU/RU), `CUSA02826`.

Status: reached the main menu and the first Uncharted 1 cutscene, then hit `Device lost`.
Upstream [shadps4-emu/shadPS4#5140](https://github.com/shadps4-emu/shadPS4/pull/5140), whose change
is included here, traced that crash and the `m_gfxEopTick` assert to the flip interrupt being raised
before the frame's EOP fence was written. Testers reported Uncharted 1 in-game without `Device lost`
with it. See also [shadps4-emu/shadPS4#5038](https://github.com/shadps4-emu/shadPS4/issues/5038).

## What the emulator does for this game

Everything below is built in and applies automatically to the serials above. You don't need a
custom config, a patch file or an internet connection.

| Setting | Why |
|---|---|
| `GPU.direct_memory_access_enabled = true` | Compute shaders read constants through dynamic addresses. Without DMA they read garbage loop bounds and the GPU hangs about 10 seconds after boot. |
| `GPU.async_pipeline_compilation = true` | New graphics pipelines are compiled on worker threads and their draws are skipped until they are ready, instead of stalling the GPU thread. Expect a few missing objects for a moment when a new effect first appears. |
| `GPU.readbacks_mode = Precise` | The game reads on the CPU results the GPU wrote, to decide what to draw. With readbacks off the CPU never sees them and the world is missing. Costs some speed. |
| `General.redzone_patches = true` | Windows only. Windows exception dispatch overwrites data that a game function keeps below the stack pointer, which crashes the main menu. |
| `Vulkan.pipeline_cache_enabled = true` | Keeps compiled pipelines between runs, so later runs start with them already built. |

To override any of them, set the key in `user/custom_configs/<serial>.json`. That file always
takes precedence over the built-in values.

The game also asserts that the GPU has already finished an older frame (`m_gfxEopTick`) when
gameplay starts. The emulator can fall behind while it compiles shaders, so the check fails even
though the frame finishes shortly after. When the game loads, the emulator finds that check in
`eboot.bin`, verifies the instructions, and skips the assert. It works on every game version and
changes nothing unless exactly one matching site is found. Look for
`Patched EOP tick assert` in `shad_log.txt`. `--ignore-game-patch` disables it.

The first run still compiles every shader. Runs after that load the pipeline cache at startup and
are smoother.

## Device lost report

If the GPU driver reports `VK_ERROR_DEVICE_LOST`, the emulator writes
`user/log/device_lost_report.txt` before it stops. It lists the serial, the GPU and driver, the
active GPU settings, the pipeline hash and shader hashes of the last 256 draws and dispatches
(with whether they used DMA), and the `VK_EXT_device_fault` information when the driver offers it.
The draws are recorded when the commands are recorded, not when the GPU runs them, so the faulting
one can be older than the newest entries.

## Reporting results

Attach `user/log/shad_log.txt` (and `user/log/device_lost_report.txt` if it exists) and include:

- game version (`APP_VER` in `sce_sys/param.sfo`) and region;
- GPU and driver version;
- how far the game got, and whether this was the first or a later run (pipeline cache).
