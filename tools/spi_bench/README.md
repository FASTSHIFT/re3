# SPI / ST7789 推送性能测试工具

配合 `docs/06-ST7789-SPI推送优化分析.md`。用于定位树莓派 SPI 推屏帧率瓶颈。

## 核心结论（TL;DR）

60fps @ 320×240 需 9.216 MB/s（73.7 Mbit/s）。实测瓶颈**不是** SPI 硬件、GPIO、ioctl 拆分或 bufsiz，而是 **CPU `ondemand` governor 在 SPI DMA 空闲期降频**。

- 默认 ondemand：100MHz SPI 只有 7.24 MB/s（49 fps）
- 切 `performance`：11.61 MB/s（79 fps），端到端 76.8 fps ✅

内核 tracepoint 证明 SPI DMA 传输本身效率 97%。

## 文件

| 文件 | 用途 |
|---|---|
| `spi_bench.c` | 基础吞吐基准，扫不同 chunk 大小 |
| `spi_profile.c` | 线性回归分离"每次固定开销"与"每字节线速率" |
| `spi_gap.c` | 测 polling(<96B) vs DMA(≥96B) 的 clk/byte |
| `spi_multi.c` | 对比 N 次 ioctl / 批量 ioctl(N) / 单大块 |
| `spi_trig.c` | 固定 size 触发器，供内核 tracepoint 抓取 |
| `st7789_e2e.c` | **端到端**：复用参考驱动 st7789.c，黑白双 buffer 切换测帧率 |
| `pi_gpio.c/.h` | 快速 GPIO 库（基于 FASTSHIFT/pi_gpio，改用 /dev/gpiomem 免 root） |
| `wiringPi_shim.c` + `wiringPi*.h` | wiringPi 兼容层，让参考驱动无需系统 wiringPi 即可跑 |
| `ktrace_spi.sh` | 内核 SPI tracepoint：测 transfer_start→stop 净传输时间 |
| `kgraph_spi.sh` | function_graph：追 bcm2835_spi_* 定位 per-transfer 开销 |

## 编译（树莓派本机 gcc）

```bash
# 端到端测试（复用参考驱动，需 rpi/st7789.c）
gcc -O2 -w -DHAVE_WIRING_PI -I. \
    st7789_e2e.c rpi/st7789.c wiringPi_shim.c pi_gpio.c -lpthread -o st7789_e2e

# 独立基准
gcc -O2 -w -o spi_bench spi_bench.c
gcc -O2 -w -o spi_multi spi_multi.c
```

## 运行

```bash
# 先设 performance governor（关键！）
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
# 调大 bufsiz
sudo modprobe -r spidev && sudo modprobe spidev bufsiz=65536

# SPI_HZ 覆盖驱动硬编码的 60MHz
SPI_HZ=100000000 ./st7789_e2e 300

# 内核打点（需 root）
sudo ./ktrace_spi.sh 100000000 32768
sudo ./kgraph_spi.sh 100000000 32768
```

接线（BCM GPIO，与 lv_gba_emu 一致）：RST=27, CS=8(CE0), DC=25, BLK=24, SCLK=11, MOSI=10。
