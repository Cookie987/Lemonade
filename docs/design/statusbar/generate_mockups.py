#!/usr/bin/env python3
"""Render status-bar redesign mockups (before/after + modes).

Proposal artifact only. Draws at 2x and downsamples to 320x240-like output.
"""

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
CHEV = "#C3C7CE"
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


def text(d, x, y, s, f, color, anchor="lm"):
    d.text((P(x), P(y)), s, font=f, fill=color, anchor=anchor)


def page_base(bg=WHITE):
    im = Image.new("RGB", (W * S, H * S), bg)
    return im, ImageDraw.Draw(im)


def bar_surface(d, h=22):
    d.rectangle([0, 0, P(W), P(h)], fill=WHITE)
    d.line([0, P(h) - 1, P(W), P(h) - 1], fill=HAIR, width=max(1, S // 2))


def back_arrow(d, color=TEXT):
    text(d, 12, 11, "\uF104", font(FA7, 14), color, "mm")


def hairline(d, y):
    d.line([P(0), P(y), P(W), P(y)], fill=HAIR, width=max(1, S // 2))


def message_rows(d, top=30):
    rows = [
        ("A", "#E8A33D", "陈同学", "晚饭吃啥？我快饿死了", "12:31", 2),
        ("B", "#5B8FF9", "项目组", "固件已经打包好了，记得验证", "11:58", 0),
        ("C", "#35A16B", "妈妈", "周末回来吗", "昨天", 1),
        ("D", "#9B7BD8", "游戏群", "[图片]", "昨天", 0),
        ("E", "#E35D5D", "快递", "取件码 8-2-1043", "周三", 0),
    ]
    y = top
    for ch, color, name, preview, time_s, unread in rows:
        d.ellipse([P(12), P(y + 6), P(48), P(y + 42)], fill=color)
        text(d, 30, y + 24, ch, font(SARASA, 16), WHITE, "mm")
        text(d, 58, y + 12, name, font(SARASA, 14), TEXT)
        text(d, 58, y + 30, preview, font(SARASA, 14), SUB)
        text(d, 308, y + 12, time_s, font(SARASA, 14), SUB, "rm")
        if unread:
            d.ellipse([P(302), P(y + 26), P(314), P(y + 38)], fill="#E35D5D")
            text(d, 308, y + 32, str(unread), font(SARASA, 10), WHITE, "mm")
        d.line([P(58), P(y + 47), P(312), P(y + 47)], fill=DIV, width=max(1, S // 2))
        y += 48


def app_cards(d, top=30):
    items = [
        ("计算器", "工具 · 128 KB · v1.2.0", "#5B8FF9"),
        ("2048", "游戏 · 96 KB · v2.0.1", "#E8A33D"),
        ("词典", "学习 · 4.2 MB · v1.0.3", "#35A16B"),
    ]
    y = top
    for name, meta, color in items:
        d.rounded_rectangle([P(8), P(y), P(312), P(y + 56)], radius=P(10), fill=WHITE)
        d.rounded_rectangle([P(18), P(y + 10), P(54), P(y + 46)], radius=P(8), fill=color)
        text(d, 36, y + 28, name[:1], font(SARASA, 16), WHITE, "mm")
        text(d, 66, y + 18, name, font(SARASA, 14), TEXT)
        text(d, 66, y + 36, meta, font(SARASA, 14), SUB)
        text(d, 300, y + 28, "\uF105", font(FA7, 14), CHEV, "mm")
        y += 62


# ---------------------------------------------------------------------------
# Panels
# ---------------------------------------------------------------------------
def before_message():
    im, d = page_base(WHITE)
    # 旧状态栏：左=时钟+日期，中=标题，右=Wi-Fi+电量
    text(d, 8, 8, "12:34 11.08 Sat", font(SARASA, 13), TEXT)
    text(d, 160, 9, "消息", font(SARASA, 14), TEXT, "mm")
    text(d, 262, 9, "\U000F0928", font(MDI, 14), TEXT, "mm")
    text(d, 312, 9, "88%", font(SARASA, 13), TEXT, "rm")
    message_rows(d, top=26)
    # 标注三块区域
    d.rectangle([P(4), P(1), P(122), P(19)], outline="#E5A3A3", width=max(1, S))
    d.rectangle([P(132), P(1), P(188), P(19)], outline="#A3C8E5", width=max(1, S))
    d.rectangle([P(250), P(1), P(316), P(19)], outline="#A3D5B5", width=max(1, S))
    return im


def after_message(status=False, back=True):
    im, d = page_base(BG)
    bar_surface(d, 22)
    if back:
        back_arrow(d)
    text(d, 160, 11, "消息", font(SARASA, 14), TEXT, "mm")
    if status:
        text(d, 292, 11, "12:34", font(SARASA, 13), SUB, "rm")
    message_rows(d, top=30)
    return im


def after_appstore(action=True):
    im, d = page_base(BG)
    bar_surface(d, 22)
    back_arrow(d)
    text(d, 160, 11, "应用商店", font(SARASA, 14), TEXT, "mm")
    if action:
        text(d, 306, 11, "刷新", font(SARASA, 14), ACCENT, "rm")
    app_cards(d, top=30)
    return im


def after_status_left(title="消息", action=None, back=False):
    """方案 B 终稿：返回（可选）+ 时间在左，标题居中，电量和动作在右。"""
    im, d = page_base(BG)
    bar_surface(d, 22)
    if back:
        back_arrow(d)
        text(d, 36, 11, "12:34", font(SARASA, 14), SUB)
    else:
        text(d, 12, 11, "12:34", font(SARASA, 14), SUB)
    text(d, 160, 11, title, font(SARASA, 14), TEXT, "mm")
    bat_right = 266 if action else 312
    text(d, bat_right - 42, 11, "\U000F0079", font(MDI, 14), SUB, "mm")
    text(d, bat_right, 11, "88%", font(SARASA, 13), SUB, "rm")
    if action:
        text(d, 306, 11, action, font(SARASA, 14), ACCENT, "rm")
    return im


def after_message_status_left():
    im = after_status_left("消息", back=False)
    d = ImageDraw.Draw(im)
    message_rows(d, top=30)
    return im


def after_chat_status_left():
    im = after_status_left("陈同学", back=True)
    d = ImageDraw.Draw(im)
    message_rows(d, top=30)
    return im


def after_appstore_status_left():
    im = after_status_left("应用商店", "刷新", back=False)
    d = ImageDraw.Draw(im)
    app_cards(d, top=30)
    return im


def after_status_full():
    im, d = page_base(BG)
    bar_surface(d, 22)
    back_arrow(d)
    text(d, 160, 11, "消息", font(SARASA, 14), TEXT, "mm")
    text(d, 248, 11, "12:34", font(SARASA, 13), SUB, "rm")
    text(d, 268, 11, "\U000F0928", font(MDI, 13), SUB, "mm")
    text(d, 292, 11, "\U000F0079", font(MDI, 13), SUB, "mm")
    text(d, 312, 11, "88", font(SARASA, 12), SUB, "rm")
    message_rows(d, top=30)
    return im


def after_edit():
    im = Image.open(ROOT / "imgs/moon.jpg").convert("RGB").resize((W * S, H * S))
    d = ImageDraw.Draw(im)
    icons = [
        ("settings-96x.png", -96, -45),
        ("timer-96x.png", 0, -45),
        ("message-96x.png", 96, -45),
        ("weather-96x.png", -48, 45),
        ("appstore-96x.png", 48, 45),
    ]
    for name, dx, dy in icons:
        icon = Image.open(ROOT / "imgs/icon/app" / name).convert("RGBA").resize((P(72), P(72)))
        im.paste(icon, (P(160 + dx - 36), P(120 + dy - 36)), icon)
    # 编辑模式：只浮一个「完成」，不出现整条白色状态栏
    d.rounded_rectangle([P(260), P(4), P(312), P(26)], radius=P(11), fill=ACCENT)
    text(d, 286, 15, "完成", font(SARASA, 12), WHITE, "mm")
    return im


def contact_sheet(panels):
    scale = 1.5
    tw, th = int(W * scale), int(H * scale)
    pad, label_h = 10, 24
    lf = ImageFont.truetype(SARASA, 18)
    cols = 3
    rows = (len(panels) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (tw + pad) + pad, rows * (th + label_h + pad) + pad), "#FFFFFF")
    sd = ImageDraw.Draw(sheet)
    for i, (im, label) in enumerate(panels):
        c, r = i % cols, i // cols
        x = pad + c * (tw + pad)
        y = pad + r * (th + label_h + pad)
        sd.text((x + 2, y + 2), label, font=lf, fill="#555B63")
        sheet.paste(im.resize((tw, th), Image.LANCZOS), (x, y + label_h))
        sd.rectangle([x, y + label_h, x + tw - 1, y + label_h + th - 1], outline="#DDE1E6")
    sheet.save(OUT / "overview.png")
    print("wrote", OUT / "overview.png")


if __name__ == "__main__":
    before = before_message()
    nav = after_message()
    status_left = after_message_status_left()
    chat_left = after_chat_status_left()
    action_left = after_appstore_status_left()
    edit = after_edit()

    before.save(OUT / "before.png")
    nav.save(OUT / "after-nav.png")
    status_left.save(OUT / "after-status-left.png")
    chat_left.save(OUT / "after-chat-left.png")
    action_left.save(OUT / "after-action-left.png")
    edit.save(OUT / "after-edit.png")

    contact_sheet(
        [
            (before, "现状：三块挤在 22px，标题只剩约 56px"),
            (nav, "方案 A：返回 + 标题（备选）"),
            (status_left, "方案 B：无页内上级（时间在左 + 电量）"),
            (chat_left, "方案 B：有页内上级（返回 + 时间 + 电量）"),
            (action_left, "方案 B：应用商店（刷新 + 时间 + 电量）"),
            (edit, "主屏不改：编辑模式仍浮「完成」"),
        ]
    )
    print("done")
