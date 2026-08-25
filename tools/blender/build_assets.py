#!/usr/bin/env python3
"""Сборка спрайтовых атласов PlanetWar из моделей Blender.

Запуск (Blender подключается как модуль bpy, отдельный бинарь не нужен):

    python3 tools/blender/build_assets.py --quality preview

Что происходит:
  1. Читаются спецификации корпусов — те же числа, что задают баланс.
  2. Модели строятся процедурно в Blender (tools/blender/pw_hulls.py).
  3. Каждая модель рендерится по кругу ортографической камерой под наклоном.
  4. Кадры пакуются в два атласа: цвет и маска цвета империи.
  5. Пишется JSON-манифест с раскладкой кадров.

На выходе только растровые PNG — в графике игры вектора нет.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402

import pw_atlas  # noqa: E402
import pw_bake  # noqa: E402
import pw_hulls  # noqa: E402
import pw_font  # noqa: E402
import pw_stars  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_DIR = os.path.join(ROOT, "assets", "src")
BUILD_DIR = os.path.join(ROOT, "assets", "build")
WORK_DIR = os.path.join(ROOT, "assets", "cache", "frames")

# Сколько пикселей спрайта приходится на одну игровую единицу длины.
# Общий для всех корпусов — поэтому линкор на карте ровно во столько раз
# крупнее корвета, во сколько он длиннее по данным баланса.
PIXELS_PER_UNIT = 48.0

QUALITY = {
    # Для CI: доказать, что пайплайн жив, за разумное время.
    "ci":      {"rotations": 8,  "samples": 24,  "scale": 0.5},
    # Для работы над игрой: смотреть и править.
    "preview": {"rotations": 16, "samples": 64,  "scale": 1.0},
    # Для сборки билда.
    "release": {"rotations": 32, "samples": 256, "scale": 1.0},
}


def load_specs() -> list[pw_hulls.HullSpec]:
    with open(os.path.join(SRC_DIR, "hulls.json"), encoding="utf-8") as handle:
        raw = json.load(handle)
    return [pw_hulls.HullSpec.from_dict(item) for item in raw["hulls"]]


def sprite_size_for(extent: float, scale: float) -> int:
    """Размер спрайта из реальных габаритов модели, кратно восьми.

    Считается, а не задаётся руками: иначе однажды кто-то удлинит линкор
    в таблице баланса, и модель молча обрежется по краю кадра.
    """
    pixels = extent * PIXELS_PER_UNIT * scale
    return max(32, int(math.ceil(pixels / 8.0)) * 8)


def build(quality: str, keep_frames: bool) -> int:
    settings = QUALITY[quality]
    rotations = settings["rotations"]
    samples = settings["samples"]
    scale = settings["scale"]

    specs = load_specs()
    started = time.time()

    print(f"PlanetWar — сборка ассетов")
    print(f"  Blender      {bpy.app.version_string}")
    print(f"  качество     {quality} ({rotations} поворотов, {samples} сэмплов)")
    print(f"  корпусов     {len(specs)}")
    print()

    scene = pw_bake.reset_scene()
    materials = pw_hulls.build_materials()
    mask_materials = pw_bake.make_mask_materials()
    pw_bake.setup_lights()
    camera = pw_bake.setup_camera(scene, ortho_scale=1.0)

    if os.path.isdir(WORK_DIR):
        shutil.rmtree(WORK_DIR)

    albedo_entries: list[tuple[str, int, str]] = []
    mask_entries: list[tuple[str, int, str]] = []

    for spec in specs:
        obj = pw_hulls.build_hull_object(spec, materials)
        bpy.context.view_layer.update()

        extent = pw_bake.fit_ortho_scale(obj)
        camera.data.ortho_scale = extent
        size = sprite_size_for(extent, scale)

        print(f"  {spec.display_name:10s} {spec.id:11s} "
              f"{len(obj.data.polygons):5d} полигонов  "
              f"спрайт {size}x{size}  "
              f"турелей {spec.weapon_slots}, двигателей {spec.engines}")

        for pass_name, mats, sink in (
            (pw_bake.PASS_ALBEDO, materials, albedo_entries),
            (pw_bake.PASS_MASK, mask_materials, mask_entries),
        ):
            pw_bake.swap_materials(obj, mats)
            paths = pw_bake.render_rotations(
                scene, obj, hull_id=spec.id, pass_name=pass_name,
                steps=rotations, size=size, samples=samples, out_dir=WORK_DIR)
            sink.extend((spec.id, index, path) for index, path in enumerate(paths))

        bpy.data.objects.remove(obj, do_unlink=True)

    # --- звёзды ---
    # Звезда на карте — главный элемент интерфейса: по ней читается
    # владение, ценность системы и происходящее в ней. Поэтому она
    # делается так же, как корабли, а не рисуется кругом из шейдера:
    # в графике игры вектора нет.
    print()
    for star_class, core_radius, color, strength, halo in pw_stars.star_specs():
        obj = pw_stars.build_star_object(star_class, core_radius, color, strength, halo)
        bpy.context.view_layer.update()

        extent = pw_bake.fit_ortho_scale(obj)
        camera.data.ortho_scale = extent
        size = sprite_size_for(extent, scale)

        print(f"  звезда     {star_class:11s} "
              f"{len(obj.data.polygons):5d} полигонов  спрайт {size}x{size}")

        # Звезде хватает одного ракурса: шар одинаков со всех сторон, и
        # печь для неё восемь поворотов — это восемь одинаковых кадров.
        for pass_name, sink in ((pw_bake.PASS_ALBEDO, albedo_entries),
                                (pw_bake.PASS_MASK, mask_entries)):
            if pass_name == pw_bake.PASS_MASK:
                # Звезда НЕ принимает цвет империи: её класс — свойство
                # мира, а не игрока. Маска должна быть чёрной ЦЕЛИКОМ.
                #
                # Обычная маска красит второй слот белым — у корабля это
                # акцент, а у звезды это ореол. Первая версия так и делала,
                # и все звёзды на карте выходили одинаково белыми: класс
                # светила переставал читаться вовсе.
                pw_bake.swap_materials(
                    obj, pw_bake.make_empty_mask_materials(len(obj.data.materials)))
            paths = pw_bake.render_rotations(
                scene, obj, hull_id=f"star_{star_class}", pass_name=pass_name,
                steps=1, size=size, samples=samples, out_dir=WORK_DIR)
            sink.extend((f"star_{star_class}", index, path) for index, path in enumerate(paths))

        bpy.data.objects.remove(obj, do_unlink=True)

    print()
    print("  упаковка атласов...")
    albedo_png = os.path.join(BUILD_DIR, "ships_albedo.png")
    mask_png = os.path.join(BUILD_DIR, "ships_accent_mask.png")

    frames, atlas_size = pw_atlas.pack(albedo_entries, albedo_png)
    mask_frames, mask_size = pw_atlas.pack(mask_entries, mask_png)

    # Раскладка обязана совпасть: движок адресует оба атласа одним индексом.
    if [(f.x, f.y, f.w, f.h) for f in frames] != \
       [(f.x, f.y, f.w, f.h) for f in mask_frames] or atlas_size != mask_size:
        print("ОШИБКА: раскладка albedo и mask разошлась", file=sys.stderr)
        return 1

    manifest = os.path.join(BUILD_DIR, "ships.json")
    pw_atlas.write_manifest(
        manifest, albedo=albedo_png, mask=mask_png, atlas_size=atlas_size,
        rotation_steps=rotations, camera_elevation=pw_bake.CAMERA_ELEVATION_DEG,
        frames=frames, generator=f"blender {bpy.app.version_string}")

    # --- шрифт интерфейса ---
    #
    # Отдельный атлас, а не общий со спрайтами: у глифов своя сетка и свой
    # размер клетки, и упаковывать их вместе с кораблями значило бы
    # усложнить обоим жизнь ради экономии одной текстуры.
    print()
    print("  выпечка шрифта...")
    font_png = os.path.join(BUILD_DIR, "font.png")
    font_scene = pw_bake.reset_scene()
    layout = pw_font.build_font_atlas(font_scene, font_png,
                                      cell=48, samples=max(8, samples // 2))
    font_manifest = os.path.join(BUILD_DIR, "font.json")
    with open(font_manifest, "w", encoding="utf-8") as handle:
        json.dump({
            "version": 1,
            "note": "Сгенерировано tools/blender/build_assets.py. Не редактировать руками.",
            "generator": f"blender {bpy.app.version_string}",
            **layout,
        }, handle, ensure_ascii=False, indent=2)
    print(f"  шрифт       {layout['width']}x{layout['height']}, "
          f"глифов {len(layout['charset'])}, клетка {layout['cell']}")

    if not keep_frames and os.path.isdir(WORK_DIR):
        shutil.rmtree(WORK_DIR)

    elapsed = time.time() - started
    print()
    print(f"  атлас        {atlas_size}x{atlas_size}, кадров {len(frames)}")
    for path in (albedo_png, mask_png, manifest, font_png, font_manifest):
        print(f"  {os.path.relpath(path, ROOT):40s} {os.path.getsize(path) // 1024:6d} КБ")
    print(f"\n  готово за {elapsed:.1f} с")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--quality", choices=sorted(QUALITY), default="preview",
                        help="ci — быстро для CI, preview — для работы, release — для билда")
    parser.add_argument("--keep-frames", action="store_true",
                        help="не удалять промежуточные кадры (для отладки пайплайна)")
    args = parser.parse_args()
    return build(args.quality, args.keep_frames)


if __name__ == "__main__":
    sys.exit(main())
