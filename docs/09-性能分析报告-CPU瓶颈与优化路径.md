# 09 - 性能分析报告：CPU 瓶颈与优化路径

> 目标平台：Raspberry Pi Zero 2 W（BCM2837，4× Cortex-A53 @ 1.0GHz，V3D GPU rev=2）
> 测试场景：GTA3 re3，分辨率 320×240，DrawDistance=0.8，帧率 ~31fps

---

## 1. 执行环境

| 项目 | 值 |
|---|---|
| 机型 | Raspberry Pi Zero 2 W Rev 1.0 |
| CPU | 4× ARM Cortex-A53 @ 1.0GHz（单核跑满） |
| GPU | V3D rev=2，3 slices × 1 QPU/slice = 3 QPUs，6 TMUs |
| 内存 | 512MB LPDDR2 |
| OS | Raspbian 13 (trixie)，内核 6.12.75+rpt-rpi-v7 |
| SPI 屏 | ST7789 320×240，66.7MHz（core_freq 400/6） |
| CPU governor | performance（固化，见 drivers/st7789/setup_pi.sh） |
| DrawDistance | 0.8（优化后，原为 1.2） |

---

## 2. 帧时间分解（[perf] log，120帧平均）

```
[perf] 320x240 cpu=24ms gpu=7ms read=0.67ms present=0.18ms dc=556 | frame=32ms 31fps
```

| 阶段 | 时间 | 说明 |
|---|---|---|
| CPU（rsIDLE 主线程） | **24ms** | 游戏逻辑 + GL命令录制 + Mesa CPU工作 |
| GPU（glFinish 等待） | **7ms** | V3D 最后几个 job 完成前的等待 |
| glReadPixels | 0.67ms | GLES→CPU 帧缓冲读回 |
| SPI present | 0.18ms | 双缓冲后台推屏（实际推屏~19ms 已被隐藏） |
| **帧总计** | **32ms** | **31fps** |

---

## 3. GPU 利用率分析（V3D 硬件计数器，vc4_gpustat 实测）

工具：`tools/vc4_gpustat/vc4_gpustat.c`，直接读 V3D PCTR 寄存器（/dev/mem mmap）。

**稳定 gameplay 下（4次×2s采样，结果高度一致）：**

| 指标 | 值 | 含义 |
|---|---|---|
| QPU 总周期 | ~3.5B/2s | V3D @ 300MHz × 2s × 3 QPU |
| **QPU 空闲率** | **49%** | GPU 一半时间在等 CPU 喂数据 |
| QPU 片段着色 | 28% | 纹理采样 + 光照计算 |
| QPU 顶点着色 | 6% | 顶点变换（较低） |
| QPU TMU 等待 | 14% | 纹理读取延迟 |
| QPU SB 等待 | 3% | 寄存器 WAW/WAR 依赖 |
| TMU cache miss | 0.2% | 纹理缓存命中率极高 |
| **L2 cache miss** | **60%** | L2 缺失率高（带宽压力） |
| TLB 像素写出 | 12 Mpx/s | 等效约 320×240@155fps 的填充率 |
| 深度剔除率 | 0% | **early-Z 完全未生效** |

**结论：GPU 空闲 49%，CPU 是瓶颈。** GPU 填充率（12 Mpx/s）远超当前帧率所需（320×240×31fps = 2.38 Mpx/s），填充率不是限制。L2 miss 率 60% 值得关注（可能来自 uniform buffer 频繁更新），但不是主要瓶颈。

---

## 4. CPU 侧热点分析（perf flamegraph，300Hz DWARF 采样，30s）

见 `docs/re3_flame.svg`。

**re3 主线程热点：**

| 函数 | 占比 | 含义 |
|---|---|---|
| libgallium（Mesa VC4驱动） | ~40% | GPU 命令录制 + DRM ioctl |
| memcpy | ~14% | 帧缓冲翻转（fill_buf in sink_spi） |
| draw_char（HUD）| ~4% | 位图字体绘制（可优化） |
| libc（malloc等）| ~4% | 内存分配 |

**ALSoftP1 音频线程（独立，不计入 cpuMs）：**

| 线程 | CPU% |
|---|---|
| re3 主线程 | 82%（跑满一核） |
| ALSoftP1 | 10% |
| 流媒体线程 | 2% |

---

## 5. GPU 提交路径分析（ftrace 实测）

**每帧提交模型：**

```
620 次 glDrawElements/glDrawArrays
  → Mesa VC4 攒批 → 3 次 vc4_submit_cl_ioctl
    第1次（长）: ~3ms（含等待上一帧 render job 完成）
    第2次（短）: ~65us
    第3次（短）: ~90us
  → glFinish: 等最后一个 job，~3ms
```

- Mesa **把 620 draw call 攒成 3 次内核提交**，批量效率极高
- 每次提交后 GPU 立刻异步开始执行，CPU 不等（`vc4_queue_submit` fire-and-forget）
- CPU-GPU **已经并行**：GPU 在 CPU 处理 draw call 的 24ms 内就在渲染，`glFinish` 等的只是最后几个未完成 job 的尾部
- 当前 GPU 空闲 49%，说明 GPU 跑完 3 个 job 后在等 CPU 准备下一帧

---

## 6. 与 2001 年 PC 硬件的对比

| | 2001年主流 PC | Pi Zero 2 W |
|---|---|---|
| CPU | Pentium 4 1.5GHz（NetBurst，IPC低） | Cortex-A53 1.0GHz（IPC 明显高于 P4） |
| GPU | GeForce 3（硬件 T&L，专用 shader） | V3D（软件 T&L，QPU shader） |
| GTA3 帧率 | 30-50fps @ 800×600 低画质 | **31fps @ 320×240** |
| GPU 几何处理 | 硬件 T&L（专用管线） | CPU 侧 Mesa 软件 T&L（libgallium 40%） |
| 瓶颈 | GPU fill rate（GeForce 3 时代）| CPU（Mesa GPU command building） |

**关键差异**：V3D 的"软件 T&L"使得 libgallium 占主线程 40% CPU。GeForce 3 有专用的几何处理硬件，CPU 只负责 DrawPrimitive 调用；Pi 的 V3D 需要 CPU 在 Mesa 里做大量工作才能生成 GPU 命令列表。这就是为什么 A53 理论上比 P4 强，但实测帧率相当。

---

## 7. 多核利用可行性评估

**当前状态：**
- re3 主线程吃满 1 核（82% 单核）
- 4 核总 CPU 利用率约 25%（top 显示）
- 另外 3 个核几乎空闲

**re3 游戏逻辑的多核化挑战：**

GTA3 的游戏逻辑是严格串行的单线程设计（源自 PS2 时代），关键路径依赖顺序执行：

```
每帧: [CGame::Process] → [RenderScene] → [glFinish] → [ReadPixels] → [SPI]
         ↑ AI/物理/脚本        ↑ GL提交       ↑ GPU等待    ↑ 读回
```

- `CGame::Process` 包含全局状态修改（交通、NPC、碰撞），难以并行
- `RenderScene` 下的 draw call 录制理论可并行，但 OpenGL 不是线程安全的

**可行的多核优化路径：**

| 方案 | 收益 | 难度 | 说明 |
|---|---|---|---|
| **SPI 推送异步化** | 已完成（+~5fps） | ✅ 已实现 | sink_spi 双缓冲后台线程 |
| **音频线程降优先级** | 小（~1fps） | ✅ 已实现 | alsoft rt-prio=0 |
| **glReadPixels 异步化** | ~2fps | 中 | PBO（Pixel Buffer Object），Pi GLES2 有限支持 |
| **CGame::Process 多线程** | 理论 ~5-8fps | 很高 | 需要大量锁或无锁重构，风险高 |
| **draw call 并行录制** | 理论 ~3-5fps | 高 | 需要 command buffer per-thread，librw 不支持 |
| **把 CPU 超到 1.2GHz** | ~+5fps（线性） | 低 | `arm_freq=1200 over_voltage=6`，官方支持范围内 |

**最推荐的低风险多核方向：**

1. **CPU 超频到 1.2GHz**：在 `/boot/firmware/config.txt` 加 `arm_freq=1200 over_voltage=6`，Pi Zero 2 W 官方允许，可稳定跑。线性提升约 20%，预计从 31fps → 37fps。
2. **glReadPixels PBO 异步化**：把 0.67ms 的 readback 和下一帧 GPU 渲染重叠，小幅改善。

---

## 8. Shader 优化空间

当前 shader 代码问题（见 `vendor/librw/src/gl/shaders/`）：

| 问题 | 影响 | 建议 |
|---|---|---|
| `discard` 禁用 early-Z | 中（GPUstat 确认 depth cull=0%） | 对不需要 alpha test 的不透明物体走 `NO_ALPHATEST` path |
| `DoDynamicLight` 动态循环+分支 | 小 | QPU 硬件执行所有分支，分支不省周期；可改用预乘顶点色 |
| skin shader 64骨骼动态索引 | 小 | GTA3 ped 实际用≤20骨骼，改小数组减少 uniform 搬运 |
| 深度剔除率 0%（GPU stat 确认） | 待查 | 可能是 GTA3 无 pre-Z pass；对 opaque geometry 加 depth prepass 可提升 early-Z 效率 |

---

## 9. 优化效果汇总

| 优化项 | 改善 | 状态 |
|---|---|---|
| DrawDistance 1.2→0.8 | +7fps（24→31fps） | ✅ 已实施 |
| CPU governor=performance | 最关键（SPI 49→78fps@100MHz） | ✅ 已固化 |
| alsoft.ini 音频优化 | ALSoftP1 CPU 降低 | ✅ 已提交 |
| SPI 双缓冲后台推送 | present 时间从 ~19ms 隐藏 | ✅ 已实施 |
| SPI 安全频率 66.7MHz | 屏幕稳定无花屏 | ✅ 已确认 |
| **待实施：CPU 超频到 1.2GHz** | **预计 +5-6fps** | ⬜ 低风险 |
| 待实施：`NO_ALPHATEST` opaque pass | 未量化，需实测 | ⬜ 中等 |

---

## 10. 工具和数据位置

| 工具/文件 | 位置 |
|---|---|
| HUD overlay（FPS/CPU/GPU/DC/freq/temp） | `src/skel/gbm/hud_overlay.cpp` |
| SPI push driver | `drivers/st7789/` |
| V3D 硬件计数器工具 | `tools/vc4_gpustat/vc4_gpustat.c`（需 sudo） |
| SPI 基准测试 | `tools/spi_bench/` |
| flamegraph（30s 300Hz DWARF） | `docs/re3_flame.svg` |
| 内核源码（vc4/v3d 驱动） | `spike/linux-vc4/drivers/gpu/drm/vc4/` |
| SPI 系统配置脚本 | `drivers/st7789/setup_pi.sh` |
| OpenAL 优化配置 | `gamefiles/alsoft.ini` |
