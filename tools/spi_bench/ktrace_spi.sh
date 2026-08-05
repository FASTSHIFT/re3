#!/bin/bash
# ktrace_spi.sh - 用内核 SPI tracepoints 测量单次 transfer 在内核里的真实耗时
# 需 root。抓 spi_transfer_start -> spi_transfer_stop 的时间差。
#
# 用法: sudo ./ktrace_spi.sh <spi_hz> <xfer_size> <device>
set -e
HZ=${1:-100000000}
SIZE=${2:-32768}
DEV=${3:-/dev/spidev0.0}

T=/sys/kernel/debug/tracing
[ -d /sys/kernel/tracing ] && T=/sys/kernel/tracing

# 复位
echo 0 > $T/tracing_on
echo nop > $T/current_tracer
echo > $T/trace
# 只开 spi transfer start/stop
echo 0 > $T/events/enable
echo 1 > $T/events/spi/spi_transfer_start/enable
echo 1 > $T/events/spi/spi_transfer_stop/enable
echo 1 > $T/tracing_on

# 触发少量传输（用已编译的 spi_gap 的单尺寸模式不方便，直接用 dd 到 spidev 不行，
# 用一个最小 C 触发器，由调用方先编译好 trigger）
/home/pi/e2e/spi_trig "$DEV" "$HZ" "$SIZE" 20

echo 0 > $T/tracing_on
# 输出：只保留 spi 事件
grep -E 'spi_transfer_(start|stop)' $T/trace | head -60
