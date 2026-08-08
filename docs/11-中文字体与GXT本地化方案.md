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
