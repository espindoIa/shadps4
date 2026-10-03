<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Uncharted: The Nathan Drake Collection

Serials: `CUSA02320` (US), `CUSA02343` (EU), `CUSA02344` (EU/RU), `CUSA02826`.

Status: boots to the main menu and plays the first Uncharted 1 cutscene. It is **not playable**
yet: a `Device lost` error still follows the first cutscene. See
[shadps4-emu/shadPS4#5038](https://github.com/shadps4-emu/shadPS4/issues/5038).

## Settings

These settings are applied automatically for the serials above. You don't need a custom config:

| Setting | Why |
|---|---|
| `GPU.direct_memory_access_enabled = true` | Compute shaders read constants through dynamic addresses. Without DMA they read garbage loop bounds and the GPU hangs about 10 seconds after boot. |
| `General.redzone_patches = true` | Windows only. Windows exception dispatch overwrites data that a game function keeps below the stack pointer, which crashes the main menu. |
| `Vulkan.pipeline_cache_enabled = true` | Keeps compiled pipelines between runs. Fewer compilation stalls mean the GPU thread is less likely to fall behind the game (see the patch below). |

To override any of them, set the key in `user/custom_configs/<serial>.json`. That file always
takes precedence over the built-in values.

The first run compiles every shader and stutters. Runs after that load the pipeline cache at
startup and are smoother.

## Patch for the EOP tick assert

When gameplay starts, the game asserts that the GPU has already finished an older frame
(`m_gfxEopTick`). The emulator can fall behind while it compiles shaders, so this check fails
even though the frame finishes shortly after.

1. Copy the `documents/patches/uncharted-ndc` folder into `user/patches/`. You should end up
   with `user/patches/uncharted-ndc/files.json`.
2. Start the game normally. Patches load automatically.
3. Check `shad_log.txt` for `Applied patch: Remove GPU EOP tick assert`.

The enabled entry only applies to game version `01.00` of `CUSA02320`; the emulator skips it on
other versions. For other versions, there is an experimental pattern-based entry. To use it,
set `isEnabled="true"` on the second `Metadata` element. It has not been verified against the
game binary. If the log shows `PatternScan failed`, the pattern does not match your version and
nothing was changed. For other regions, also add your serial to `files.json`.

## Reporting results

Attach `user/log/shad_log.txt` and include:

- game version (`APP_VER` in `sce_sys/param.sfo`) and region;
- GPU and driver version;
- how far the game got, and whether this was the first or a later run (pipeline cache).
