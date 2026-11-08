#!/usr/bin/env python3
"""Weather UI redesign mockups (proposal only)."""

from pathlib import Path
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(os.environ.get("MOCKUP_OUT") or Path(__file__).resolve().parent)
OUT.mkdir(parents=True, exist_ok=True)
S = 2
W, H = 320, 240
TOP = 52  # tab bar bottom

WHITE = "#FFFFFF"
BG = "#F2F3F5"
TEXT = "#1F1F1F"
SUB = "#8A9099"
HAIR = "#E7E9EC"
ACCENT = "#1F95F6"
AMBER = "#E8A33D"
AMBERBG = "#FFF4E5"
AMBERFG = "#A86400"

SARASA = str(ROOT / "fonts/SarasaUiSC-Regular.ttf")
MDI = str(ROOT / "fonts/materialdesignicons-webfont.ttf")
ICON = ROOT / "imgs/weather/black2x"

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


def bar(d, title="天气", action="刷新"):
    d.rectangle([0, 0, P(W), P(22)], fill=WHITE)
    d.line([0, P(21), P(W), P(21)], fill=HAIR, width=max(1, S // 2))
    txt(d, 12, 11, "12:34", font(SARASA, 14), SUB)
    txt(d, 160, 11, title, font(SARASA, 14), TEXT, "mm")
    bat_right = 276 if action else 314
    pct = "88%"
    pct_w = d.textlength(pct, font=font(SARASA, 13)) / S
    icon_x = bat_right - pct_w - 11
    txt(d, icon_x, 11, "\U000F0079", font(MDI, 14), SUB, "mm")
    txt(d, bat_right, 11, pct, font(SARASA, 13), SUB, "rm")
    if action:
        txt(d, 314, 11, action, font(SARASA, 14), ACCENT, "rm")


def tabs(d, active=0):
    labels = ["现在", "7日", "详情"]
    d.rectangle([0, P(22), P(W), P(52)], fill=WHITE)
    d.line([0, P(51), P(W), P(51)], fill=HAIR, width=max(1, S // 2))
    for i, lab in enumerate(labels):
        color = ACCENT if i == active else SUB
        cx = 320 / 6 + i * (320 / 3)
        txt(d, cx, 37, lab, font(SARASA, 14), color, "mm")
    cx = 320 / 6 + active * (320 / 3)
    d.rectangle([P(cx - 16), P(49), P(cx + 16), P(51)], fill=ACCENT)


def card(d, x, y, w, h, fill=WHITE):
    d.rounded_rectangle([P(x), P(y), P(x + w), P(y + h)], radius=P(12), fill=fill)


def weather_icon(im, code, x, y, size):
    path = ICON / f"{code}@2x.png"
    icon = Image.open(path).convert("RGBA").resize((P(size), P(size)), Image.LANCZOS)
    im.paste(icon, (P(x), P(y)), icon)


def scrollbar(d, top, bottom):
    d.rounded_rectangle([P(316), P(top), P(319), P(bottom)], radius=P(1.5), fill="#C9CED6")


def now_tab():
    im, d = new_page()
    bar(d)
    tabs(d, 0)
    # hero card
    card(d, 8, TOP + 6, 304, 78)
    weather_icon(im, "13", 16, TOP + 14, 48)
    txt(d, 72, TOP + 12, "5℃", font(SARASA, 40), TEXT, "lt")
    txt(d, 72, TOP + 62, "小雨", font(SARASA, 18), TEXT)
    txt(d, 130, TOP + 64, "体感 0℃", font(SARASA, 14), SUB)
    txt(d, 296, TOP + 14, "12℃ / -2℃", font(SARASA, 14), TEXT, "rt")
    txt(d, 296, TOP + 34, "湿度 96%", font(SARASA, 14), SUB, "rt")
    txt(d, 296, TOP + 54, "北风 4级", font(SARASA, 14), SUB, "rt")
    # alert
    d.rounded_rectangle([P(8), P(TOP + 90), P(312), P(TOP + 118)], radius=P(8), fill=AMBERBG)
    d.rectangle([P(8), P(TOP + 90), P(11), P(TOP + 118)], fill=AMBER)
    txt(d, 18, TOP + 104, "寒潮蓝色预警 · 山东省气象台继续发布", font(SARASA, 14), AMBERFG)
    # minutely
    card(d, 8, TOP + 124, 304, 96)
    txt(d, 12, TOP + 134, "未来 2 小时", font(SARASA, 14), SUB)
    txt(d, 296, TOP + 134, "更新 16:48", font(SARASA, 14), SUB, "rm")
    txt(d, 12, TOP + 152, "50分钟后开始下小雨，105分钟后停了", font(SARASA, 14), TEXT)
    vals = [0] * 10 + [0.05, 0.09, 0.08] + [0] * 10 + [0.04] + [0]
    mx = max(vals) or 1
    base_y = TOP + 124 + 30 + 36
    for i, v in enumerate(vals[:24]):
        x = 12 + i * 11.7
        h = 3 if v == 0 else int(3 + (v / mx) * 30)
        color = "#D6E9FF" if v == 0 else ACCENT
        d.rectangle([P(x), P(base_y - h), P(x + 5), P(base_y)], fill=color)
    for x, lab in [(12, "现在"), (70, "30分"), (128, "60分"), (186, "90分"), (244, "120分")]:
        txt(d, x, TOP + 204, lab, font(SARASA, 14), SUB)
    # today summary card (below the fold; scroll)
    card(d, 8, TOP + 230, 304, 76)
    txt(d, 16, TOP + 240, "今日", font(SARASA, 14), SUB)
    txt(d, 16, TOP + 260, "降水 0.5mm", font(SARASA, 14), TEXT)
    txt(d, 164, TOP + 260, "能见度 13km", font(SARASA, 14), TEXT)
    txt(d, 16, TOP + 280, "紫外线 1", font(SARASA, 14), TEXT)
    txt(d, 164, TOP + 280, "气压 1029hPa", font(SARASA, 14), TEXT)
    scrollbar(d, TOP + 6, TOP + 150)
    return im


def seven_tab():
    im, d = new_page()
    bar(d)
    tabs(d, 1)
    days = [
        ("今天", "13", "小雨", "小雨", "12℃ / -2℃"),
        ("11-09", "0", "晴", "晴", "10℃ / -2℃"),
        ("11-10", "0", "晴", "晴", "17℃ / 0℃"),
        ("11-11", "0", "晴", "多云", "17℃ / 1℃"),
        ("11-12", "1", "多云", "阴", "18℃ / 3℃"),
        ("11-13", "4", "阴", "中雨", "16℃ / 7℃"),
        ("11-14", "13", "小雨", "多云", "13℃ / 4℃"),
    ]
    y = TOP + 4
    for i, (date, icon, day, night, temp) in enumerate(days):
        if i % 2 == 0:
            card(d, 8, y - 2, 304, 26)
        if i == 0:
            d.rectangle([P(8), P(y - 2), P(11), P(y + 24)], fill=ACCENT)
        txt(d, 16, y + 11, date, font(SARASA, 14), ACCENT if i == 0 else TEXT)
        weather_icon(im, icon, 72, y - 1, 20)
        label = day if day == night else f"{day}转{night}"
        txt(d, 98, y + 11, label, font(SARASA, 14), TEXT)
        txt(d, 200, y + 11, temp, font(SARASA, 14), TEXT)
        y += 26
    return im


def detail_tab():
    im, d = new_page()
    bar(d)
    tabs(d, 2)
    card(d, 8, TOP + 4, 304, 68)
    txt(d, 16, TOP + 14, "天文", font(SARASA, 14), SUB)
    txt(d, 16, TOP + 34, "日出 06:38", font(SARASA, 14), TEXT)
    txt(d, 164, TOP + 34, "日落 17:04", font(SARASA, 14), TEXT)
    txt(d, 16, TOP + 54, "月出 05:30", font(SARASA, 14), TEXT)
    txt(d, 164, TOP + 54, "月落 16:06", font(SARASA, 14), TEXT)
    card(d, 8, TOP + 78, 304, 60)
    txt(d, 16, TOP + 88, "环境", font(SARASA, 14), SUB)
    txt(d, 16, TOP + 108, "气压 1029hPa", font(SARASA, 14), TEXT)
    txt(d, 164, TOP + 108, "紫外线 1", font(SARASA, 14), TEXT)
    txt(d, 16, TOP + 126, "云量 89%", font(SARASA, 14), TEXT)
    txt(d, 164, TOP + 126, "能见度 23km", font(SARASA, 14), TEXT)
    card(d, 8, TOP + 144, 304, 60)
    txt(d, 16, TOP + 154, "风与湿度", font(SARASA, 14), SUB)
    txt(d, 16, TOP + 174, "北风 1-3级", font(SARASA, 14), TEXT)
    txt(d, 164, TOP + 174, "湿度 73%", font(SARASA, 14), TEXT)
    txt(d, 16, TOP + 192, "降水 7.4mm", font(SARASA, 14), TEXT)
    txt(d, 164, TOP + 192, "月相 残月", font(SARASA, 14), TEXT)
    scrollbar(d, TOP + 4, TOP + 140)
    return im


def sheet(panels):
    scale = 1.5
    tw, th = int(W * scale), int(H * scale)
    pad, label_h = 12, 28
    lf = ImageFont.truetype(SARASA, 19)
    cols = 3
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
    out.save(OUT / "overview.png")
    print("wrote", OUT / "overview.png")


if __name__ == "__main__":
    now = now_tab()
    seven = seven_tab()
    detail = detail_tab()
    now.save(OUT / "now.png")
    seven.save(OUT / "seven-day.png")
    detail.save(OUT / "detail.png")
    sheet(
        [
            (now, "现在：当前 + 预警 + 未来 2 小时降水（可滚动）"),
            (seven, "7 日：日期/天气/高低温/白天转夜间"),
            (detail, "详情：天文、环境、风与湿度（可滚动）"),
        ]
    )
    print("done")
