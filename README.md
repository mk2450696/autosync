# AutoPacer v10 — Software G-Sync for iGPU display + dGPU render setups

## What this solves

You have:
- Monitor connected to **motherboard HDMI** (Intel UHD 730 output)
- Games rendering on **RTX 3060 Ti** via CASO Tier 3 passthrough
- Frame generation (OptiScaler / DLSS Enabler / native FSR FG) causing **tearing**
- VRR active on Intel output but unable to sync with NVIDIA's FG pipeline

**Root cause of every previous failure:** Intel owns the scanout timing. NVIDIA generates frames asynchronously. With direct GPU output, G-Sync lets the GPU dictate scanout timing. You don't have that. Every previous fix (SyncInterval=1 → judder; burst catcher alone → still tears; VBlank relay on render thread → tanks FPS) addressed symptoms not the cause.

**v10 solution:** A dedicated VBlank relay thread calls `WaitForVBlank()` on Intel's IDXGIOutput (found by matching the primary monitor handle, confirmed iGPU). It posts one semaphore slot per VBlank. The Present hook waits on that semaphore before calling Present with SyncInterval=0. The render thread is never blocked by WaitForVBlank itself — only by the semaphore wait (which is ~0ms when VBlank is coming right up). This is software G-Sync.

## Architecture

```
[NVIDIA render thread]          [VBlank relay thread]
  renders frame                   WaitForVBlank(IntelOutput)
  EnforceBurstThreshold()  <---   ReleaseSemaphore(1 slot)
  WaitForSingleObject(sem)  <---- [fires at Intel VBlank]
  oPresent(sc, 0, flags)
  [Intel scans out frame at VBlank — no tearing]
```

## Settings

**RTSS:** Close it completely. Do not use alongside AutoPacer — they both hook Present and will conflict.

**DLSS Enabler / OptiScaler vsync:** OFF. AutoPacer handles sync. If you leave mod vsync ON, you get double VBlank waits = half your FPS.

**NVCP:** vsync = Off, no frame cap set (or set a ceiling like 158fps max if you want a hard ceiling)

**Display:** 165Hz, VRR on (in Intel Graphics software)

**FG multiplier:** Any — 2x, 3x, 4x, 5x. AutoPacer's burst threshold (6.2ms) prevents burst frames from colliding within a single VBlank period.

## Installation

1. Build via GitHub Actions (push to your repo, download artifact) — gets you `AutoPacer.asi`
2. Place `AutoPacer.asi` in your game folder (same directory as the game .exe)
3. You need an ASI loader. If the game already uses one (Script Hook, etc.) it's already there. Otherwise use **Ultimate ASI Loader**: download `dinput8.dll` from its releases and place it alongside the .asi.
4. Launch the game. You should hear **one beep** when the hook installs successfully.

## Tuning

In `dllmain.cpp`, top of file:

```cpp
static constexpr double BURST_THRESHOLD_MS  = 6.2;   // Raise if tearing persists; lower if latency feels high
static constexpr DWORD  VBLANK_WAIT_TIMEOUT = 50;     // Semaphore wait timeout in ms (fallback if VBlank thread stalls)
static constexpr bool   ENABLE_BEEP         = true;   // Set false to disable startup beep
```

If tearing **persists:** increase `BURST_THRESHOLD_MS` to 6.5 or 7.0.  
If **latency feels high:** decrease to 6.0 (minimum safe for 165Hz).  
If you switch to **144Hz mode:** change to 7.2 (one VBlank at 144Hz = 6.94ms).

## Why this is different from every previous version

| Version | Failure mode |
|---------|-------------|
| v1-v7 | Various feedback loops, measured own output, wrong GPU VBlank |
| v8 | Burst threshold 5.5ms < VBlank period 6.06ms → frames still collide |
| v9 | SyncInterval=1 + Intel's VRR not supported → fixed 165Hz → judder |
| RTSS FES | Works but: high latency, conflicts with hooks, can't coexist with mods |
| **v10** | Decoupled VBlank thread + semaphore + SyncInterval=0 = software G-Sync |

## Debugging

Check **DebugView** (Sysinternals) while running — AutoPacer logs:
- `Found Intel output: adapter=Intel(R) UHD Graphics 730, output=0` — good
- `VBlank relay thread started` — good  
- `SUCCESS: AutoPacer v10 active` — good
- `Could not find Intel output` — check primary display assignment in Windows Display Settings

## Known limitations

- Does not replicate G-Sync feel 1:1 — the semaphore has ~0.1ms jitter vs hardware
- At very high FG multipliers (5x+), interpolation variance itself causes micro-judder that this cannot fix (it's not a sync issue, it's bad interpolated frames)
- If your game uses exclusive fullscreen, CASO may not be active — use Borderless Windowed
