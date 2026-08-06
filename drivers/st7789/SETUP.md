# ST7789 SPI — Raspberry Pi Provisioning

One-time system setup for the ST7789 SPI display. Run once per fresh OS image;
`setup_pi.sh` handles everything below and is safe to re-run.

## Quick start

```bash
sudo ./setup_pi.sh            # bufsiz=65536, core_freq=400 (Pi Zero 2 W)
sudo reboot                   # required for SPI/bufsiz/core_freq
```

After reboot, verify:

```bash
gcc -O2 example.c st7789.c pi_gpio.c -o st7789_example
./st7789_example 300          # expect ~76 fps at 320x240
```

## What it configures (and why)

| # | Setting | Where | Why (lesson learned) |
|---|---|---|---|
| 1 | `dtparam=spi=on` | config.txt | Creates `/dev/spidev0.0`. Without it there is no SPI device. |
| 2 | `spidev.bufsiz=65536` | cmdline.txt | Default is **4096**. A single transfer larger than bufsiz fails with EMSGSIZE. A fresh image resets this — the #1 gotcha. |
| 3 | `core_freq=400` + `core_freq_min` | config.txt | Real SPI clock = `core_freq / even divisor`. Unlocked core_freq drifts, so the actual SPI rate wanders. |
| 4 | CPU governor = `performance` | systemd unit `st7789-governor.service` | **The single biggest perf factor.** The default `ondemand` governor downclocks the CPU during SPI DMA idle, cutting throughput ~40% (76 → 51 fps). See docs/06 §5.6. |
| 5 | user in `gpio`,`spi` groups | usermod | Root-free access to `/dev/gpiomem` and `/dev/spidev`. |

## Gotchas this prevents

- **Black screen / no backlight after a reimage.** Root cause was the driver's
  default 32K chunk exceeding the reset `bufsiz=4096`, so `st7789_flush` failed
  before anything displayed. The driver now clamps chunk to bufsiz automatically,
  and this script restores bufsiz=65536 for full speed.
- **Frame rate mysteriously stuck ~50 fps.** That's the `ondemand` governor.
  The systemd unit pins `performance` on every boot.
- **SPI speed not matching the requested Hz.** core_freq quantization; e.g.
  requesting 60MHz yields 400/8=50MHz. Request values that divide core_freq by an
  even number (400/4=100MHz is the practical target).

## Manual equivalent (if you prefer not to run the script)

```bash
# /boot/firmware/config.txt  (single settings, one per line)
dtparam=spi=on
core_freq=400
core_freq_min=400

# /boot/firmware/cmdline.txt  (MUST remain a single line — append to the end)
... spidev.bufsiz=65536

# CPU governor (runtime; make persistent via the systemd unit in setup_pi.sh)
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# groups
sudo usermod -aG gpio,spi "$USER"

sudo reboot
```

## Reverting

`setup_pi.sh` backs up `config.txt` and `cmdline.txt` to `*.st7789.bak` on first
run. To revert:

```bash
sudo mv /boot/firmware/config.txt.st7789.bak  /boot/firmware/config.txt
sudo mv /boot/firmware/cmdline.txt.st7789.bak /boot/firmware/cmdline.txt
sudo systemctl disable --now st7789-governor.service
sudo rm /etc/systemd/system/st7789-governor.service
sudo reboot
```
