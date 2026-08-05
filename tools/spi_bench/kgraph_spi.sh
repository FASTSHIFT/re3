#!/bin/bash
# kgraph_spi.sh - 用 function_graph 追踪 bcm2835 SPI DMA 路径，定位 per-transfer 开销
# 需 root。
set -e
HZ=${1:-100000000}
SIZE=${2:-32768}
DEV=${3:-/dev/spidev0.0}

T=/sys/kernel/debug/tracing
[ -d /sys/kernel/tracing ] && T=/sys/kernel/tracing

echo 0 > $T/tracing_on
echo > $T/trace
echo 0 > $T/events/enable

# 只追 spi/dma 相关函数，降低噪声
echo function_graph > $T/current_tracer
echo > $T/set_ftrace_filter
for f in bcm2835_spi_transfer_one bcm2835_spi_prepare_sg bcm2835_spi_dma_tx_done \
         bcm2835_spi_dma_rx_done spi_transfer_one_message __spi_pump_messages \
         spi_sync spi_sync_locked dma_async_issue_pending vchiq_ignore \
         bcm2835_spi_prepare_message spidev_ioctl spidev_message; do
  echo $f >> $T/set_ftrace_filter 2>/dev/null || true
done

echo funcgraph-abstime > $T/trace_options 2>/dev/null || true
echo funcgraph-proc > $T/trace_options 2>/dev/null || true

echo 1 > $T/tracing_on
/home/pi/e2e/spi_trig "$DEV" "$HZ" "$SIZE" 5
echo 0 > $T/tracing_on

cat $T/trace | grep -vE '^#' | head -120
