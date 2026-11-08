#!/usr/bin/env python3
"""Render message/chat redesign mockups (before/after). Proposal only."""

from pathlib import Path
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(os.environ.get("MOCKUP_OUT") or Path(__file__).resolve().parent)
OUT.mkdir(parents=True, exist_ok=True)
S = 2
W, H = 320, 240

WHITE = "#FFFFFF"
BG = "#F2F3F5"
TEXT = "#1F1F1F"
SUB = "#8A9099"
HAIR = "#E7E9EC"
ACCENT = "#1F95F6"
DIV = "#EDEFF2"

SARASA = str(ROOT / "fonts/SarasaUiSC-Regular.ttf")
FA7 = str(ROOT / "fonts/Font Awesome 7 Free-Solid-900.otf")
MDI = str(ROOT / "fonts/materialdesignicons-webfont.ttf")

_cache = {}


def font(path, size):
    key = (path, size)
    if key not in _cache:
        _cache[key] = ImageFont.truetype(path, size * S)
    return _cache[key]


def P(v):
    return int(round(v * S))


def txt(d, x, y, s, f, color, anchor="lm"):
    d.text((P(x), P(y)), s, font=f, fill=color, anchor=anchor)


def new_page(bg=BG):
    im = Image.new("RGB", (W * S, H * S), bg)
    return im, ImageDraw.Draw(im)


def bar(d, title, back=False, action=None, bg=WHITE):
    d.rectangle([0, 0, P(W), P(22)], fill=bg)
    d.line([0, P(21), P(W), P(21)], fill=HAIR, width=max(1, S // 2))
    tx = 12
    if back:
        txt(d, 12, 11, "\uF104", font(FA7, 14), TEXT, "mm")
        tx = 36
    txt(d, tx, 11, "12:34", font(SARASA, 14), SUB)
    txt(d, 160, 11, title, font(SARASA, 14), TEXT, "mm")
    bat_right = 266 if action else 312
    txt(d, bat_right - 42, 11, "\U000F0079", font(MDI, 14), SUB, "mm")
    txt(d, bat_right, 11, "88%", font(SARASA, 13), SUB, "rm")
    if action:
        txt(d, 306, 11, action, font(SARASA, 14), ACCENT, "rm")


def bubble(d, x, y, w, text_s, meta=None, self_bubble=False, time_s=None):
    h = 44 if meta is None else 62
    fill = ACCENT if self_bubble else WHITE
    border = 0 if self_bubble else 1
    d.rounded_rectangle([P(x), P(y), P(x + w), P(y + h)], radius=P(16),
                        fill=fill, outline=(None if self_bubble else "#E3E7EC"), width=P(1) if border else 0)
    if meta:
        txt(d, x + 10, y + 10, meta, font(SARASA, 14), "#D8EEFF" if self_bubble else SUB)
        sy = y + 30
    else:
        sy = y + 12
    txt(d, x + 10, sy, text_s, font(SARASA, 14), WHITE if self_bubble else TEXT)
    if time_s:
        txt(d, x + w - 10, y + h - 10, time_s, font(SARASA, 13),
            "#D8EEFF" if self_bubble else SUB, "rm")


def date_pill(d, y, label="今天"):
    tw = d.textlength(label, font=font(SARASA, 14)) / S
    w = tw + 16
    d.rounded_rectangle([P(160 - w / 2), P(y), P(160 + w / 2), P(y + 18)], radius=P(9), fill="#E7EAEE")
    txt(d, 160, y + 9, label, font(SARASA, 14), SUB, "mm")


def bubble_compact(d, x, y, lines, time_s, self_bubble=False, width=180):
    """lines: 一行文本或两行文本列表；时间与最后一行同排右对齐。"""
    h = 28 if len(lines) == 1 else 40
    fill = ACCENT if self_bubble else WHITE
    d.rounded_rectangle([P(x), P(y), P(x + width), P(y + h)], radius=P(12),
                        fill=fill,
                        outline=(None if self_bubble else "#E3E7EC"),
                        width=(0 if self_bubble else P(1)))
    color = WHITE if self_bubble else TEXT
    tcolor = "#D8EEFF" if self_bubble else SUB
    if len(lines) == 1:
        txt(d, x + 8, y + 14, lines[0], font(SARASA, 14), color)
    else:
        txt(d, x + 8, y + 11, lines[0], font(SARASA, 14), color)
        txt(d, x + 8, y + 27, lines[1], font(SARASA, 14), color)
    txt(d, x + width - 8, y + h - 14, time_s, font(SARASA, 13), tcolor, "rm")


def chat_compact():
    im, d = new_page(BG)
    bar(d, "陈同学", back=True)
    date_pill(d, 30)
    bubble_compact(d, 8, 52, ["晚饭吃啥？我快饿死了"], "12:31", width=212)
    bubble_compact(d, 320 - 8 - 96, 86, ["火锅？"], "12:32", self_bubble=True, width=96)
    bubble_compact(d, 8, 120, ["刚开完会，", "马上到"], "12:33", width=176)
    bubble_compact(d, 320 - 8 - 78, 166, ["好"], "12:34", self_bubble=True, width=78)
    input_bar_compact(d, 200, placeholder="发送消息…")
    return im


def input_bar_compact(d, y=200, placeholder="发送消息…"):
    d.rectangle([0, P(y), P(W), P(H)], fill=WHITE)
    d.line([0, P(y), P(W), P(y)], fill=HAIR, width=max(1, S // 2))
    d.rounded_rectangle([P(8), P(y + 5), P(312), P(y + 35)], radius=P(15),
                        fill="#F5F6F8", outline="#E3E7EC", width=P(1))
    txt(d, 22, y + 20, placeholder, font(SARASA, 14), SUB)


def input_bar(d, y=196, placeholder="发送消息…"):
    d.rectangle([0, P(y), P(W), P(H)], fill=WHITE)
    d.line([0, P(y), P(W), P(y)], fill=HAIR, width=max(1, S // 2))
    d.rounded_rectangle([P(8), P(y + 6), P(264), P(y + 38)], radius=P(16),
                        fill="#F5F6F8", outline="#E3E7EC", width=P(1))
    txt(d, 22, y + 22, placeholder, font(SARASA, 14), SUB)
    d.ellipse([P(272), P(y + 5), P(308), P(y + 41)], fill=ACCENT)
    txt(d, 290, y + 23, "\uF1D8", font(FA7, 14), WHITE, "mm")


def chat_before():
    im, d = new_page("#F5F5F5")
    bar(d, "陈同学", back=True)
    bubble(d, 10, 40, 216, "晚饭吃啥？我快饿死了", meta="陈同学 · 12:31")
    bubble(d, 104, 112, 206, "火锅？", meta="我 · 12:32", self_bubble=True)
    bubble(d, 10, 182, 180, "行，楼下那家", meta="陈同学 · 12:33")
    # 当前输入框：底边被裁掉
    d.rectangle([0, P(226), P(W), P(240)], fill=WHITE)
    d.line([0, P(226), P(W), P(226)], fill=HAIR, width=max(1, S // 2))
    d.rounded_rectangle([P(8), P(230), P(264), P(262)], radius=P(16),
                        fill="#F5F6F8", outline="#E3E7EC", width=P(1))
    txt(d, 22, 240, "单击输入消息，长按进入全屏输入", font(SARASA, 14), SUB)
    return im


def chat_after():
    im, d = new_page(BG)
    bar(d, "陈同学", back=True)
    date_pill(d, 30)
    bubble(d, 10, 52, 200, "晚饭吃啥？我快饿死了", time_s="12:31")
    bubble(d, 150, 102, 160, "火锅？", self_bubble=True, time_s="12:32")
    bubble(d, 10, 152, 170, "行，楼下那家", time_s="12:33")
    input_bar(d, 196)
    return im


def tabs(d, y, active=0, styled=True):
    labels = ["联系人", "广场", "设置"]
    if styled:
        d.rectangle([0, P(y), P(W), P(y + 30)], fill=WHITE)
        d.line([0, P(y + 29), P(W), P(y + 29)], fill=HAIR, width=max(1, S // 2))
        for i, lab in enumerate(labels):
            color = ACCENT if i == active else SUB
            cx = 320 / 6 + i * (320 / 3)
            txt(d, cx, y + 15, lab, font(SARASA, 14), color, "mm")
        cx = 320 / 6 + active * (320 / 3)
        d.rectangle([P(cx - 16), P(y + 27), P(cx + 16), P(y + 29)], fill=ACCENT)
    else:
        d.rectangle([0, P(y), P(W), P(y + 33)], fill="#F5F5F5")
        for i, lab in enumerate(labels):
            color = TEXT if i == active else SUB
            cx = 320 / 6 + i * (320 / 3)
            txt(d, cx, y + 16, lab, font(SARASA, 14), color, "mm")


def contacts_before():
    im, d = new_page("#F5F5F5")
    bar(d, "消息")
    tabs(d, 22, 0, styled=False)
    rows = [("陈同学", False, 2), ("项目组", False, 0), ("妈妈", True, 1), ("游戏群", False, 0)]
    y = 62
    for name, fav, unread in rows:
        if fav:
            d.rectangle([P(0), P(y), P(W), P(y + 40)], fill="#B3A6DE")
            color = WHITE
        else:
            color = TEXT
        txt(d, 16, y + 20, name, font(SARASA, 14), color)
        if fav:
            name_w = d.textlength(name, font=font(SARASA, 14)) / S
            txt(d, 16 + name_w + 6, y + 20, "\uF005", font(FA7, 12), WHITE)
        if unread:
            d.ellipse([P(300), P(y + 14), P(312), P(y + 26)], fill="#E35D5D")
            txt(d, 306, y + 20, str(unread), font(SARASA, 10), WHITE, "mm")
        y += 44
    d.ellipse([P(258), P(186), P(304), P(232)], fill=ACCENT)
    txt(d, 281, 209, "+", font(SARASA, 20), WHITE, "mm")
    return im


def contacts_after():
    im, d = new_page(BG)
    bar(d, "消息", action="+")
    tabs(d, 22, 0, styled=True)
    rows = [
        ("陈", "陈同学", "晚饭吃啥？我快饿死了", "12:31", 2, True, "#5B8FF9"),
        ("项", "项目组", "固件已经打包好了，记得验证", "11:58", 0, False, "#E8A33D"),
        ("妈", "妈妈", "周末回来吗", "昨天", 1, True, "#35A16B"),
    ]
    y = 60
    for initial, name, preview, time_s, unread, fav, color in rows:
        d.rounded_rectangle([P(8), P(y), P(312), P(y + 52)], radius=P(12), fill=WHITE)
        d.ellipse([P(14), P(y + 9), P(48), P(y + 43)], fill=color)
        txt(d, 31, y + 26, initial, font(SARASA, 14), WHITE, "mm")
        txt(d, 58, y + 17, name, font(SARASA, 14), TEXT)
        if fav:
            txt(d, 58 + d.textlength(name, font=font(SARASA, 14)) / S + 6, y + 17,
                "\uF005", font(FA7, 12), "#E8B23D")
        txt(d, 58, y + 36, preview, font(SARASA, 14), SUB)
        txt(d, 300, y + 17, time_s, font(SARASA, 13), SUB, "rm")
        if unread:
            d.ellipse([P(286), P(y + 28), P(300), P(y + 42)], fill="#E35D5D")
            txt(d, 293, y + 35, str(unread), font(SARASA, 10), WHITE, "mm")
        y += 58
    return im


def sheet(panels, cols, name):
    scale = 1.5
    tw, th = int(W * scale), int(H * scale)
    pad, label_h = 12, 28
    lf = ImageFont.truetype(SARASA, 19)
    rows = (len(panels) + cols - 1) // cols
    out = Image.new("RGB", (cols * (tw + pad) + pad, rows * (th + label_h + pad) + pad), "#FFFFFF")
    sd = ImageDraw.Draw(out)
    for i, (im, label) in enumerate(panels):
        c, r = i % cols, i // cols
        x = pad + c * (tw + pad)
        y = pad + r * (th + label_h + pad)
        sd.text((x + 2, y + 2), label, font=lf, fill="#555B63")
        out.paste(im.resize((tw, th), Image.LANCZOS), (x, y + label_h))
        sd.rectangle([x, y + label_h, x + tw - 1, y + label_h + th - 1], outline="#DDE1E6")
    out.save(OUT / name)
    print("wrote", OUT / name)


def contact_sheet(panels):
    sheet(panels, 2, "overview.png")


if __name__ == "__main__":
    cb = chat_before()
    ca = chat_after()
    cc = chat_compact()
    lb = contacts_before()
    la = contacts_after()
    cb.save(OUT / "chat-before.png")
    ca.save(OUT / "chat-after.png")
    cc.save(OUT / "chat-compact.png")
    lb.save(OUT / "contacts-before.png")
    la.save(OUT / "contacts-after.png")
    sheet(
        [
            (cb, "现状：气泡 62px，每条带发送人"),
            (ca, "改版 v1：气泡 44px，时间单独一行"),
            (cc, "紧凑版：单行 28px，时间并到最后一行"),
        ],
        3,
        "chat-compare.png",
    )
    contact_sheet(
        [
            (cc, "聊天紧凑版"),
            (la, "联系人改版"),
            (lb, "联系人现状"),
            (cb, "聊天现状"),
        ]
    )
    print("done")
