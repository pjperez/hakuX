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
| Sega GT 2002 | 50 fps (50 cap) | **~12 fps in races** | GPU: ~1,700 strip draws/frame, unjoined |
| Halo CE | 51 fps | not yet measured | — |
| Colin McRae 2005 | 48 fps | not yet measured | — |
| Doom 3 | 50 fps | not yet measured | — |
| Halo 2 | 29 fps (30 cap) | not yet measured | — |
| Deus Ex: IW | 25 fps | 25 fps | CPU (FEX JIT of Unreal Warfare) |
| Beyond Good & Evil | 37 fps first run | not yet measured | cold caches on first run |

Races in Sega GT push 13k–23k draws/s (~1,700/frame) with the Bifrost
no-descriptor-indexing path binding state per draw: ~80–120 ms/frame.

## Roadmap

### Strip batching without ExtendedDynamicState2 — the big one

The back end already joins short same-state strips into one indexed draw with
primitive restart (`hw/xbox/hle/vk/batch.c`; Blinx: ~500k strip draws/s
joined). On this GPU it never runs:

```
strip batching off: no dynamic primitive restart
```

`hle_batch_init` gates it on `caps.eds2` (`VK_EXT_extended_dynamic_state2`,
absent on Mali r34) because the joined draw enables restart dynamically.
**Primitive restart for strips is core Vulkan 1.0 via the pipeline's
`primitiveRestartEnable`** — no dynamic state needed. Plan: build strip
pipelines with restart on (a restart index that never appears is harmless to
unjoined strips), relax the gate, measure Sega GT races.

### Then

- Per-draw descriptor work on the no-indexing path (`vk/draw.c` set1/pc
  counts in the emit stats) — the next slice of per-draw overhead.
- Deus Ex is CPU-bound in the guest; nothing GPU-side helps it.
- Upstream: the two quirks above are PR-ready with their measurements.
