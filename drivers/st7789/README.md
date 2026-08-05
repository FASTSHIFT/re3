# ST7789 SPI Display Driver (Raspberry Pi)

Formal, self-contained user-space driver for pushing an RGB565 framebuffer to an
ST7789 panel over SPI. Ready for API integration (e.g. re3 → offscreen render →
`st7789_flush`).

Backends: GPIO via `/dev/gpiomem` (root-free), SPI via `spidev`.

## Files

| File | Purpose |
|---|---|
| `st7789.h` | Public API (the only header integrators include) |
| `st7789.c` | Driver implementation |
| `pi_gpio.h/.c` | Fast GPIO backend (based on FASTSHIFT/pi_gpio, `/dev/gpiomem`) |
| `example.c` | Usage example / fps smoke test |
| `CMakeLists.txt` | Builds `libst7789.a` (+ example) |

## Build

```bash
cmake -B build -S . && cmake --build build
# or directly:
gcc -O2 example.c st7789.c pi_gpio.c -o st7789_example
```

Integrators: link the `st7789` CMake target, or add `st7789.c` + `pi_gpio.c` to
your build and include `st7789.h`.

## Quick start

```c
#include "st7789.h"

st7789_config_t cfg;
st7789_config_default(&cfg);      // 320x240 landscape, /dev/spidev0.0, 100MHz
// cfg.width = 240; cfg.height = 240;   // override if needed

st7789_tune_system(65536);        // best-effort: performance governor (key!)

st7789_t* dev = st7789_open(&cfg);
int w = st7789_get_width(dev), h = st7789_get_height(dev);

uint16_t* fb = malloc(w * h * sizeof(uint16_t));
// ... render RGB565 into fb ...
st7789_flush(dev, fb);            // full frame
// or st7789_flush_area(dev, x, y, rw, rh, subfb);  // dirty rect

st7789_close(dev);
```

## Wiring (BCM GPIO, defaults)

| Signal | Pin |
|---|---|
| RST | 27 |
| CS  | 8 (CE0) |
| DC  | 25 |
| BLK | 24 |
| SCLK | 11 (SPI0) |
| MOSI | 10 (SPI0) |

CS is driven by GPIO; SPI runs with `SPI_NO_CS`.

## Performance notes (see docs/06 for the full analysis)

- Measured **~76 fps at 320x240** (100MHz SPI, performance governor).
- **The key knob is the CPU governor.** With the default `ondemand` governor the
  CPU downclocks during SPI DMA idle, cutting throughput ~40%. `st7789_tune_system`
  sets `performance`; make it persistent (rc.local / systemd) for production.
- Requesting 100MHz: the real SPI clock is `core_freq / even-divisor`. With
  core_freq=400 the usable steps are 400/4=100MHz, 400/6=66.7MHz, 400/8=50MHz.
  Lock `core_freq` in config.txt to avoid drift.
- `chunk_bytes` default 32768 (avoids the DMA-lite 32K limit; equal throughput to
  64K). Enlarge kernel `spidev.bufsiz` (boot cmdline) if you push single
  transfers larger than the default 4096.
- 100MHz exceeds the ST7789 datasheet nominal (~62MHz). It works on the bus here,
  but verify visual stability on the actual panel/ribbon before shipping.

## Prerequisites on the Pi

- `dtparam=spi=on` in config.txt (creates `/dev/spidev0.0`).
- User in the `gpio` and `spi` groups (root-free operation).

## Integration hand-off (for API layer)

1. Render your scene to an offscreen RGB565 buffer at the panel resolution
   (recommend matching the panel, e.g. 240x240, to save GPU fill and SPI bytes).
2. Call `st7789_flush` once per frame, or `st7789_flush_area` for dirty rects.
3. Handle the `<0` return (SPI error) as needed.
4. Endianness: the driver sets the panel to little-endian by default
   (`cfg.little_endian=1`) so a native-endian `uint16_t` RGB565 buffer maps
   directly; flip if your pixel source differs.
