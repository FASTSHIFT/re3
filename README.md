<!-- LANG-SWITCH --> **简体中文** | [English](README.en.md) | [原始上游 README（备份）](README.upstream.md)

# re3 · 树莓派 ST7789 掌机移植与性能优化版

> 基于开源社区的 GTA III 逆向工程项目 [re3](README.upstream.md)，将其移植到
> **Raspberry Pi Zero 2 W + 2 寸 ST7789 SPI 屏**（Waveshare GamePi20 形态），
> 并针对低端硬件做了一系列性能优化、输入适配和简体中文本地化。

本仓库**只包含逆向出的源码与本人的移植/优化改动，不含任何游戏资源**。运行需要
你**自备一份正版 GTA III**。详见文末[免责声明](#免责声明)。

---

## 这是什么

原版 re3 把 GTA III 完整逆向重写为可跨平台编译的 C++ 源码。本分支在其之上，让
游戏能够：

- 在 **Pi Zero 2 W（4×Cortex-A53 @1GHz，512MB）** 上跑起来；
- **脱离桌面/HDMI**，用 GBM + EGL 无窗口离屏渲染，再把画面推到 **320×240 的
  ST7789 SPI 屏**；
- 用 **GPIO physical buttons / evdev 手柄键鼠**（支持热插拔）作为输入；
- 显示**简体中文**（复用 re3 的日文 CJK 字体管线，加宽图集几何）。

目标平台是掌机，但代码是平台解耦的，理论上很容易移植到性能更好的 Pi 4 / Pi 5 /
其他开发板（见 [docs/08 平台解耦方案](docs/08-平台解耦方案.md)）。

---

## 性能优化方案

具体分析和数据都在 `docs/` 里，这里只列要点：

- **离屏渲染链路**：GBM + EGL surfaceless GLES2 → `glReadPixels` 直接读成
  **RGB565**（省一半带宽，且是屏的原生格式）→ 推屏。
  见 [docs/07](docs/07-脱离桌面离屏渲染验证.md)。
- **ST7789 SPI 推送**：**双缓冲 + 后台 SPI 线程**，主线程填一帧的同时后台推上一
  帧；SPI 时钟调到实测稳定的 66.7MHz。见 [docs/06](docs/06-ST7789-SPI推送优化分析.md)。
- **精简后处理（RPI_LEAN_FX）**：默认关闭吃填充率/带宽的全屏拷贝特效（屏幕雨滴、
  颜色滤镜、运动模糊），编译期固化以防被重新打开。见
  [docs/05](docs/05-画质选项与性能影响.md)。
- **发布构建默认开启 LTO + 提速编译选项**：`-flto=auto`（覆盖 vendored librw）
  加 `-fno-math-errno`、`-ffunction/-fdata-sections + --gc-sections` 等；A53 上还
  带 `-mtune=cortex-a53 -mfpu=neon-vfpv4` 硬浮点。默认随 `RE3_RPI` 开启。
- **内存优化**：关闭掌机用不到的后台服务、zram（zstd）swap 调优。见
  [docs/13](docs/13-内存占用分析与优化.md)。
- **性能剖析工具**：内建 per-stage 计时（TIMEBARS / TIMEBARS_LOG）与屏上性能
  HUD，用于定位 CPU/GPU 瓶颈。见 [docs/03](docs/03-性能分析与Profiler方案.md)、
  [docs/09](docs/09-性能分析报告-CPU瓶颈与优化路径.md)、
  [docs/04](docs/04-GPU瓶颈深度分析.md)。

---

## 新增特性

- **ST7789 SPI 屏输出后端**（`RE3_OUTPUT_SPI`）+ 独立的 ST7789 驱动
  （`drivers/st7789/`）。
- **可插拔输入/输出后端**：输出可选 SPI / fbdev / SDL 窗口（桌面调试）；输入可选
  GPIO 物理按键 / evdev 键鼠手柄 / SDL。多源可同时生效。见
  [docs/08](docs/08-平台解耦方案.md)。
- **输入设备热插拔**：evdev 通过 uevent 监听 `/dev/input` 的插拔（键鼠、手柄、
  蓝牙 PS5 手柄）。见 [docs/10](docs/10-输入设备热插拔设计.md)。
- **掌机手柄映射**（GamePi20 GPIO 按键）：
  - 徒步时 ABXY 控制视角（右摇杆），车内/菜单为正常面键；
  - 右肩键：徒步长按瞄准、双击切武器，车内瞄准；
  - 左肩键：徒步开火，**车内按喇叭**；
  - **B 键车内为手刹**；START 暂停并翻转性能 HUD。
- **屏上性能 HUD**（前后端分离：采集后端持久化 fd，渲染前端只格式化+绘制）：
  FPS、CPU/GPU 分段耗时、内存占用、ARM/V3D 频率、温度。
- **简体中文本地化**：全量翻译 GXT + 中文字体图集，复用日文 CJK 管线。工具在
  `tools/`，方案见 [docs/11](docs/11-中文字体与GXT本地化方案.md)、
  [docs/12](docs/12-中文翻译批处理交接说明.md)。

> 想不起来某个细节？`docs/` 目录（01–13）里有完整的分析、方案和踩坑记录。

---

## 构建（树莓派交叉编译）

需要 armhf 交叉工具链（`arm-linux-gnueabihf-gcc/g++`）和一份从 Pi 同步的
sysroot；工具链文件见 `rpi-armhf-toolchain.cmake`。

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

`RE3_RPI=ON` 会自动开启 LTO 与提速编译选项。把产物 `build-gbm/src/re3` 拷到 Pi
上、放进你的正版 GTA III 目录即可运行。

其他平台（桌面 / Windows / macOS 等）的原始构建方式见
[原始上游 README](README.upstream.md)。

---

## 文档索引（`docs/`）

| 编号 | 主题 |
|---|---|
| 01 | 现状分析与移植计划 |
| 02 | 阶段一跑通记录 |
| 03 | 性能分析与 Profiler 方案 |
| 04 | GPU 瓶颈深度分析 |
| 05 | 画质选项与性能影响 |
| 06 | ST7789 SPI 推送优化分析 |
| 07 | 脱离桌面离屏渲染验证 |
| 08 | 平台解耦方案（输入源 + 渲染输出可插拔）|
| 09 | 性能分析报告：CPU 瓶颈与优化路径 |
| 10 | 输入设备热插拔设计 |
| 11 | 中文字体与 GXT 本地化方案 |
| 12 | 中文翻译批处理交接说明 |
| 13 | 内存占用分析与优化 |

---

## 关于 re3 项目本身

re3 是社区从 2018 年开始、把 GTA III 逐函数逆向重写成 C++ 源码的项目，姊妹项目
reVC 对应 GTA: Vice City，配套自研渲染层 librw。完整的项目起源、致谢与上游改进
列表，请见 [原始上游 README](README.upstream.md)。

---

## 免责声明

本项目为**个人技术学习与交流用途**，基于开源社区的 GTA III 逆向工程项目（re3）
进行树莓派平台的移植与性能优化。

- 本仓库**仅包含本人编写的源代码、构建脚本与移植补丁，不包含任何《侠盗猎车手 III》
  （Grand Theft Auto III）的游戏资源**（美术、音频、地图、剧本、字库等）。
- 运行本项目**需要你自行拥有一份正版 GTA III** 并使用其游戏文件。本项目**不提供
  游戏本体或任何受版权保护资源的下载**，也请勿索取或分享此类资源。
- 《Grand Theft Auto III》及其全部素材版权归 **Rockstar Games / Take-Two
  Interactive** 所有。本人与其无任何关联，亦未获其授权或认可。
- 本内容仅供**学习、研究与非商业交流**。请于测试后自行删除相关文件。因使用本项目
  产生的任何后果由使用者自行承担。
- 如版权方认为本内容不妥，请联系我，我将立即配合处理（下架 / 删除）。

**若你打算移植到 Pi 4 / Pi 5 / 其他开发板**：欢迎交流，但请同样遵守——只分享代码
与改动，**不分享游戏资源，不商业化**。

---

## 许可

沿用上游立场：本代码仅用于**学习、文档与 modding 目的**，不鼓励盗版或商业使用，
衍生作品请保持开源并注明出处。详见 [原始上游 README 的 License 段](README.upstream.md#license)。
