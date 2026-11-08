#!/usr/bin/env python3
"""Render 320x240 design mockups for the LemonadeOS settings redesign.

These are proposal artifacts, not shipped assets. They are drawn at 2x and
downsampled so the PNGs read like the real LVGL output at 320x240.

Fonts/colours mirror packages/resources.yaml and the existing settings pages:
  SarasaUiSC-Regular 14   -> body text
  MiSans-Medium 36        -> hero numbers
  Font Awesome 7 Solid    -> row icons / chevrons
"""

from pathlib import Path
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(os.environ.get("MOCKUP_OUT") or Path(__file__).resolve().parent)
OUT.mkdir(parents=True, exist_ok=True)
S = 2  # supersampling factor

W, H = 320, 240

BG = "#F2F3F5"
CARD = "#FFFFFF"
TEXT = "#1F1F1F"
SUB = "#8A9099"
GROUP = "#9AA0A6"
CHEV = "#C3C7CE"
DIV = "#EDEFF2"
ACCENT = "#1F95F6"
DANGER = "#E5484D"
OK = "#35A16B"
TRACK = "#E4E7EB"
TOPBAR = "#FFFFFF"
HAIR = "#E7E9EC"

FONT_DIR = ROOT / "fonts"
SARASA = str(FONT_DIR / "SarasaUiSC-Regular.ttf")
MISANS = str(FONT_DIR / "MiSans-Medium.ttf")
FA7 = str(FONT_DIR / "Font Awesome 7 Free-Solid-900.otf")
FA_BRANDS = str(FONT_DIR / "Font Awesome 7 Brands-Regular-400.otf")

_fonts: dict[tuple[str, int], ImageFont.FreeTypeFont] = {}


def font(path: str, size: int) -> ImageFont.FreeTypeFont:
    key = (path, size)
    if key not in _fonts:
        _fonts[key] = ImageFont.truetype(path, size * S)
    return _fonts[key]


# Font Awesome 7 codepoints used in the mockups.
ICON = {
    "wifi": "\uF1EB",
    "globe": "\uF0AC",
    "display": "\uF108",
    "sun": "\uF185",
    "volume": "\uF028",
    "battery": "\uF241",
    "sd": "\uF7C2",
    "hdd": "\uF0A0",
    "shield": "\uF3ED",
    "update": "\uF021",
    "info": "\uF05A",
    "cloud": "\uF0C2",
    "usb": "\uF287",
    "vial": "\uF492",
    "chevron": "\uF105",
    "back": "\uF104",
    "bolt": "\uF0E7",
    "temp": "\uF2C9",
    "chip": "\uF2DB",
    "clock": "\uF017",
    "tag": "\uF02B",
    "file": "\uF15C",
    "download": "\uF019",
    "check": "\uF058",
    "refresh": "\uF021",
    "lock": "\uF023",
    "trash": "\uF1F8",
    "droplet": "\uF043",
    "invert": "\uF042",
    "sliders": "\uF1DE",
}


def P(v: float) -> int:
    return int(round(v * S))


def new_page(title: str | None, back: bool = True, action: str | None = None) -> Image.Image:
    im = Image.new("RGB", (W * S, H * S), BG)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, P(W), P(22)], fill=TOPBAR)
    d.line([0, P(22), P(W), P(22)], fill=HAIR, width=max(1, S // 2))
    if back:
        d.text((P(12), P(11)), ICON["back"], font=font(FA7, 14), fill=TEXT, anchor="mm")
    if title:
        d.text((P(160), P(11)), title, font=font(SARASA, 14), fill=TEXT, anchor="mm")
    if action:
        d.text((P(308), P(11)), action, font=font(SARASA, 14), fill=ACCENT, anchor="rm")
    return im


def card(d: ImageDraw.ImageDraw, x: float, y: float, w: float, h: float, fill: str = CARD) -> None:
    d.rounded_rectangle([P(x), P(y), P(x + w) - 1, P(y + h) - 1], radius=P(12), fill=fill)


def group(d: ImageDraw.ImageDraw, x: float, y: float, text: str) -> None:
    d.text((P(x), P(y)), text, font=font(SARASA, 14), fill=GROUP, anchor="lm")


def row(
    d: ImageDraw.ImageDraw,
    cx: float,
    y: float,
    w: float,
    icon: str | None,
    title: str,
    value: str | None = None,
    chevron: bool = True,
    h: float = 44,
    last: bool = False,
    icon_color: str = "#7A828C",
    value_color: str = SUB,
    title_color: str = TEXT,
    icon_size: int = 14,
) -> None:
    d.text((P(cx + 12), P(y + h / 2)), ICON[icon] if icon in ICON else icon,
           font=font(FA7, icon_size), fill=icon_color, anchor="lm")
    d.text((P(cx + 32), P(y + h / 2)), title, font=font(SARASA, 14), fill=title_color, anchor="lm")
    if value is not None:
        anchor_x = cx + w - (26 if chevron else 12)
        d.text((P(anchor_x), P(y + h / 2)), value, font=font(SARASA, 14), fill=value_color, anchor="rm")
    if chevron:
        d.text((P(cx + w - 14), P(y + h / 2)), ICON["chevron"], font=font(FA7, 14), fill=CHEV, anchor="rm")
    if not last:
        d.line([P(cx + 32), P(y + h - 0.5), P(cx + w - 0.5), P(y + h - 0.5)], fill=DIV, width=max(1, S // 2))


def switch(d: ImageDraw.ImageDraw, x: float, y: float, on: bool = True, w: float = 40, h: float = 22) -> None:
    track = ACCENT if on else "#DDE1E6"
    d.rounded_rectangle([P(x), P(y), P(x + w), P(y + h)], radius=P(h / 2), fill=track)
    kx = x + w - h / 2 - 1 if on else x + h / 2 + 1
    d.ellipse([P(kx - 9), P(y + h / 2 - 9), P(kx + 9), P(y + h / 2 + 9)], fill=CARD)


def slider(d: ImageDraw.ImageDraw, x: float, y: float, w: float, pct: float, h: float = 6) -> None:
    d.rounded_rectangle([P(x), P(y), P(x + w), P(y + h)], radius=P(h / 2), fill=TRACK)
    d.rounded_rectangle([P(x), P(y), P(x + max(h, w * pct)), P(y + h)], radius=P(h / 2), fill=ACCENT)
    kx = x + w * pct
    d.ellipse([P(kx - 8), P(y + h / 2 - 8), P(kx + 8), P(y + h / 2 + 8)], fill=CARD)
    d.ellipse([P(kx - 8), P(y + h / 2 - 8), P(kx + 8), P(y + h / 2 + 8)], outline=ACCENT, width=max(1, S))


def bar(d: ImageDraw.ImageDraw, x: float, y: float, w: float, pct: float, h: float = 6) -> None:
    d.rounded_rectangle([P(x), P(y), P(x + w), P(y + h)], radius=P(h / 2), fill=TRACK)
    d.rounded_rectangle([P(x), P(y), P(x + max(h, w * pct)), P(y + h)], radius=P(h / 2), fill=ACCENT)


def pill(d: ImageDraw.ImageDraw, x: float, y: float, text: str, fg: str, bg: str) -> float:
    tw = d.textlength(text, font=font(SARASA, 14)) / S
    w = tw + 14
    h = 20
    d.rounded_rectangle([P(x), P(y), P(x + w), P(y + h)], radius=P(10), fill=bg)
    d.text((P(x + w / 2), P(y + h / 2)), text, font=font(SARASA, 14), fill=fg, anchor="mm")
    return w


def scrollbar(d: ImageDraw.ImageDraw, content_h: float) -> None:
    view_h = H - 22
    track_x = W - 3
    thumb_h = max(20, view_h * view_h / content_h)
    d.rounded_rectangle([P(track_x), P(28), P(track_x + 2), P(28 + thumb_h)], radius=P(1), fill="#C9CDD3")


# ---------------------------------------------------------------------------
# Home, option A: grouped list with per-row status summaries
# ---------------------------------------------------------------------------
def home_a() -> Image.Image:
    content_h = 496
    im = new_page("设置", back=True)
    d = ImageDraw.Draw(im)
    # extend canvas so the full (scrolled) content is visible in one image
    full = Image.new("RGB", (W * S, content_h * S), BG)
    fd = ImageDraw.Draw(full)
    full.paste(im, (0, 0))
    d = ImageDraw.Draw(full)

    card(d, 8, 30, 304, 88)
    row(d, 8, 30, 304, "wifi", "网络与连接", "SEEWO")
    row(d, 8, 74, 304, "vial", "瓶中之海", "蓝牙广播", last=True)

    group(d, 16, 134, "本机")
    card(d, 8, 148, 304, 176)
    row(d, 8, 148, 304, "sun", "显示与亮度", "80%")
    row(d, 8, 192, 304, "volume", "声音", "蜂鸣器开")
    row(d, 8, 236, 304, "battery", "电源与电池", "43% · 充电中")
    row(d, 8, 280, 304, "sd", "存储", "12.4 GB 可用", last=True)

    group(d, 16, 342, "系统")
    card(d, 8, 356, 304, 132)
    row(d, 8, 356, 304, "shield", "安全与隐私", "数字密码")
    row(d, 8, 400, 304, "update", "系统更新", "已是最新")
    row(d, 8, 444, 304, "info", "关于", "1.11.0", last=True)

    scrollbar(d, content_h)
    return full


def home_a_viewport(full: Image.Image) -> Image.Image:
    return full.crop((0, 0, W * S, H * S))


# ---------------------------------------------------------------------------
# Home, option B: 2 x 4 dashboard tiles, everything visible
# ---------------------------------------------------------------------------
def home_b() -> Image.Image:
    im = new_page("设置", back=True)
    d = ImageDraw.Draw(im)
    tiles = [
        ("wifi", "网络与连接"),
        ("vial", "瓶中之海"),
        ("sun", "显示与亮度"),
        ("volume", "声音"),
        ("battery", "电源与电池"),
        ("sd", "存储"),
        ("shield", "安全与隐私"),
        ("update", "系统更新"),
        ("info", "关于"),
    ]
    for i, (icon, title) in enumerate(tiles):
        col, r = i % 3, i // 3
        x = 8 + col * 103
        y = 28 + r * 64
        card(d, x, y, 98, 58)
        d.text((P(x + 49), P(y + 19)), ICON[icon], font=font(FA7, 16), fill="#7A828C", anchor="mm")
        d.text((P(x + 49), P(y + 41)), title, font=font(SARASA, 14), fill=TEXT, anchor="mm")
    return im


# ---------------------------------------------------------------------------
# Sub pages
# ---------------------------------------------------------------------------
def page_network() -> Image.Image:
    im = new_page("网络与连接")
    d = ImageDraw.Draw(im)
    card(d, 8, 28, 304, 88)
    d.text((P(20), P(50)), ICON["wifi"], font=font(FA7, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(50)), "Wi-Fi", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    d.text((P(230), P(50)), "SEEWO · -58 dBm", font=font(SARASA, 14), fill=SUB, anchor="rm")
    switch(d, 268, 39, True)
    d.line([P(40), P(71.5), P(311), P(71.5)], fill=DIV, width=max(1, S // 2))
    row(d, 8, 72, 304, "sliders", "Wi-Fi 配置与扫描", "2 个已保存", last=True)

    card(d, 8, 128, 304, 88)
    row(d, 8, 128, 304, "cloud", "Cookie IoT", "已绑定")
    d.text((P(20), P(194)), "\uF287", font=font(FA_BRANDS, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(194)), "USB 主机模式", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    switch(d, 268, 183, False)
    return im


def page_display() -> Image.Image:
    im = new_page("显示与亮度")
    d = ImageDraw.Draw(im)

    card(d, 8, 26, 304, 70)
    d.text((P(20), P(48)), ICON["sun"], font=font(FA7, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(48)), "屏幕亮度", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    d.text((P(296), P(48)), "80%", font=font(SARASA, 14), fill=SUB, anchor="rm")
    slider(d, 40, 74, 256, 0.8)

    card(d, 8, 104, 304, 132)
    row(d, 8, 104, 304, "display", "背景图片", "系统默认")
    d.text((P(20), P(170)), ICON["cloud"], font=font(FA7, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(170)), "获取 Cookie IoT 背景图", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    d.text((P(296), P(170)), "获取", font=font(SARASA, 14), fill=ACCENT, anchor="rm")
    d.line([P(40), P(191.5), P(311), P(191.5)], fill=DIV, width=max(1, S // 2))
    d.text((P(20), P(214)), ICON["invert"], font=font(FA7, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(214)), "颜色反转", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    switch(d, 268, 203, True)
    return im


def page_sound() -> Image.Image:
    im = new_page("声音")
    d = ImageDraw.Draw(im)
    card(d, 8, 28, 304, 44)
    d.text((P(20), P(50)), ICON["volume"], font=font(FA7, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(50)), "蜂鸣器", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    switch(d, 268, 39, True)

    card(d, 8, 80, 304, 132)
    for i, (label, on) in enumerate([("开机音效", True), ("通知音效", True), ("闹钟音效", False)]):
        y = 80 + i * 44
        d.text((P(40), P(y + 22)), label, font=font(SARASA, 14), fill=TEXT, anchor="lm")
        switch(d, 268, y + 11, on)
        if i < 2:
            d.line([P(40), P(y + 43.5), P(311), P(y + 43.5)], fill=DIV, width=max(1, S // 2))
    d.text((P(16), P(228)), "总开关关闭后，提示音将静音", font=font(SARASA, 14), fill=GROUP, anchor="lm")
    return im


def page_power() -> Image.Image:
    im = new_page("电源与电池")
    d = ImageDraw.Draw(im)
    card(d, 8, 28, 304, 92)
    d.text((P(20), P(66)), "43%", font=font(MISANS, 36), fill=TEXT, anchor="lm")
    d.text((P(116), P(46)), ICON["bolt"], font=font(FA7, 14), fill=OK, anchor="lm")
    d.text((P(134), P(46)), "充电中", font=font(SARASA, 14), fill=OK, anchor="lm")
    d.text((P(296), P(46)), "42.1 ℃", font=font(SARASA, 14), fill=SUB, anchor="rm")
    bar(d, 20, 94, 276, 0.43)
    d.text((P(20), P(108)), "预计 1 小时 40 分钟充满", font=font(SARASA, 14), fill=SUB, anchor="lm")

    card(d, 8, 128, 304, 88)
    d.text((P(20), P(150)), ICON["battery"], font=font(FA7, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(150)), "电池电压", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    d.text((P(296), P(150)), "4.05 V", font=font(SARASA, 14), fill=SUB, anchor="rm")
    d.line([P(40), P(171.5), P(311), P(171.5)], fill=DIV, width=max(1, S // 2))
    d.text((P(20), P(194)), "\uF287", font=font(FA_BRANDS, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(194)), "USB 输入", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    d.text((P(296), P(194)), "5.02 V", font=font(SARASA, 14), fill=SUB, anchor="rm")
    return im


def page_storage() -> Image.Image:
    im = new_page("存储")
    d = ImageDraw.Draw(im)

    def storage_card(y, title, used, total, pct, icon):
        card(d, 8, y, 304, 86)
        d.text((P(20), P(y + 20)), ICON[icon], font=font(FA7, 14), fill="#7A828C", anchor="lm")
        d.text((P(40), P(y + 20)), title, font=font(SARASA, 14), fill=TEXT, anchor="lm")
        d.text((P(296), P(y + 20)), f"{used} / {total}", font=font(SARASA, 14), fill=SUB, anchor="rm")
        bar(d, 20, y + 46, 276, pct)
        free = round(total * (1 - pct), 1)
        d.text((P(20), P(y + 66)), f"剩余 {free} 可用", font=font(SARASA, 14), fill=GROUP, anchor="lm")

    card(d, 8, 28, 304, 86)
    d.text((P(20), P(48)), ICON["hdd"], font=font(FA7, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(48)), "内部存储", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    d.text((P(296), P(48)), "1.2 MB / 4.0 MB", font=font(SARASA, 14), fill=SUB, anchor="rm")
    bar(d, 20, 74, 276, 0.3)
    d.text((P(20), P(94)), "剩余 2.8 MB 可用", font=font(SARASA, 14), fill=GROUP, anchor="lm")

    card(d, 8, 122, 304, 86)
    d.text((P(20), P(142)), ICON["sd"], font=font(FA7, 14), fill="#7A828C", anchor="lm")
    d.text((P(40), P(142)), "SD 卡", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    d.text((P(296), P(142)), "17.4 GB / 29.8 GB", font=font(SARASA, 14), fill=SUB, anchor="rm")
    bar(d, 20, 168, 276, 0.58)
    d.text((P(20), P(188)), "剩余 12.4 GB 可用", font=font(SARASA, 14), fill=GROUP, anchor="lm")
    return im


def page_update() -> Image.Image:
    im = new_page("系统更新")
    d = ImageDraw.Draw(im)
    card(d, 8, 28, 304, 72)
    d.text((P(20), P(50)), "当前版本", font=font(SARASA, 14), fill=SUB, anchor="lm")
    d.text((P(20), P(72)), "1.11.0", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    pill(d, 200, 42, "可更新", ACCENT, "#E8F2FE")
    d.text((P(296), P(72)), "检查于 12:30", font=font(SARASA, 14), fill=GROUP, anchor="rm")

    card(d, 8, 108, 304, 84)
    d.text((P(20), P(126)), "更新日志", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    d.text((P(20), P(148)), "· 优化设置页层级与布局", font=font(SARASA, 14), fill=SUB, anchor="lm")
    d.text((P(20), P(170)), "· 修复瓶中之海滑块的滚动问题", font=font(SARASA, 14), fill=SUB, anchor="lm")

    d.rounded_rectangle([P(8), P(200), P(140), P(234)], radius=P(10), fill=CARD)
    d.text((P(74), P(217)), "检查更新", font=font(SARASA, 14), fill=TEXT, anchor="mm")
    d.rounded_rectangle([P(148), P(200), P(312), P(234)], radius=P(10), fill=ACCENT)
    d.text((P(230), P(217)), "安装 1.12.0", font=font(SARASA, 14), fill=CARD, anchor="mm")
    return im


def page_about() -> Image.Image:
    content_h = 300
    im = new_page("关于")
    full = Image.new("RGB", (W * S, content_h * S), BG)
    full.paste(im, (0, 0))
    d = ImageDraw.Draw(full)
    card(d, 8, 28, 304, 60)
    logo = (
        Image.open(ROOT / "imgs/logo.png")
        .convert("RGBA")
        .crop((70, 0, 250, 160))
        .resize((P(40), P(36)), Image.LANCZOS)
    )
    full.paste(logo, (P(20), P(40)), logo)
    d.text((P(70), P(48)), "LemonadeOS", font=font(SARASA, 14), fill=TEXT, anchor="lm")
    d.text((P(70), P(68)), "1.11.0 · ESPHome 2026.10.0", font=font(SARASA, 14), fill=SUB, anchor="lm")

    card(d, 8, 96, 304, 132)
    row(d, 8, 96, 304, "chip", "设备型号", "Lemonade S3 Touch", chevron=False)
    row(d, 8, 140, 304, "temp", "CPU 温度", "42.1 ℃", chevron=False)
    row(d, 8, 184, 304, "clock", "正常运行时间", "3 天 12 小时", chevron=False)

    card(d, 8, 236, 304, 88)
    row(d, 8, 236, 304, "tag", "MAC 地址", "AC:12:34:56:78:9A", chevron=False)
    row(d, 8, 280, 304, "file", "上次错误日志", "无记录")
    scrollbar(d, content_h)
    return full


def contact_sheet(home_a_file: Image.Image) -> None:
    pages = [
        (page_network(), "网络与连接"),
        (page_display(), "显示与亮度"),
        (page_sound(), "声音"),
        (page_power(), "电源与电池"),
        (page_storage(), "存储"),
        (page_update(), "系统更新"),
        (home_b(), "设置首页 · 方案 B"),
        (home_a_viewport(home_a_file), "设置首页 · 方案 A"),
        (page_about(), "关于"),
    ]
    scale = 1.5
    tw, th = int(W * scale), int(H * scale)
    pad, label_h = 12, 28
    label_font = ImageFont.truetype(SARASA, 21)
    cols = 3
    rows = (len(pages) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (tw + pad) + pad, rows * (th + label_h + pad) + pad), "#FFFFFF")
    d = ImageDraw.Draw(sheet)
    for i, (im, label) in enumerate(pages):
        c, r = i % cols, i // cols
        x = pad + c * (tw + pad)
        y = pad + r * (th + label_h + pad)
        d.text((x + 2, y + 4), label, font=label_font, fill="#555B63")
        sheet.paste(im.resize((tw, th), Image.LANCZOS), (x, y + label_h))
        d.rectangle([x, y + label_h, x + tw - 1, y + label_h + th - 1], outline="#DDE1E6")
    sheet.save(OUT / "overview.png")
    print("wrote", OUT / "overview.png")


if __name__ == "__main__":
    home_a_file = home_a()
    home_a_file.save(OUT / "home-a-full.png")
    home_a_viewport(home_a_file).save(OUT / "home-a.png")
    home_b().save(OUT / "home-b.png")
    page_network().save(OUT / "page-network.png")
    page_display().save(OUT / "page-display.png")
    page_sound().save(OUT / "page-sound.png")
    page_power().save(OUT / "page-power.png")
    page_storage().save(OUT / "page-storage.png")
    page_update().save(OUT / "page-update.png")
    page_about().save(OUT / "page-about.png")
    contact_sheet(home_a_file)
    print("done")
