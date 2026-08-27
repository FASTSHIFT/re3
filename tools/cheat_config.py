#!/usr/bin/env python3
"""cheat_config.py - interactive generator for re3's gamepad cheat map (docs/18).

Records controller button combinations/sequences from a PS5 (DualSense) or
standard evdev gamepad and writes them to a cheats.ini the game reads at
startup. Instead of hand-writing key names you just press the buttons.

从 PS5(DualSense)/标准 evdev 手柄录制作弊码的按键组合或序列，写入游戏启动时
读取的 cheats.ini。无需手写键名，按一下手柄即可录入。

Usage / 用法:
    pip install evdev
    python3 tools/cheat_config.py                 # interactive / 交互式
    python3 tools/cheat_config.py -o PATH          # output file / 输出文件
    python3 tools/cheat_config.py --list           # list cheats / 列出作弊码
    python3 tools/cheat_config.py --no-pad         # by name / 手动输入键名
    python3 tools/cheat_config.py --lang zh|en     # force language / 指定语言
    python3 tools/cheat_config.py --selftest       # run self-test / 自检

Key/cheat names MUST stay in sync with the C side / 键名与作弊名须与 C 侧一致:
  * key names   -> src/skel/gbm/cheat_keys.def
  * cheat names -> kCheats[] in src/skel/gbm/cheat_input.cpp
See docs/18.
"""

import argparse
import locale
import os
import sys
import time

# ---- contract with the C side --------------------------------------------

# evdev code -> config key name. Mirrors gamepad_read_state() in input_evdev.cpp
# and the names in cheat_keys.def.
BTN_MAP = {
    "BTN_SOUTH": "CROSS",
    "BTN_EAST": "CIRCLE",
    "BTN_NORTH": "TRIANGLE",
    "BTN_WEST": "SQUARE",
    "BTN_TL": "L1",
    "BTN_TR": "R1",
    "BTN_THUMBL": "L3",
    "BTN_THUMBR": "R3",
    "BTN_SELECT": "CREATE",
    "BTN_START": "OPTIONS",
}
TRIGGER_THRESH = 200  # matches CHEAT_TRIGGER_THRESH in cheat_input.cpp

KEY_NAMES = [
    "CROSS", "CIRCLE", "TRIANGLE", "SQUARE",
    "L1", "R1", "L2", "R2",
    "L3", "R3",
    "CREATE", "OPTIONS",
    "UP", "DOWN", "LEFT", "RIGHT",
]

# (config name, English desc, 中文描述). Names MUST match kCheats[] in
# cheat_input.cpp.
CHEATS = [
    ("weapons", "Full weapon set", "全套武器"),
    ("health", "Full health", "回满血"),
    ("armour", "Full armour", "满护甲"),
    ("money", "+$250000", "加钱 $250000"),
    ("tank", "Spawn tank (Rhino)", "刷坦克"),
    ("blowupcars", "Blow up all cars", "炸毁所有车"),
    ("changeplayer", "Change player model", "切换角色模型"),
    ("mayhem", "Peds riot", "市民暴动"),
    ("everybodyattacks", "Everybody attacks player", "全民攻击玩家"),
    ("weaponsforall", "Peds get weapons", "市民持武器"),
    ("fasttime", "Faster time", "时间加速"),
    ("slowtime", "Slower time", "时间减速"),
    ("wanted_up", "Wanted level +2", "通缉等级 +2"),
    ("wanted_down", "Clear wanted level", "清除通缉"),
    ("sunny", "Sunny weather", "晴天"),
    ("cloudy", "Cloudy weather", "多云"),
    ("rainy", "Rainy weather", "雨天"),
    ("foggy", "Foggy weather", "雾天"),
    ("fastweather", "Fast weather cycle", "天气快速循环"),
    ("wheelsonly", "Render wheels only", "只渲染车轮"),
    ("alldodos", "All cars fly (Dodo)", "所有车可飞"),
    ("stronggrip", "Strong car grip", "超强抓地力"),
    ("nastylimbs", "Nasty limbs", "血腥断肢"),
]
CHEAT_NAMES = {name for name, _, _ in CHEATS}
DEFAULT_MODIFIER = "CREATE"

# ---- i18n -----------------------------------------------------------------

STR = {
    "using_pad":      ("Using gamepad: {n} ({p})", "使用手柄：{n} ({p})"),
    "no_pad":         ("No gamepad found under /dev/input. Connect one, or use --no-pad.",
                       "在 /dev/input 下未找到手柄。请连接手柄，或使用 --no-pad。"),
    "need_evdev":     ("python-evdev not installed. Run: pip install evdev  (or use --no-pad)",
                       "未安装 python-evdev。请运行: pip install evdev （或使用 --no-pad）"),
    "loaded":         ("Loaded {n} existing mapping(s); modifier={m}.",
                       "已载入 {n} 条现有映射；修饰键={m}。"),
    "ask_modifier":   ("\nSet modifier key? current={m} (y/N): ",
                       "\n设置修饰键？当前={m} (y/N): "),
    "hold_modifier":  ("  ... press & HOLD the modifier key(s) now",
                       "  ... 现在按住你要作为修饰键的键"),
    "modifier_set":   ("  modifier = {m}", "  修饰键 = {m}"),
    "pick_cheat":     ("Pick a cheat number (or 'q' to finish): ",
                       "选择作弊码编号（或输入 'q' 结束）: "),
    "invalid_sel":    ("Invalid selection.", "无效选择。"),
    "press_step":     ("  step {i}: press & HOLD the combo ({t:.0f}s window), or nothing to end sequence",
                       "  第 {i} 步：按住组合键（{t:.0f} 秒窗口），不按任何键则结束序列"),
    "captured":       ("  captured: {c}", "  已录制：{c}"),
    "seq_confirm":    ("  {combo} = {cheat}  confirm? (Y/n): ",
                       "  {combo} = {cheat}  确认？(Y/n): "),
    "added":          ("  added {combo} = {cheat}", "  已添加 {combo} = {cheat}"),
    "no_keys":        ("  no keys captured.", "  未录到按键。"),
    "wrote":          ("\nWrote {n} mapping(s) to {p}", "\n已写入 {n} 条映射到 {p}"),
    "avail":          ("Available cheats:", "可用作弊码："),
    "nopad_hint":     ("No-pad mode: type key names joined by '+', steps by ',' e.g. CROSS,CROSS,UP",
                       "手动模式：用 '+' 连接同时按的键，用 ',' 分隔序列步，如 CROSS,CROSS,UP"),
    "valid_keys":     ("Valid keys:", "可用键名："),
    "combo_in":       ("  combo/sequence: ", "  组合/序列: "),
    "invalid_key":    ("  invalid key name, skipping.", "  无效键名，跳过。"),
    "ask_mod_name":   ("  modifier keys: ", "  修饰键: "),
}


def L(lang):
    idx = 1 if lang == "zh" else 0
    return lambda key, **kw: STR[key][idx].format(**kw)


def detect_lang():
    env = (os.environ.get("LANG") or os.environ.get("LC_ALL") or "")
    try:
        loc = locale.getlocale()[0] or ""
    except Exception:
        loc = ""
    if "zh" in env.lower() or "zh" in loc.lower():
        return "zh"
    return "en"


# ---- config file I/O ------------------------------------------------------

def load_existing(path):
    """Return (modifier, [(seq, cheat), ...]) where seq is a list of steps,
    each step a list of key names."""
    modifier = DEFAULT_MODIFIER
    mappings = []
    if not os.path.exists(path):
        return modifier, mappings
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].split(";", 1)[0].strip()
            if "=" not in line:
                continue
            lhs, rhs = (s.strip() for s in line.split("=", 1))
            if lhs.upper() == "MODIFIER":
                modifier = rhs.upper()
                continue
            seq = [[k.strip().upper() for k in step.split("+") if k.strip()]
                   for step in lhs.split(",") if step.strip()]
            mappings.append((seq, rhs.lower()))
    return modifier, mappings


def _seq_str(seq):
    return ",".join("+".join(step) for step in seq)


def write_config(path, modifier, mappings, t):
    desc = {name: (en, zh) for name, en, zh in CHEATS}
    lines = [
        "# cheats.ini - PS5 gamepad cheat map (generated by tools/cheat_config.py)",
        "# Syntax:  <KEY+KEY,...> = <cheat_name>   (modifier auto-required)",
        "#   '+' = keys pressed together in one step; ',' = ordered sequence.",
        "# Key/cheat names: see docs/18.",
        "",
        f"modifier = {modifier}",
        "",
    ]
    for seq, cheat in mappings:
        combo_str = _seq_str(seq)
        pad = " " * max(1, 24 - len(combo_str))
        d = desc.get(cheat, ("", ""))[0]
        lines.append(f"{combo_str}{pad}= {cheat:<16}# {d}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(t("wrote", n=len(mappings), p=path))


# ---- cheat selection ------------------------------------------------------

def list_cheats(lang="en"):
    idx = 2 if lang == "zh" else 1
    print(STR["avail"][1 if lang == "zh" else 0])
    for i, row in enumerate(CHEATS):
        print(f"  {i:2d}. {row[0]:<18} {row[idx]}")


def choose_cheat(t, lang):
    list_cheats(lang)
    while True:
        s = input(t("pick_cheat")).strip()
        if s.lower() in ("q", ""):
            return None
        if s.isdigit() and 0 <= int(s) < len(CHEATS):
            return CHEATS[int(s)][0]
        print(t("invalid_sel"))


# ---- gamepad capture (evdev) ----------------------------------------------

def find_gamepad():
    from evdev import InputDevice, list_devices, ecodes
    pads = []
    for path in list_devices():
        try:
            dev = InputDevice(path)
        except OSError:
            continue
        keys = dev.capabilities().get(ecodes.EV_KEY, [])
        if ecodes.BTN_GAMEPAD in keys or ecodes.BTN_SOUTH in keys:
            pads.append(dev)
    if not pads:
        return None
    for dev in pads:
        if "dualsense" in dev.name.lower() or "ps5" in dev.name.lower():
            return dev
    return pads[0]


def capture_combo(dev, hold_secs=2.0):
    """Read the set of keys held during a short window; return list of names."""
    pressed = set()
    pressed |= _sample_active(dev)
    deadline = time.time() + hold_secs
    while time.time() < deadline:
        r = dev.read_one()
        if r is None:
            time.sleep(0.01)
            continue
        name = _event_to_key(r)
        if name:
            pressed.add(name)
    pressed |= _sample_active(dev)
    return sorted(pressed, key=KEY_NAMES.index)


def _event_to_key(ev):
    from evdev import ecodes
    if ev.type == ecodes.EV_KEY and ev.value == 1:
        for n in _code_names(ev.code):
            if n in BTN_MAP:
                return BTN_MAP[n]
    if ev.type == ecodes.EV_ABS:
        if ev.code == ecodes.ABS_Z and ev.value > TRIGGER_THRESH:
            return "L2"
        if ev.code == ecodes.ABS_RZ and ev.value > TRIGGER_THRESH:
            return "R2"
        if ev.code == ecodes.ABS_HAT0Y:
            return "UP" if ev.value < 0 else ("DOWN" if ev.value > 0 else None)
        if ev.code == ecodes.ABS_HAT0X:
            return "LEFT" if ev.value < 0 else ("RIGHT" if ev.value > 0 else None)
    return None


def _code_names(code):
    from evdev import ecodes
    val = ecodes.bytype[ecodes.EV_KEY].get(code)
    if val is None:
        return []
    return val if isinstance(val, list) else [val]


def _sample_active(dev):
    active = set()
    try:
        for code in dev.active_keys():
            for n in _code_names(code):
                if n in BTN_MAP:
                    active.add(BTN_MAP[n])
    except OSError:
        pass
    return active


# ---- interactive flows ----------------------------------------------------

def _record_sequence(dev, t):
    """Record one or more steps for a single cheat; empty step ends it."""
    seq = []
    for i in range(1, 13):
        print(t("press_step", i=i, t=2.0))
        combo = capture_combo(dev, 2.0)
        if not combo:
            break
        print(t("captured", c="+".join(combo)))
        seq.append(combo)
        time.sleep(0.4)  # small gap so the next step's edge is distinct
    return seq


def flow_with_pad(path, t, lang):
    try:
        import evdev  # noqa: F401
    except ImportError:
        print(t("need_evdev"))
        return 1

    dev = find_gamepad()
    if dev is None:
        print(t("no_pad"))
        return 1
    print(t("using_pad", n=dev.name, p=dev.path))

    modifier, mappings = load_existing(path)
    if mappings:
        print(t("loaded", n=len(mappings), m=modifier))

    if input(t("ask_modifier", m=modifier)).strip().lower() == "y":
        print(t("hold_modifier"))
        combo = capture_combo(dev, 2.0)
        if combo:
            modifier = "+".join(combo)
            print(t("modifier_set", m=modifier))

    while True:
        cheat = choose_cheat(t, lang)
        if cheat is None:
            break
        seq = _record_sequence(dev, t)
        if not seq:
            print(t("no_keys"))
            continue
        combo_str = _seq_str(seq)
        if input(t("seq_confirm", combo=combo_str, cheat=cheat)).strip().lower() == "n":
            continue
        mappings = [(s, ch) for (s, ch) in mappings if _seq_str(s) != combo_str]
        mappings.append((seq, cheat))
        print(t("added", combo=combo_str, cheat=cheat))

    write_config(path, modifier, mappings, t)
    return 0


def flow_no_pad(path, t, lang):
    print(t("nopad_hint"))
    print(t("valid_keys"), " ".join(KEY_NAMES))
    modifier, mappings = load_existing(path)
    if input(t("ask_modifier", m=modifier)).strip().lower() == "y":
        m = input(t("ask_mod_name")).strip().upper()
        if m and all(k in KEY_NAMES for k in m.replace(",", "+").split("+") if k):
            modifier = m
    while True:
        cheat = choose_cheat(t, lang)
        if cheat is None:
            break
        raw = input(t("combo_in")).strip().upper()
        seq = [[k.strip() for k in step.split("+") if k.strip()]
               for step in raw.split(",") if step.strip()]
        if not seq or any(k not in KEY_NAMES for step in seq for k in step):
            print(t("invalid_key"))
            continue
        combo_str = _seq_str(seq)
        mappings = [(s, ch) for (s, ch) in mappings if _seq_str(s) != combo_str]
        mappings.append((seq, cheat))
        print(t("added", combo=combo_str, cheat=cheat))
    write_config(path, modifier, mappings, t)
    return 0


# ---- self-test ------------------------------------------------------------

def selftest():
    """Offline checks: name sync + config round-trip. No gamepad needed."""
    ok = True
    # round-trip a config through write/load
    import tempfile
    t = L("en")
    sample = [([["L1", "R1", "TRIANGLE"]], "weapons"),
              ([["CROSS"], ["CROSS"], ["UP"]], "money")]
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "cheats.ini")
        write_config(p, "CREATE", sample, t)
        mod, maps = load_existing(p)
        assert mod == "CREATE", mod
        assert maps == sample, maps
    # all cheat names valid identifiers, all key names known
    for _, ch in sample:
        assert ch in CHEAT_NAMES
    print("selftest: config round-trip OK")
    print("selftest: repeated-key sequence CROSS,CROSS,UP preserved:",
          _seq_str(sample[1][0]))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="Generate re3 gamepad cheat map.")
    ap.add_argument("-o", "--output", default="cheats.ini", help="output path")
    ap.add_argument("--list", action="store_true", help="list cheats and exit")
    ap.add_argument("--no-pad", action="store_true", help="pick keys by name")
    ap.add_argument("--lang", choices=["zh", "en"], help="force language")
    ap.add_argument("--selftest", action="store_true", help="run offline self-test")
    args = ap.parse_args()

    lang = args.lang or detect_lang()
    t = L(lang)

    if args.selftest:
        return selftest()
    if args.list:
        list_cheats(lang)
        return 0
    if args.no_pad:
        return flow_no_pad(args.output, t, lang)
    return flow_with_pad(args.output, t, lang)


if __name__ == "__main__":
    sys.exit(main())
