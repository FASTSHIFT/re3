#!/usr/bin/env bash
#
# setup_pi.sh - One-shot Raspberry Pi provisioning for the ST7789 SPI display.
#
# Idempotent: safe to re-run. Configures everything the driver needs and the
# performance knobs discovered during profiling (see docs/06 and README.md):
#   1. Enable SPI (dtparam=spi=on) in config.txt          -> /dev/spidev0.0
#   2. Persist spidev.bufsiz in the kernel cmdline        -> large SPI transfers
#   3. Lock core_freq so the SPI clock divisor is stable  -> no clock drift
#   4. Persist CPU governor = performance via systemd     -> no DMA-idle downclock
#   5. Add the user to the gpio/spi groups                -> root-free operation
#
# A reboot is required for cmdline/config/core_freq changes to take effect.
# The governor service takes effect immediately (and on every boot).
#
# Usage:  sudo ./setup_pi.sh [bufsiz] [core_freq]
#   bufsiz     default 65536
#   core_freq  default 400   (Pi Zero 2 W; usable SPI = core_freq / even divisor)
#
# SPDX-License-Identifier: MIT

set -euo pipefail

BUFSIZ="${1:-65536}"
CORE_FREQ="${2:-400}"

if [[ $EUID -ne 0 ]]; then
    echo "Please run as root: sudo $0" >&2
    exit 1
fi

# --- Locate boot files (new Bookworm/Trixie layout vs legacy) ---
if [[ -f /boot/firmware/config.txt ]]; then
    BOOT=/boot/firmware
else
    BOOT=/boot
fi
CONFIG="$BOOT/config.txt"
CMDLINE="$BOOT/cmdline.txt"
echo "Using boot dir: $BOOT"

backup() { [[ -f "$1" && ! -f "$1.st7789.bak" ]] && cp "$1" "$1.st7789.bak" && echo "  backed up $1 -> $1.st7789.bak" || true; }

# --- 1. Enable SPI ---
echo "[1/5] Enabling SPI..."
backup "$CONFIG"
if grep -qE '^\s*dtparam=spi=on' "$CONFIG"; then
    echo "  SPI already enabled"
else
    # replace a commented/disabled line if present, else append
    if grep -qE '^\s*#?\s*dtparam=spi=' "$CONFIG"; then
        sed -i -E 's/^\s*#?\s*dtparam=spi=.*/dtparam=spi=on/' "$CONFIG"
    else
        echo 'dtparam=spi=on' >> "$CONFIG"
    fi
    echo "  set dtparam=spi=on"
fi

# --- 2. Persist spidev.bufsiz in cmdline (must stay a single line) ---
echo "[2/5] Setting spidev.bufsiz=$BUFSIZ..."
backup "$CMDLINE"
if grep -qE 'spidev\.bufsiz=[0-9]+' "$CMDLINE"; then
    sed -i -E "s/spidev\.bufsiz=[0-9]+/spidev.bufsiz=$BUFSIZ/" "$CMDLINE"
    echo "  updated existing spidev.bufsiz"
else
    # append to the end of the (single) line, trimming trailing whitespace first
    sed -i -E "s/[[:space:]]*\$/ spidev.bufsiz=$BUFSIZ/" "$CMDLINE"
    echo "  appended spidev.bufsiz=$BUFSIZ"
fi
if [[ $(wc -l < "$CMDLINE") -ne 1 ]]; then
    echo "  WARNING: cmdline.txt is not a single line; please check $CMDLINE" >&2
fi

# --- 3. Lock core_freq (SPI clock = core_freq / even divisor) ---
echo "[3/5] Locking core_freq=$CORE_FREQ..."
if grep -qE '^\s*core_freq=' "$CONFIG"; then
    sed -i -E "s/^\s*core_freq=.*/core_freq=$CORE_FREQ/" "$CONFIG"
else
    echo "core_freq=$CORE_FREQ" >> "$CONFIG"
fi
if grep -qE '^\s*core_freq_min=' "$CONFIG"; then
    sed -i -E "s/^\s*core_freq_min=.*/core_freq_min=$CORE_FREQ/" "$CONFIG"
else
    echo "core_freq_min=$CORE_FREQ" >> "$CONFIG"
fi
echo "  core_freq / core_freq_min = $CORE_FREQ"

# --- 4. Persist CPU governor = performance (systemd service) ---
echo "[4/5] Installing performance-governor service..."
cat > /etc/systemd/system/st7789-governor.service <<'EOF'
[Unit]
Description=Set CPU governor to performance for ST7789 SPI push
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g"; done'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now st7789-governor.service >/dev/null 2>&1 || true
echo "  governor now: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"

# --- 5. Group membership (root-free /dev/gpiomem + /dev/spidev) ---
echo "[5/5] Adding user to gpio/spi groups..."
TARGET_USER="${SUDO_USER:-pi}"
for grp in gpio spi; do
    if getent group "$grp" >/dev/null; then
        usermod -aG "$grp" "$TARGET_USER" && echo "  $TARGET_USER added to $grp"
    fi
done

echo
echo "Done. Summary:"
echo "  SPI enabled       : $(grep -E '^\s*dtparam=spi=on' "$CONFIG" >/dev/null && echo yes || echo NO)"
echo "  spidev.bufsiz      : $(grep -oE 'spidev\.bufsiz=[0-9]+' "$CMDLINE" || echo unset)"
echo "  core_freq          : $(grep -oE '^core_freq=[0-9]+' "$CONFIG" || echo unset)"
echo "  governor (runtime) : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
echo
echo ">>> REBOOT required for SPI/bufsiz/core_freq changes to take effect. <<<"
echo "    sudo reboot"
