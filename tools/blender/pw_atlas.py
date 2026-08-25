"""Упаковка отрендеренных кадров в спрайтовые атласы.

Один атлас на проход, к нему JSON-манифест с прямоугольниками кадров.
Движок грузит два PNG и одну таблицу — вместо тысяч отдельных файлов, каждый
из которых стоил бы отдельного вызова отрисовки.

Чтение и запись идут через API изображений Blender, поэтому лишних
зависимостей вроде Pillow не появляется: bpy у нас и так есть.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, asdict

import bpy
import numpy as np


@dataclass
class Frame:
    hull: str
    rotation: int
    x: int
    y: int
    w: int
    h: int


def load_rgba(path: str) -> np.ndarray:
    """PNG -> массив (h, w, 4) float32 в привычной ориентации сверху вниз.

    Blender хранит пиксели снизу вверх, поэтому переворачиваем сразу на входе
    и дальше во всём пайплайне работаем в системе координат с началом
    в левом верхнем углу — той же, что ждёт движок.
    """
    image = bpy.data.images.load(path)
    try:
        width, height = image.size
        buffer = np.empty(width * height * 4, dtype=np.float32)
        image.pixels.foreach_get(buffer)
        return buffer.reshape(height, width, 4)[::-1].copy()
    finally:
        bpy.data.images.remove(image)


def save_rgba(pixels: np.ndarray, path: str) -> None:
    height, width = pixels.shape[:2]
    image = bpy.data.images.new(os.path.basename(path), width=width, height=height,
                                alpha=True, float_buffer=False)
    try:
        image.pixels.foreach_set(pixels[::-1].reshape(-1).astype(np.float32))
        image.file_format = "PNG"
        image.filepath_raw = path
        image.save()
    finally:
        bpy.data.images.remove(image)


def _shelf_pack(sizes: list[tuple[int, int]], atlas_size: int):
    """Полочная упаковка. Кадры одного корпуса одинаковы, так что этого хватает.

    Возвращает список позиций или None, если в атлас такого размера не влезло.
    """
    positions: list[tuple[int, int]] = [(0, 0)] * len(sizes)
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0]))

    cursor_x = cursor_y = shelf_height = 0
    for index in order:
        w, h = sizes[index]
        if w > atlas_size or h > atlas_size:
            return None
        if cursor_x + w > atlas_size:
            cursor_x = 0
            cursor_y += shelf_height
            shelf_height = 0
        if cursor_y + h > atlas_size:
            return None
        positions[index] = (cursor_x, cursor_y)
        cursor_x += w
        shelf_height = max(shelf_height, h)
    return positions


def pack(entries: list[tuple[str, int, str]], out_png: str,
         max_size: int = 4096) -> tuple[list[Frame], int]:
    """Собрать атлас из кадров.

    entries — список (hull_id, rotation_index, путь к PNG).
    Возвращает описания кадров и сторону атласа.
    """
    images = [load_rgba(path) for _, _, path in entries]
    sizes = [(img.shape[1], img.shape[0]) for img in images]

    atlas_size = 256
    positions = None
    while atlas_size <= max_size:
        positions = _shelf_pack(sizes, atlas_size)
        if positions is not None:
            break
        atlas_size *= 2
    if positions is None:
        raise RuntimeError(
            f"кадры не помещаются в атлас {max_size}x{max_size} — "
            "уменьшите sprite_size или число поворотов")

    canvas = np.zeros((atlas_size, atlas_size, 4), dtype=np.float32)
    frames = []
    for (hull, rotation, _), image, (x, y) in zip(entries, images, positions):
        h, w = image.shape[:2]
        canvas[y:y + h, x:x + w] = image
        frames.append(Frame(hull=hull, rotation=rotation, x=x, y=y, w=w, h=h))

    os.makedirs(os.path.dirname(out_png), exist_ok=True)
    save_rgba(canvas, out_png)
    return frames, atlas_size


def write_manifest(path: str, *, albedo: str, mask: str, atlas_size: int,
                   rotation_steps: int, camera_elevation: float,
                   frames: list[Frame], generator: str) -> None:
    """Манифест, который читает движок.

    Прямоугольники общие для обоих атласов: albedo и mask пекутся одной
    раскладкой, поэтому индекс кадра один и тот же.
    """
    payload = {
        "version": 1,
        "generator": generator,
        "note": "Сгенерировано tools/blender/build_assets.py. Не редактировать руками.",
        "atlas_size": atlas_size,
        "rotation_steps": rotation_steps,
        "camera": {
            "projection": "orthographic",
            "elevation_deg": camera_elevation,
        },
        "textures": {
            "albedo": os.path.basename(albedo),
            "accent_mask": os.path.basename(mask),
        },
        "frames": [asdict(f) for f in frames],
    }
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
