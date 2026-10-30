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
- **`no_timestamps` extended to this G52 (r34p0).** The quirk existed for
  Mali-G615 ("timestamp queries never resolve"); this driver has the same
  failure — a 60 s profile created both query pools and armed them, and not
  one per-pass or whole-cb GPU time came back while the counter-free pass
  histogram logged every second. Without the quirk the profiler silently
  produces no timing data instead of none at all.

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
Camera-measured (player A/B, clean windows): **internal view 12.4 fps,
external view ~48.7 fps** — both submitting the same ~1000 draws / ~37k
verts per FRAME, GPU near-fully busy in both (77 ms vs 20 ms of work per
frame). The cockpit pays ~58 ms/frame in extra per-pixel work.

Per-frame counter diff (internal vs external, from the session logs):
**+560 draws, +5 target clears, +7 semaphore syncs, +9 traps per frame** —
a whole extra scene render plus its own clear-bounded targets, exactly the
shape of a rear-view mirror pass rendered from a second camera. Per-pass
GPU timing would name it directly, but this driver's timestamp queries
never resolve (see the no_timestamps quirk below), so the pass-level
profiler has never produced data on this device — every profile so far was
frame-level only. The generic lever for a pixel-bound title on this GPU is
below; a per-title mod that kills the cockpit's extra scene render remains
the surgical fix.

### Render scale 0.5x (new): the pixel-bound lever

The app's render scale was integer 1-4x — the one value this GPU needs for
heavy titles (½x) was impossible. It now runs 0.5x-4x: the scale is held in
**eighths of a guest pixel** end to end (`g_hle_rscale`, `HleVkSurf.scale`),
so ½x renders every target at half extent with all guest↔host rectangle
math, scissor/viewport, point sizes, linear-texture UV normalization,
zread halves and readback/down-blit paths sharing the same denominator.
Small render-to-texture targets stay exact 1x as before — and a surface
sampled by texel is now divided by **its own** scale in the shaders
(flags' top byte holds a 2-bit code a stage), not the device's: the two
differ for guest-resolution surfaces, which is why Sega GT's cars (64x64
1x skin targets) first lost their textures at 0.5x. Set per game:
**game's settings → Resolution scale → 0.5x** (a quarter of the pixels;
Sega GT 2002 ships with it preset in this build's app data).

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
