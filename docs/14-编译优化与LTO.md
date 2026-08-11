# 14 - 编译优化与 LTO

> 目标平台：Raspberry Pi Zero 2 W（Cortex-A53，armv7 hard-float）。
> 目标：在不改游戏逻辑的前提下，靠编译期优化榨性能。默认随 `RE3_RPI` 开启，
> 桌面/CI 不受影响，且只在非 Debug 生效。

---

## 1. 开启的选项

两个 CMake 选项，默认值绑定 `RE3_RPI`：

- **`RE3_LTO`** — 链接期优化 `-flto=auto`，通过 `CMAKE_INTERPROCEDURAL_OPTIMIZATION`
  开启。设 `CMP0069 NEW` 让它同时覆盖 vendored **librw**（渲染热路径）。GCC 下把
  `ar/ranlib/nm` 路由到 `gcc-ar/gcc-ranlib/gcc-nm` 包装器，保证静态库里的 LTO
  段不被丢弃（交叉工具链同样适用）。
- **`RE3_EXTRA_OPT`** — 从 lv_gba_emu 借鉴、但**只保留利于运行速度**的一组：
  - `-fno-math-errno`：libm 调用不设 errno
  - `-ffunction-sections -fdata-sections` + 链接 `-Wl,--gc-sections`：死代码消除，
    利于 I-cache、二进制更小
  - `-fno-stack-protector -fno-ident`：去掉少量开销

架构调优（`-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a53`）
在工具链文件 `rpi-armhf-toolchain.cmake` 里，Release 基线是 `-O3 -DNDEBUG`。

## 2. 刻意**不**抄的选项

lv_gba_emu 那套里有一批是**体积优先**的，会拖慢速度或破坏链接，明确排除：

- `-falign-functions=1 -falign-jumps=1 -falign-loops=1`：缩小对齐是为体积，伤流水线。
- `-fno-unroll-loops`：禁止循环展开，直接伤速度。
- `-fwhole-program`：假设全部代码在一个编译单元，re3 要链 librw/SDL/OpenAL，会出错。
- `-Ofast`（= `-O3 -ffast-math`）：改变浮点语义，物理/渲染有风险；只取其中安全的
  `-fno-math-errno`。

## 3. 严重回归：LTO + strict aliasing 导致过场动画人物头消失

### 现象
开启 LTO 后，过场动画（cutscene）里人物的**头不见了**。普通 `-O3`（无 LTO）正常。

### 根因
re3 是**逆向工程代码**，充斥类型双关 / 严格别名（strict aliasing）违规——上游其实
知道，但只是**关掉警告**（`src/CMakeLists.txt` 里的 `-Wno-strict-aliasing`），
并没有关掉编译器的 aliasing **假设**。

- 普通 `-O3`：编译器在单个翻译单元内较保守，这些违规大多“侥幸”能工作。
- **LTO**：跨模块优化会**激进地利用 strict-aliasing 假设**（认为不同类型的指针
  不会指向同一内存），从而重排 / 消除本该发生的读写，把潜在的 UB 变成**真实的
  miscompile**。cutscene 头部（CutsceneHead 的 frame/骨骼/可见性等基于类型双关的
  访问）正好中招。

### 关键点：警告查不出来
这类问题**不会**产生有用的编译警告——它是“合法假设下的合法优化”，只是假设不成立。
所以“检查 LTO 警告”找不到元凶（本次构建仅有几个无关的 `-Wstringop-overflow` /
`-Walloc-size` 噪声）。判断要靠**理解 LTO 的语义前提**，而非看 warning。

### 修复
开 LTO 时无条件加 **`-fno-strict-aliasing`**：

```cmake
add_compile_options(-fno-strict-aliasing)
```

它禁止编译器做“不同类型指针不别名”的假设，正是 re3 这种靠**实际内存布局**工作的
逆向代码所需要的。运行期代价可忽略（只是少了一类激进假设），但保证 LTO 对 re3
安全。上游用 `-Wno-strict-aliasing` 只是“闭嘴”，我们用 `-fno-strict-aliasing`
才是“真正安全”。

## 4. 结果

- 帧率有提升（实测游戏内约 mid-30s ~ low-40s FPS，随场景波动）。
- 二进制更小（`--gc-sections` + LTO 去死代码）。
- 加 `-fno-strict-aliasing` 后过场动画恢复正常。

## 5. 经验

- **LTO 对逆向 / 类型双关重的代码是高风险优化**，务必配 `-fno-strict-aliasing`。
- 抄别人的优化 flag 要**按目标（速度 vs 体积）筛选**，不能整套照搬。
- miscompile 类问题**不看 warning 看语义前提**：先问“这个优化假设了什么，我的代码
  满不满足这个假设”。

## 6. 严重回归：LTO 导致存档崩溃 / 系统卡死 → 默认关闭 LTO（2026-08）

### 现象
开启 LTO 的构建，在**存档**时出现崩溃/卡死：

- 一次表现为**整机网络不可达**（ping `Destination Host Unreachable`、SSH 连不上），
  即**系统级 hang**（不只是 re3 进程死循环——普通进程死循环不会让内核网络栈停摆），
  需要硬重启。
- 一次表现为进程 **crash**。

**关掉 LTO 编译的完全相同版本（同 -O3、同 EXTRA_OPT、仅 `RE3_LTO=OFF`）稳定**，
存档不再崩。据此判定是 **LTO 引入的 miscompile**。

### 与 strict-aliasing 的关系
本项目此前已为 LTO 加了 `-fno-strict-aliasing`（见上文，修过过场动画头消失的
miscompile）。**但即便如此，LTO 版在存档路径仍不稳定**——说明 LTO 在这里触发了
另一类 miscompile（尚未定位到具体函数）。存档路径本身的全量 LTO 警告是干净的
（`grep -i warning` 无 save/GenericGame/MemoryCard/Pools 相关项），所以**编译期
警告查不出来**，和之前的经验一致：miscompile 类问题不看 warning 看行为。

存档流程 `GenericSave`/`GenericLoad` 大量使用 `work_buff`(55000B) + `memcpy` 宏 +
指针步进 + 手工对齐，正是 LTO 跨模块别名/生命周期分析最容易出错、也最容易把
「潜在 UB」放大成「真实内存损坏」的地方；一旦写坏关键内存，后果可从进程崩溃升级到
系统级卡死。

### 决定：默认关闭 LTO
`CMakeLists.txt` 的 `RE3_LTO` 默认值由 `RE3_RPI` 改为 **OFF**。LTO 转为**可选**
（`-DRE3_LTO=ON` 显式开启），在根因定位前不作为默认发布配置。

- **保留** `RE3_EXTRA_OPT`（默认随 RE3_RPI 开）：`-fno-math-errno` /
  function-data-sections + `--gc-sections` / `-fno-stack-protector -fno-ident`
  与架构调优都与本次崩溃无关，安全。
- 代价：失去 LTO 的那部分收益（更小的二进制 + 跨模块内联）。但**稳定性优先**——
  一个会崩存档、甚至拖垮系统的优化不能作为默认。

### 教训
- **LTO 对逆向 / 类型双关 / 手工缓冲密集的代码是高风险优化**：即使加了
  `-fno-strict-aliasing`，仍可能在别处 miscompile。
- 收益（帧率、体积）要与稳定性权衡；**默认发布配置以稳定为先，激进优化设为 opt-in**。
- 若要重新启用 LTO，必须先用带符号的 LTO 版复现存档崩溃 + coredump/日志定位到
  具体被 miscompile 的函数，再针对性隔离（如给该翻译单元关 LTO），而非全局开。
