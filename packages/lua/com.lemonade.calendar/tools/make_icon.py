"""生成日历应用图标（96x96 PNG），与其它内置应用的图标尺寸一致。

用法：python packages/lua/com.lemonade.calendar/tools/make_icon.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

SIZE = 96
OUT = Path(__file__).resolve().parents[1] / "assets" / "icon.png"

BG_TOP = (27, 84, 166)
BG_BOTTOM = (18, 58, 120)
CARD = (255, 255, 255)
HEADER = (224, 76, 60)
GRID = (206, 216, 228)
ACCENT = (27, 84, 166)
SHADOW = (12, 40, 86)


def lerp(a: int, b: int, t: float) -> int:
    return int(round(a + (b - a) * t))


def rounded_rect_mask(w: int, h: int, radius: int) -> list[list[bool]]:
    mask = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            cx = min(max(x, radius), w - 1 - radius)
            cy = min(max(y, radius), h - 1 - radius)
            dx = x - cx
            dy = y - cy
            if dx * dx + dy * dy <= radius * radius or (radius <= x < w - radius) or (radius <= y < h - radius):
                mask[y][x] = True
    return mask


def main() -> None:
    # 背景：竖直渐变的圆角方形（真机图标是方形的，留 4px 圆角）
    radius = 18
    mask = rounded_rect_mask(SIZE, SIZE, radius)

    pixels = [[(0, 0, 0) for _ in range(SIZE)] for _ in range(SIZE)]
    for y in range(SIZE):
        t = y / (SIZE - 1)
        base = (lerp(BG_TOP[0], BG_BOTTOM[0], t), lerp(BG_TOP[1], BG_BOTTOM[1], t), lerp(BG_TOP[2], BG_BOTTOM[2], t))
        for x in range(SIZE):
            pixels[y][x] = base

    # 卡片阴影
    card_x, card_y, card_w, card_h = 14, 18, 68, 64
    for y in range(card_y + 3, card_y + card_h + 3):
        for x in range(card_x + 1, card_x + card_w + 1):
            if 0 <= y < SIZE and 0 <= x < SIZE and mask[y][x]:
                r, g, b = pixels[y][x]
                pixels[y][x] = (lerp(r, SHADOW[0], 0.35), lerp(g, SHADOW[1], 0.35), lerp(b, SHADOW[2], 0.35))

    # 卡片主体
    for y in range(card_y, card_y + card_h):
        for x in range(card_x, card_x + card_w):
            if not (0 <= y < SIZE and 0 <= x < SIZE):
                continue
            # 卡片自身的圆角
            r = 8
            cx = min(max(x, card_x + r), card_x + card_w - 1 - r)
            cy = min(max(y, card_y + r), card_y + card_h - 1 - r)
            dx = x - cx
            dy = y - cy
            if dx * dx + dy * dy > r * r:
                continue
            pixels[y][x] = CARD

    # 顶部红色标头（日历的装订条）
    for y in range(card_y, card_y + 14):
        for x in range(card_x, card_x + card_w):
            if pixels[y][x] == CARD:
                pixels[y][x] = HEADER

    # 两个挂环
    for hx in (card_x + 18, card_x + 50):
        for y in range(card_y - 4, card_y + 3):
            for x in range(hx - 2, hx + 3):
                if 0 <= y < SIZE and 0 <= x < SIZE and mask[y][x]:
                    pixels[y][x] = (245, 221, 216)

    # 日期格子：3 行 4 列的小方块，其中一格用主题色（"今天"）
    gx0, gy0 = card_x + 9, card_y + 22
    cell, gap = 11, 5
    today_cell = (2, 1)
    for row in range(3):
        for col in range(4):
            x0 = gx0 + col * (cell + gap)
            y0 = gy0 + row * (cell + gap)
            color = ACCENT if (col, row) == today_cell else GRID
            for y in range(y0, y0 + cell):
                for x in range(x0, x0 + cell):
                    if 0 <= y < SIZE and 0 <= x < SIZE and pixels[y][x] == CARD:
                        pixels[y][x] = color

    # 编码 PNG（RGBA，圆角外透明）
    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)  # filter type 0
        for x in range(SIZE):
            r, g, b = pixels[y][x]
            alpha = 255 if mask[y][x] else 0
            raw.extend((r, g, b, alpha))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(png)
    print(f"wrote {OUT} ({len(png)} bytes)")


if __name__ == "__main__":
    main()
