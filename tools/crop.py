#!/usr/bin/env python3
"""Обрезать и увеличить кусок PNG — чтобы интерфейс можно было РАССМОТРЕТЬ.

ЗАЧЕМ ЭТО ЛЕЖИТ В РЕПОЗИТОРИИ. Снимок экрана в натуральную величину
не показывает типографику. Полдюжины дефектов шрифта — мелкий кегль,
дробные координаты, широкий пробел, нечитаемый значок — жили в игре
и не были заметны ни на одном снимке 1600x900. Все стали очевидны
на первом же кадре, увеличенном вдвое и обрезанном по одной панели.

То же с полупрозрачной кромкой панели: на чёрном фоне её не видно
вовсе, поэтому здесь есть подложка в клетку — альфа обязана быть видна
глазом, а не выводиться из чисел.

Без PIL: пакет не входит в зависимости проекта, и тащить его ради
одного инструмента отладки — плохая сделка. PNG без чересстрочности
читается и пишется тремя десятками строк на стандартной библиотеке.

Примеры:

    tools/crop.py кадр.png верх.png --rect 0 0 900 60 --zoom 3
    tools/crop.py assets/build/ui.png плитки.png --zoom 4 --checker
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib


def read_png(path: str) -> tuple[int, int, list[bytearray]]:
    """Вернуть ширину, высоту и строки RGBA по восемь бит на канал."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: это не PNG")

    width = height = 0
    depth = color = interlace = 0
    palette: bytes = b""
    transparency: bytes = b""
    idat = bytearray()

    offset = 8
    while offset < len(data):
        (length,) = struct.unpack(">I", data[offset:offset + 4])
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        offset += 12 + length

        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            transparency = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    if depth != 8:
        raise SystemExit(f"{path}: поддерживается только 8 бит на канал, здесь {depth}")
    if interlace != 0:
        raise SystemExit(f"{path}: чересстрочный PNG не поддерживается")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color)
    if channels is None:
        raise SystemExit(f"{path}: неизвестный тип цвета {color}")

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows: list[bytearray] = []
    previous = bytearray(stride)
    position = 0
    for _ in range(height):
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position:position + stride])
        position += stride
        _unfilter(filter_type, line, previous, channels)
        rows.append(line)
        previous = line

    return width, height, [_to_rgba(row, color, channels, palette, transparency)
                           for row in rows]


def _unfilter(kind: int, line: bytearray, previous: bytearray, channels: int) -> None:
    """Снять фильтр строки. Пять видов, как в спецификации PNG."""
    if kind == 0:
        return
    for i in range(len(line)):
        left = line[i - channels] if i >= channels else 0
        up = previous[i]
        upper_left = previous[i - channels] if i >= channels else 0
        if kind == 1:
            line[i] = (line[i] + left) & 0xFF
        elif kind == 2:
            line[i] = (line[i] + up) & 0xFF
        elif kind == 3:
            line[i] = (line[i] + (left + up) // 2) & 0xFF
        elif kind == 4:
            estimate = left + up - upper_left
            da = abs(estimate - left)
            db = abs(estimate - up)
            dc = abs(estimate - upper_left)
            best = left if (da <= db and da <= dc) else (up if db <= dc else upper_left)
            line[i] = (line[i] + best) & 0xFF
        else:
            raise SystemExit(f"неизвестный фильтр строки {kind}")


def _to_rgba(row: bytearray, color: int, channels: int,
             palette: bytes, transparency: bytes) -> bytearray:
    if color == 6:
        return row
    count = len(row) // channels
    out = bytearray(count * 4)
    for i in range(count):
        source = row[i * channels:(i + 1) * channels]
        if color == 0:      # серый
            r = g = b = source[0]
            a = 255
        elif color == 2:    # RGB
            r, g, b = source
            a = 255
        elif color == 4:    # серый с альфой
            r = g = b = source[0]
            a = source[1]
        else:               # палитра
            index = source[0]
            r, g, b = palette[index * 3:index * 3 + 3]
            a = transparency[index] if index < len(transparency) else 255
        out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return out


def write_png(path: str, width: int, height: int, rows: list[bytearray]) -> None:
    raw = bytearray()
    for row in rows:
        raw.append(0)  # фильтр «нет»: файл отладочный, размер не важен
        raw += row

    def chunk(kind: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        handle.write(chunk(b"IEND", b""))


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source")
    parser.add_argument("target")
    parser.add_argument("--rect", nargs=4, type=int, metavar=("X", "Y", "W", "H"),
                        help="кусок исходника; без него берётся весь кадр")
    parser.add_argument("--zoom", type=int, default=2,
                        help="во сколько раз увеличить (ближайший сосед)")
    parser.add_argument("--checker", action="store_true",
                        help="подложка в клетку: так видно полупрозрачность")
    args = parser.parse_args()

    width, height, rows = read_png(args.source)

    x, y, w, h = args.rect if args.rect else (0, 0, width, height)
    x = max(0, min(x, width))
    y = max(0, min(y, height))
    w = max(1, min(w, width - x))
    h = max(1, min(h, height - y))

    zoom = max(1, args.zoom)
    out: list[bytearray] = []
    for row_index in range(h * zoom):
        source = rows[y + row_index // zoom]
        line = bytearray(w * zoom * 4)
        for column in range(w * zoom):
            sx = (x + column // zoom) * 4
            r, g, b, a = source[sx:sx + 4]
            if args.checker and a < 255:
                # Клетка в шестнадцать пикселей ЭКРАНА, а не исходника:
                # иначе при увеличении она превращается в широкие полосы
                # и мешает смотреть на то, ради чего всё затевалось.
                light = ((column // 16) + (row_index // 16)) % 2 == 0
                back = 96 if light else 64
                alpha = a / 255.0
                r = int(r * alpha + back * (1.0 - alpha))
                g = int(g * alpha + back * (1.0 - alpha))
                b = int(b * alpha + back * (1.0 - alpha))
                a = 255
            line[column * 4:column * 4 + 4] = bytes((r, g, b, a))
        out.append(line)

    write_png(args.target, w * zoom, h * zoom, out)
    print(f"{args.target}: {w}x{h} из {args.source} с увеличением {zoom}x "
          f"→ {w * zoom}x{h * zoom}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
