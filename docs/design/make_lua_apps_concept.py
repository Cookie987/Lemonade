"""Concept mockup: Settings > 运行中的应用 (320x240 @3x).

Tokens from setting-style.yaml / statusbar.yaml:
  page bg 0xf2f3f5 | card #fff r12 | row 304x44 | chinese_font14 everywhere
  title 0x1f1f1f | value 0x8a9099 | group 0x9aa0a6 | chevron 0xc3c7ce
  danger 0xe5484d | accent 0x1f95f6 | row separator 0xedeff2

Status bar = statusbar_enter(title, back=true, action=4):
  time x=36 | wifi x=76 | title TOP_MID w=105 | battery x=-44 | action 36x22 x=-4 (transparent)

Body is ONE vertical scroll container. Layout:
  1. 全部停止 row (top, danger)
  2. PSRAM card
  3. 内部 RAM card
  4. group label
  5. apps rows card
"""

import os

from PIL import Image, ImageDraw, ImageFont

SCALE = 3
W, H = 320, 240
REG = r"C:\Windows\Fonts\msyh.ttc"

PAGE_BG = (0xF2, 0xF3, 0xF5)
WHITE = (0xFF, 0xFF, 0xFF)
TITLE = (0x1F, 0x1F, 0x1F)
VALUE = (0x8A, 0x90, 0x99)
GROUP = (0x9A, 0xA0, 0xA6)
CHEVRON = (0xC3, 0xC7, 0xCE)
DANGER = (0xE5, 0x48, 0x4D)
ACCENT = (0x1F, 0x95, 0xF6)
LINE = (0xE7, 0xE9, 0xEC)
SEP = (0xED, 0xEF, 0xF2)
TRACK = (0xE9, 0xEC, 0xF0)
SCROLL = (0xC9, 0xCE, 0xD4)
ORANGE = (0xFF, 0x9F, 0x0A)

img = Image.new("RGB", (W * SCALE, H * SCALE), "white")
d = ImageDraw.Draw(img)


def font():
    try:
        return ImageFont.truetype(REG, 14 * SCALE)
    except Exception:
        return ImageFont.load_default()


def rect(x, y, w, h, fill=None, outline=None, r=0):
    box = [x * SCALE, y * SCALE, (x + w) * SCALE - 1, (y + h) * SCALE - 1]
    if r:
        d.rounded_rectangle(box, radius=r * SCALE, fill=fill, outline=outline)
    else:
        d.rectangle(box, fill=fill, outline=outline)


def text(x, y, s, color=TITLE, anchor="la"):
    d.text((x * SCALE, y * SCALE), s, font=font(), fill=color, anchor=anchor)


def bar(x, y, w, h, pct, fg=ACCENT):
    rect(x, y, w, h, fill=TRACK, r=h // 2)
    rect(x, y, max(2, int(w * pct)), h, fill=fg, r=h // 2)


# ── page body ──
rect(0, 0, W, H, fill=PAGE_BG)

# ══════════════ scroll content (x8 y26 w304 h210) ══════════════
# 1) PSRAM card (h=64)
rect(8, 26, 304, 64, fill=WHITE, r=12)
text(20, 34, "PSRAM")
text(300, 34, "3.9 / 8.0 MB", color=VALUE, anchor="ra")
bar(20, 52, 280, 6, 0.49)
text(20, 64, "剩余 4.1 MB · 最大块 2.3 MB", color=VALUE)

# 2) internal RAM card (h=64)
rect(8, 94, 304, 64, fill=WHITE, r=12)
text(20, 102, "内部 RAM")
text(300, 102, "201 / 341 KB", color=VALUE, anchor="ra")
bar(20, 120, 280, 6, 0.59, fg=ORANGE)
text(20, 132, "剩余 140 KB · 最大块 96 KB · Lua 合计 46 KB", color=VALUE)

# 3) group label
text(16, 164, "运行中的应用 · 3 个", color=GROUP)

# 4) apps rows
rows = [
    ("英语词典", "前台运行 · 2 分钟", "Lua 14 KB · 栈 6.2 KB"),
    ("音乐", "已挂起 · 18 分钟", "Lua 18 KB · 栈 7.4 KB"),
    ("2048", "已挂起 · 31 分钟", "Lua 14 KB · 栈 5.8 KB"),
]
card_y = 182
rect(8, card_y, 304, 44 * len(rows), fill=WHITE, r=12)
for i, (name, sub, mem) in enumerate(rows):
    y = card_y + i * 44
    if i:
        rect(8, y, 304, 1, fill=SEP)
    text(20, y + 6, name)
    text(20, y + 24, sub, color=VALUE)
    text(234, y + 6, mem, color=VALUE, anchor="ra")
    rect(248, y + 11, 48, 22, fill=(0xFD, 0xEB, 0xEC), r=6)
    text(272, y + 22, "停止", color=DANGER, anchor="mm")

# 5) stop-all at the very end of the scroll content
stop_y = card_y + 44 * len(rows) + 8
rect(8, stop_y, 304, 44, fill=WHITE, r=12)
text(20, stop_y + 13, "全部停止", color=DANGER)
text(296, stop_y + 13, "\u203A", color=CHEVRON, anchor="ra")

# ── scrollbar (AUTO) ──
rect(313, 26, 3, 210, fill=TRACK, r=1)
rect(313, 26, 3, 96, fill=SCROLL, r=1)

# ── statusbar drawn over content ──
rect(0, 0, W, 22, fill=WHITE)
rect(0, 21, W, 1, fill=LINE)
text(16, 11, "\u2039", anchor="mm")
text(36, 3, "10:32", color=VALUE)
text(76, 3, "\u25CF", color=VALUE)
text(160, 11, "运行中的应用", anchor="mm")
text(276, 3, "86%", color=VALUE, anchor="ra")
text(298, 11, "刷新", color=ACCENT, anchor="mm")

# ── clip content at the page bottom edge ──
rect(0, 238, W, 2, fill=PAGE_BG)

img = img.resize((W * SCALE, H * SCALE), Image.LANCZOS)
out = r"H:\Github\Lemonade\docs\images\settings-lua-apps-concept.png"
os.makedirs(os.path.dirname(out), exist_ok=True)
img.save(out)
print("saved", out, "content bottom =", card_y + 44 * len(rows))
