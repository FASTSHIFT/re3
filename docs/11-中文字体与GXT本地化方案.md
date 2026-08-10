# 11 - 中文字体与 GXT 本地化方案

## 目标

让 re3 在 Pi(及桌面)上显示简体中文，且：

- 只需维护**翻译文本**（真 Unicode），不需要手工码位映射。
- 对 Pi **零运行时开销**（沿用原版的纹理图集渲染，不做运行时光栅化）。
- 与上游 re3 字体系统的既有 CJK 先例（日文 `FONTJAP`）保持一致，改动可控。

## 背景：re3 字体系统（源码实证）

re3 的字体是**纹理图集（atlas）**，非矢量（见 `src/renderer/Font.cpp`）：

- 每个字符 = 图集里一个固定格子。`CFont::PrintChar` 用码位算格子坐标：
  - 西文：`xoff = c % 16, yoff = c / 16`（16 列图集 `font2`）。
  - 日文：`xoff = c % 48`（48 列大图集 `FONTJAP`），码位直接作图集索引。
- 字宽表 `Size[...]`（西文 193 项）/ `Size_jp[]`（日文）做比例间距。
- GXT 文本是 `wchar`(16-bit)，`CText` 原样加载不转换（`CData::Load`），
  即 **GXT 里的 16-bit 值就是字体图集索引**。原版 english.gxt 里 'Y'=0x59
  直接对应图集第 0x59 格。

**日文分支（`MORE_LANGUAGES` + `IsJapanese()`）就是现成的 CJK 模板**：一张大
图集 + 码位当索引 + 独立字宽表 + 独立 TXD（`FONTS_J.TXD`）。中文照此扩展即可。

## 方案选型

### 方案 A：离线预渲染字体图集（选定）

离线用 TTF 把用到的汉字光栅化成一张大图集纹理，打进 TXD；re3 侧新增中文字体
分支，码位→格子索引，运行时和原版一样只是贴图。

- 翻译产物：只需提供真 Unicode 中文文本。
- 映射：离线脚本自动分配“字符→图集格子”，同时产出 GXT 用的码位与字宽表。
- 运行时开销：零（纯贴图），适合 Pi。
- 字号：预渲染时定死；320×240 屏幕不需要高分辨率字模，图集很小即可。

### 方案 B：运行时集成 TTF（stb_truetype）——不选

运行时按需光栅化 + 动态 atlas 缓存。改动大（要接管 CFont 字形来源、动态上传
GPU 纹理），首次渲染有卡顿，Pi CPU 紧张不划算。我们这个场景（固定小屏、固定
字号、有限字数）用不上它的“任意字号/全字库”优点。

**结论：TTF 只在离线环节用于光栅化字模，不进 re3 运行时。**

## 架构设计（方案 A）

### 码位分配策略

只对翻译文本里**实际出现**的汉字建图集（按需子集，通常几百到数千字），而非全
字库。分配规则：

- 保留 ASCII/西文走原有 `font2` 图集不变（码位 < 0x100 时仍用西文字体）。
- 汉字从一个**私有起始码位**开始顺序编号（如 0x0100 起，避开 ASCII），编号即
  图集格子索引。这个映射由离线脚本生成，GXT 和字宽表都用它，前后端一致。
- 记录“真 Unicode 汉字 ↔ 分配码位”表，供 GXT 打包和调试复用。

### 离线工具链

```
翻译文本(key -> 中文串, 真 Unicode)
        │
        ▼  tools/gen_cn_font.py  (+ 一个开源 TTF: 文泉驿/思源)
        ├── chinese.gxt              标准 GTA3 GXT，码位=图集格子索引
        ├── fonts_c.png              汉字图集(N 列固定格，含西文回退区可选)
        ├── cn_charmap.json          真汉字 <-> 码位 映射(调试/复用)
        └── cn_font_widths.inc       字宽表(C 数组，供 re3 编译期使用)
```

- 收集文本用到的所有汉字去重排序 → 顺序分配码位 → freetype/PIL 渲染每个字到
  固定格子（如 24×24 或 32×32，适配 320×240）→ 拼成图集 PNG。
- 同时把每条中文串按“真汉字→分配码位”重写成 `wchar` 序列，用 GXT 打包器
  （复用 gxt-utils 骨架，但直通 16-bit，不走它的西文 charset）写出 `chinese.gxt`。

### PNG → TXD

用 magic-txd / rw 工具或脚本把 `fonts_c.png` 打成 `MODELS/FONTS_C.TXD`
（纹理名沿用一个约定，如 `fontcn`）。此步可先用桌面验证，再固化脚本。

### re3 侧改动（仿日文分支）

- 新增 `LANGUAGE_CHINESE`（`CMenuManager`）与 `FONT_CHINESE`（`Font.h` 枚举）。
- `CText::Load` 增加 `CHINESE.GXT` 文件名分支。
- `CFont::Initialise/ReloadFonts` 增加加载 `FONTS_C.TXD` 的分支。
- `CFont::PrintChar` 增加中文图集索引分支（列数按图集定，如 `c % COLS`）。
- 字宽：非等宽用生成的 `cn_font_widths`，或先用等宽简化。
- 用一个 `RE3_CHINESE`/`MORE_LANGUAGES` 编译开关或运行期语言选项启用。

### 渲染链路验证顺序（降低风险）

1. **最小验证**：几十个汉字 + 一个 TTF → 小图集 PNG + 对应 GXT，先在桌面
   build 确认“码位→图集格子”对齐、字能正确显示（不改动大范围 re3）。
2. 扩到全量翻译文本，生成完整图集与 GXT。
3. Pi 上验证性能与显示。

## 风险与备注

- 图集格子尺寸与 UV 计算必须和 `PrintChar` 的除法/列数严格一致，最易出错，故先
  做最小验证。
- 字宽：先等宽跑通，再按字形宽度优化排版。
- 授权：选用开放授权字体（文泉驿正黑 GPL 字体例外 / 思源 SIL OFL）。
- 文件名：re3 无 `CHINESE.GXT` 分支，需改源码新增语言槽；避免覆盖英文槽以便回退。

## 参考

- 字体系统与日文 CJK 先例：`src/renderer/Font.cpp`、`src/renderer/Font.h`
- GXT 加载：`src/text/Text.cpp`（TKEY/TDAT，wchar 原样加载）
- GXT 打包骨架：`spike/gxt-utils`（build/extract；charset 需替换为直通 UTF-16）
- 无名汉化为何不可直接复用：私有码位 + 私有 `fonts.wmf` 点阵库，见前期分析

## 已知问题：拉丁字幕在 CJK 管线下的水平挤压（2026-08）

### 现象

游戏内对话字幕（DrawSubtitles）里的拉丁字母/数字水平方向被挤压、间距不匀，
看起来又细又高，不像原版英文那样紧凑成比例。

### 渲染路径（源码实证）

字幕在 `src/renderer/Hud.cpp` 的 `DrawSubtitles`：

```cpp
CFont::SetScale(SCREEN_SCALE_X_PC(0.48f), SCREEN_SCALE_Y_PC(1.12f)); // scaleX=0.48
CFont::SetFontStyle(FONT_LOCALE(FONT_BANK));
```

`FONT_LOCALE(style)`（`Font.h`）在日文模式下把 `FONT_BANK` 强制替换成
`FONT_JAPANESE`。中文借用日文管线，所以字幕走进 `IsJapaneseFont()` 的 CJK 绘制
分支（`CFont::PrintChar`，`Font.cpp`）。

### 根因

CJK 绘制分支对 **ASCII 字母和汉字一视同仁，全部按“全宽方格”绘制**：

- 绘制矩形宽固定 `CJK_DRAWW * scaleX`（24 × 0.48 ≈ 11.5px），
  高 `CJK_DRAWH * scaleY`（24 × 1.12 ≈ 26.9px）。
- UV 采样固定整格 `1/CJK_COLS`（整个 16px cell）。
- 水平步进走 `GetCharacterSize` → `Size_jp[c] * scaleX`（原版日文半宽表）。

而图集里（`gen_cn_font.py` 的 `put()`）每个 ASCII 字母是**居中渲染在 16px 方格**
里的。三个后果：

1. **方块化**：窄字母（i/l/t）与宽字母（m/w）塞进同一居中方格，左右留白不均，
   间距忽疏忽密。
2. **变窄**：`scaleX=0.48` 是原版为拉丁比例字模调的宽高比；套到 24px 方格上把
   字模水平压成又细又高（11.5×26.9）。
3. **步进失配**：固定 24px 绘制宽 vs `Size_jp[c]` 半宽表步进不一致，间距进一步
   不匀。

对比原版英文（EFIGS）字幕走 else 分支：绘制宽 `32*scaleX*w`、UV 宽 `w`、步进
`charWidth*scaleX` 三者都按字符真实宽度 `w`，故拉丁字母紧凑均匀。CJK 分支丢掉了
这个“按真实字宽”的比例性。

### 修复方向

- **方案 A（治本，推荐）**：让 ASCII 在 CJK 管线里恢复比例渲染。
  `gen_cn_font.py` 的 ASCII 区改为**左对齐渲染 + 导出每字符归一化字宽**（墨迹宽 /
  cell）；`Font.cpp` CJK 分支对 `c <= 94`（ASCII）用该比例宽度统一驱动 UV-w、
  绘制矩形宽、步进，汉字仍走全宽方格。需改工具 + 源码 + 重新生成 TXD/GXT + 重部署。
  收益：菜单/HUD/字幕里的英文数字排版一并变正常。
- **方案 B（治标）**：只在 `Font.cpp` CJK 分支让 ASCII 绘制矩形宽 == 步进宽，
  消除重叠；字母仍居中方格（留白偏大）。仅改源码。

---

## 开发复盘：GXT + CJK 字体管线（阶段性总结，2026-08）

### 最终采用的方案

**复用 re3 现成的日文（Japanese）CJK 字体管线**，而不是新造一套 `RE3_CHINESE`
字体槽。中文资源直接投递到日文槽位并在菜单里选“JAP”语言：

- `text/JAPANESE.GXT`：TKEY/TDAT，码位 = 图集格子索引 + 0x20（见下“码位契约”）。
- `models/FONTS_J.TXD`：一张加宽的 CJK 图集（fontJAP）+ 原版 font1/font2。
- re3 侧仅把日文分支里**硬编码的图集几何**参数化（`CJK_COLS/ROWS/TEXW/CELLW/
  ADVANCE/DRAWW/DRAWH/LINEH`），用 `RE3_CHINESE` 编译开关切换；关掉开关时数值与
  原版日文完全一致，不影响上游行为。
- re3 检测到 `text/japanese.gxt` + `models/fonts_j.txd` 同时存在，会自动在菜单里
  加一个“JAP”语言项——无需新增语言枚举。

### 关键技术结论（都已定位并解决）

1. **码位契约**：打印循环 `c = *s - 0x20`；CJK 绘制 `xoff = c % COLS,
   yoff = c / COLS`。故图集格子 `i` ↔ GXT wchar `i + 0x20`。ASCII 放在格子
   0..94（wchar 即 ASCII 本身，直通），汉字从格子 95 起。
2. **控制符（`~...~`）**：与原版 JAPANESE.gxt 一致，token 里**每个字符**都带
   `0x8000` 位（`JAP_TERMINATION = 0x8000 | '~'`），包括内部字母
   （`~k~` → `0x807E 0x806B 0x807E`）。引擎的 token 解析器按此标志匹配。
3. **TXD 像素格式必须是 PAL8（fmt 0x2500）**，不能是 C8888：GL3 上 librw 只把
   PAL4/PAL8 的 D3D8 纹理转成可用的 GL3 raster（`readAsImage`），C8888 会保持
   不可用 → 整屏色块。
4. **TXD 必须打包全部 3 张纹理**（fontJAP + font1 + font2）：缺 font1/font2 时
   `SetTexture` 返回 null → 全色块。`png2txd.py --base-txd` 原样携带原版
   font1/font2。
5. **TXD 字典 device id 必须为 0**（非 0 会破坏字典加载）。
6. **字形覆盖率放在 ALPHA 通道**（RGB 全白），调色板随索引渐变 alpha；顶点色负责
   给字形上色。

### 工具链现状

- `tools/translate_doubao.py`：火山引擎方舟（豆包）机器翻译，
  模型 `doubao-seed-2-1-pro-260628`，env `ARK_API_KEY`。
- `tools/make_translation_batches.py`：拆批 / 合并翻译（保护控制符）。
- `tools/gen_cn_font.py`：翻译文本 → JAPANESE.GXT + FONTJAP 图集 PNG + 调试
  charmap。
- `tools/png2txd.py`：PNG → PAL8 TXD（`--base-txd` 携带原版 font1/font2）。

### 翻译产物

- 源：`Sergeanur/GXT` 的 `III PC/american.txt`（2649 键）+ 从 re3 扩展
  `american.gxt` 提取的 43 个 re3 自定义键。
- 全量简体中文 2693 条，机器翻译，提交在 fork `FASTSHIFT/GXT` 的
  `spike/GXT/III PC/chinese.txt`。

### 相关提交

- `957c8fb0` re3 CJK 几何参数化（加宽图集）
- `bad2e5ec` 工具：图集 / GXT / PAL8
- `a16f1f4c` 0x8000 token 标志
- `62d21859` RGBA 图集 + png2txd

### 走过的弯路（教训）

- 最初想新造 `RE3_CHINESE` 独立字体精灵路径，改动面大；后改为**复用日文管线只加宽
  几何**，改动可控、可回退（关开关即原版行为）。→ 先评估“复用 vs 新写”。
- Zpix 像素字体（`spike/zpix.ttf`）在目标尺寸下太小太细，弃用，改 Noto Sans CJK
  Bold 16px/16px cell。
- 一度把 C8888 纹理喂给 GL3 导致整屏色块，排查后确认必须 PAL8（结论 3）。
- glfw 调试构建弃用，改 SDL+GBM（`build-sdl-cn`）做屏上字体迭代。

### 遗留 / 待办

- **拉丁字幕水平挤压**（见上一节），方案 A/B 待实施。
- 约 10 个键无任何英文源（FPS_AK/UZI 等武器调试名、FAST_CAR、KF_3、FEM_SL0），
  极少可见，低优先级。
- 可考虑改用 re3 扩展版 `gamefiles/TEXT/american.gxt`（2699 键）做一次干净的全量
  重跑，替代当前 batch_099 补丁式流程。
- 核实 `spike/patch_fontjap.py` 临时脚本是否还需要。

## 修复：裸 FONT_BANK / FONT_HEADING 导致的 CJK 乱码（2026-08）

### 现象
中文（借用日文管线）下，右下角**车名 / 地名**、**任务奖励金额 / mission
passed 大字**等显示为乱码（拉丁字形被 CJK 码位错误索引）。而字幕、菜单等正常。

### 根因
re3 的 CJK 支持依赖**调用点手动把字体样式包一层 `FONT_LOCALE(style)`**
（`IsJapanese() ? FONT_JAPANESE : style`）。上游在若干 HUD 处**漏包**了：

- `CHud::Draw` 里 zone name / vehicle name 用了裸 `FONT_BANK`；
- BigMessage（任务奖励、mission passed、wasted/busted）用了裸 `FONT_HEADING`。

裸样式在中文/日文下不会走 CJK 图集分支（`IsJapaneseFont()` 只认
`FONT_JAPANESE`/`FONT_PAGER`），于是 CJK 码位被拿去拉丁字体图集取字形 → 乱码。
**日文玩家在这些地方同样乱码**，是上游 CJK 支持的通病，不是中文特有。

### 修复：下沉到 SetFontStyle 统一处理
与其在每个调用点补 `FONT_LOCALE`（易遗漏），直接在 `CFont::SetFontStyle` 内部
做重定向：

```cpp
// src/renderer/Font.cpp
if (IsJapanese() && (style == FONT_BANK || style == FONT_HEADING))
    style = FONT_JAPANESE;
```

- 一次覆盖**所有**调用点（含未来新增），车名/地名/任务奖励/大字标题全部恢复。
- 已手动包 `FONT_LOCALE` 的调用点传入的已是 `FONT_JAPANESE`，不匹配条件，不受影响。
- `FONT_PAGER` 有独立 CJK 处理，`FONT_JAPANESE` 本身不动，均保持原样。
- 顺带修好日文的同类 bug。

模拟器（build-sdl-cn）验证：车名、地名、任务奖励、字幕、菜单标题、任务完成/失败
大字全部正常。
