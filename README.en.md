[简体中文](README.md) | **English** | [Original upstream README (backup)](README.upstream.md)

# re3 · Raspberry Pi ST7789 Handheld Port & Performance Optimization

> A port of the community GTA III reverse-engineering project
> [re3](README.upstream.md) to a **Raspberry Pi Zero 2 W + 2" ST7789 SPI
> display** (Waveshare GamePi20 form factor), with a set of performance
> optimizations, input adaptations, and Simplified Chinese localization aimed
> at low-end hardware.

This repository **contains only reverse-engineered source code plus my own
port/optimization changes — no game assets**. Running it requires that you
**own a legitimate copy of GTA III**. See the [Disclaimer](#disclaimer) below.

---

## What this is

Upstream re3 rewrites GTA III as portable, compilable C++ source. This branch
builds on it so the game can:

- run on a **Pi Zero 2 W (4×Cortex-A53 @1GHz, 512MB)**;
- render **without a desktop/HDMI**, via GBM + EGL surfaceless GLES2 offscreen,
  pushing frames to a **320×240 ST7789 SPI panel**;
- take input from **GPIO physical buttons / evdev keyboard-mouse-gamepad** (with
  hotplug);
- display **Simplified Chinese** (reusing re3's Japanese CJK font pipeline with
  a widened atlas geometry).

The target is a handheld, but the code is platform-decoupled and should port
easily to faster boards (Pi 4 / Pi 5 / others) — see
[docs/08 Platform Decoupling](docs/08-平台解耦方案.md).

---

## Performance work

Full analysis and numbers live under `docs/`; the highlights:

- **Offscreen render path**: GBM + EGL surfaceless GLES2 → `glReadPixels`
  straight into **RGB565** (half the bandwidth, native panel format) → push to
  panel. See [docs/07](docs/07-脱离桌面离屏渲染验证.md).
- **ST7789 SPI push**: **double-buffered + background SPI thread** — the main
  thread fills one frame while the background thread flushes the previous one;
  SPI clock set to a measured-stable 66.7MHz. See
  [docs/06](docs/06-ST7789-SPI推送优化分析.md).
- **Lean post-processing (RPI_LEAN_FX)**: fill-rate/bandwidth-heavy fullscreen
  effects (screen rain droplets, colour filter, motion blur) are off by default,
  fixed at compile time so they can't silently re-enable. See
  [docs/05](docs/05-画质选项与性能影响.md).
- **LTO + speed flags on by default for release**: `-flto=auto` (covers the
  vendored librw) plus `-fno-math-errno`, `-ffunction/-fdata-sections +
  --gc-sections`, and A53 tuning (`-mtune=cortex-a53 -mfpu=neon-vfpv4`). Enabled
  with `RE3_RPI`.
- **Memory**: disable background services a handheld doesn't need; tune the
  zram (zstd) swap. See [docs/13](docs/13-内存占用分析与优化.md).
- **Profiling**: built-in per-stage timers (TIMEBARS / TIMEBARS_LOG) and an
  on-screen perf HUD to pin down CPU/GPU bottlenecks. See
  [docs/03](docs/03-性能分析与Profiler方案.md),
  [docs/09](docs/09-性能分析报告-CPU瓶颈与优化路径.md),
  [docs/04](docs/04-GPU瓶颈深度分析.md).

---

## New features

- **ST7789 SPI output backend** (`RE3_OUTPUT_SPI`) plus a standalone ST7789
  driver (`drivers/st7789/`).
- **Pluggable input/output backends**: output = SPI / fbdev / SDL window
  (desktop debug); input = GPIO buttons / evdev / SDL. Multiple input sources can
  be active at once. See [docs/08](docs/08-平台解耦方案.md).
- **Input device hotplug**: evdev watches `/dev/input` via netlink uevent
  (keyboard, mouse, gamepad, Bluetooth PS5 pad). See
  [docs/10](docs/10-输入设备热插拔设计.md).
- **Handheld pad mapping** (GamePi20 GPIO buttons):
  - on foot ABXY = camera look (right stick); in vehicle/menus = normal face
    buttons;
  - R shoulder: on foot hold-to-aim, double-tap to cycle weapon; in vehicle aim;
  - L shoulder: on foot fire, **horn while driving**;
  - **B is the handbrake in a vehicle**; START pauses and toggles the perf HUD.
- **On-screen perf HUD** (front/back split: the sampling backend keeps fds
  persistent, the render frontend only formats + draws): FPS, per-stage CPU/GPU
  timings, memory usage, ARM/V3D clocks, temperature.
- **Simplified Chinese localization**: full GXT translation + a Chinese font
  atlas reusing the Japanese CJK pipeline. Tools in `tools/`, design in
  [docs/11](docs/11-中文字体与GXT本地化方案.md),
  [docs/12](docs/12-中文翻译批处理交接说明.md).

> Forgot a detail? The `docs/` directory (01–13) has the full analysis, designs,
> and gotchas.

---

## Building (Raspberry Pi cross-compile)

You need an armhf cross toolchain (`arm-linux-gnueabihf-gcc/g++`) and a sysroot
synced from the Pi; the toolchain file is `rpi-armhf-toolchain.cmake`.

```bash
cmake -S . -B build-gbm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$(pwd)/rpi-armhf-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBRW_PLATFORM=GL3 -DLIBRW_GL3_GFXLIB=GBM \
  -DRE3_AUDIO=OAL -DRE3_RPI=ON \
  -DRE3_OUTPUT_SPI=ON -DRE3_INPUT_EVDEV=ON \
  -DRE3_CHINESE=ON
cmake --build build-gbm
```

`RE3_RPI=ON` auto-enables LTO and the speed flags. Copy `build-gbm/src/re3` to
the Pi, drop it into your legitimate GTA III directory, and run.

For other platforms (desktop / Windows / macOS, etc.), see the
[original upstream README](README.upstream.md).

---

## Docs index (`docs/`)

| # | Topic |
|---|---|
| 01 | Current-state analysis & port plan |
| 02 | Phase-1 bring-up log |
| 03 | Performance analysis & profiler approach |
| 04 | GPU bottleneck deep-dive |
| 05 | Graphics options & performance impact |
| 06 | ST7789 SPI push optimization |
| 07 | Desktop-free offscreen rendering validation |
| 08 | Platform decoupling (pluggable input source + render output) |
| 09 | Performance report: CPU bottleneck & optimization path |
| 10 | Input device hotplug design |
| 11 | Chinese font & GXT localization |
| 12 | Chinese translation batch handoff |
| 13 | Memory usage analysis & optimization |

(Docs are written in Chinese.)

---

## About the re3 project itself

re3 is a community effort started in 2018 to rewrite GTA III into C++ source
function by function; the sister project reVC covers GTA: Vice City, with the
homebrew render layer librw. For the full project origin, credits, and upstream
improvement list, see the [original upstream README](README.upstream.md).

---

## Disclaimer

This project is for **personal technical study and exchange**. It is a Raspberry
Pi port and performance optimization of the community GTA III reverse-engineering
project (re3).

- This repository **contains only source code, build scripts, and port patches
  written by me — no Grand Theft Auto III game assets** (art, audio, maps,
  scripts, fonts, etc.).
- Running it **requires that you own a legitimate copy of GTA III** and use its
  game files. This project **does not distribute the game or any copyrighted
  assets**; please do not request or share such content.
- *Grand Theft Auto III* and all its assets are the property of **Rockstar Games
  / Take-Two Interactive**. I am not affiliated with, authorized by, or endorsed
  by them.
- This content is for **study, research, and non-commercial exchange** only.
  Please delete related files after testing. You assume all responsibility for
  any consequences of using this project.
- If the rights holder considers this content inappropriate, please contact me
  and I will comply immediately (takedown / removal).

**If you plan to port to a Pi 4 / Pi 5 / another board**: you're welcome to, but
please follow the same rule — share code and changes only, **no game assets, no
commercial use**.

---

## License

Following upstream's stance: this code is for **educational, documentation, and
modding purposes** only. It does not encourage piracy or commercial use; please
keep derivative work open source and give proper credit. See the
[License section of the original upstream README](README.upstream.md#license).
