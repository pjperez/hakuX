# hakuX t618 — the RG Vita branch

Device-targeted fork of [rfandango/hakuX](https://github.com/rfandango/hakuX)
for the **Anbernic RG Vita**: Unisoc Tiger T618 (2× Cortex-A75 @ 2.0 GHz +
6× A55), **Mali-G52 MP2** (Bifrost, Vulkan **1.1**, Arm driver r34p0-01eac0),
Android 12, 3 GB RAM.

Everything here is measured on that hardware; the device stays connected over
adb and every change is A/B'd with the emulator's own instrumentation
(`debug.hakux.perf`, the hitch monitor, the per-second pushbuffer stats).

## Build

The stock build is generic `armv8-a`, which makes every atomic read-modify-write
an out-of-line libatomic call. This branch builds with:

```
cmake -S native -B build/native/core -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DNC_DEPS=<native-deps> \
  -DHAKUX_ARCH=armv8.2-a \
  -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -mtune=cortex-a75" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -mtune=cortex-a75"
```

`armv8.2-a` is safe on both the A75 and A55 (both have LSE; both lack nothing
it uses) and gives the core and FEX inline LSE atomics — verified: 207 inline
LSE instructions in libhakuxcore.so, zero out-of-line atomic calls.
Deliberately NOT `+dotprod`/`+rcpc`: the T618's A55 may lack them.

## What's on this branch

### 1. Arm driver quirks (`hw/xbox/hle/vk/quirks.c`)

- **`present_cpu_wait` off for Arm's driver.** Qualcomm's workaround cost
  620–730 ms/s of CPU in the presenter on this GPU; Arm's driver needs none
  (0.3 ms/s measured, rendering identical).
- **`no_sem_wait` (new).** `vkWaitSemaphores` (the host wait on the timeline
  semaphore) returned `VK_ERROR_DEVICE_LOST` mid-game across titles — the
  "graphic driver stopped responding" error, several times per session. The
  counter query keeps working, so `hle_tl_sem_wait` polls
  `vkGetSemaphoreCounterValue` in userspace instead. Zero device losses in
  ~40 min of testing after; measured cost nil (7 timeline waits in a whole
  race session).

## Device performance ledger (measured, 2026-10-30)

| Game | Menus / attract | Gameplay | Bound by |
|---|---|---|---|
| Sega GT 2002 | 50 fps (50 cap) | **13.3 fps in races** | GPU workload: `gpu=963ms/s`, ~74 ms/frame across ~55 cbs — the game's own 640x720 internal passes, shadows and 688 copies/s |
| Halo CE | 51 fps | not yet measured | — |
| Colin McRae 2005 | 48 fps | **48.7 fps measured in a race** (59k draws/s!) | light per-draw work |
| Doom 3 | 50 fps | not yet measured | — |
| Halo 2 | 29 fps (30 cap) | not yet measured | — |
| Deus Ex: IW | 25 fps | 25 fps | CPU (FEX JIT of Unreal Warfare) |
| Beyond Good & Evil | 37 fps first run | not yet measured | cold caches on first run |

Sega GT's races are the clearest case of pixel-workload bound on this GPU:
Colin McRae pushes 2.7x the draws (59k/s) at 3.7x the framerate (48.7 vs
13.3), so draw count is not the limit — the race's own render targets are.
Player-measured: the **exterior race camera runs visibly faster** than the
in-car/close views — the car's env-mapped reflective paint is the expensive
pixel work, and it fills the screen exactly when the camera is near or in
the car. Draw-path work cannot fix that; a per-title mod (the emulator's
mods system) that kills or downgrades the car-paint reflection passes is
the remaining lever.

## Roadmap

### Strip batching without ExtendedDynamicState2 — DONE, and extended

The back end joins short same-state strips into one indexed draw with
primitive restart (`hw/xbox/hle/vk/batch.c`), but on this GPU it never ran
(`strip batching off: no dynamic primitive restart`). Two changes landed:

1. The restart comes from the **pipelines** (`primitiveRestartEnable`, core
   Vulkan 1.0 for strips) when `VK_EXT_extended_dynamic_state_2` is absent.
2. **Indexed strip runs** (`hle_batch_draw_idx`): titles that draw strips
   indexed (Sega GT 2002 races: ~23k indexed strips/s) never reached the
   batch before — only unindexed strips (Blinx's shape) did. Their 16-bit
   arena indices are copied into the run rebased to its vertexOffset.

Measured: Sega GT 2002's heavy scenes (the game that drew ~23k indexed
strips a second in races) join 430 batches/s of 865 draws, rendering
identically. Runs whose draws carry a vertex binding between them stay one
draw — a join across different vertex buffer offsets needs the offset delta
folded into the indices and the bind recording suppressed (next step).

### The next bottleneck, now measured

The same scenes run ~27k draws/s with **~12k pipeline and ~11.6k descriptor
binds a second** — per-draw state churn, the Bifrost no-descriptor-indexing
tax. Strip joining shaves the draws; the state binds are the bigger cost and
the real remaining ceiling for this GPU.

### Then

- Join across differing vertex buffer offsets (offset delta folded into the
  rebase, bind recording suppressed for held draws).
- Deus Ex is CPU-bound in the guest; nothing GPU-side helps it.
- Upstream: the two driver quirks are PR-ready with their measurements; the
  eds2-free batching is a candidate once it has more device mileage.
