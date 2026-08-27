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
import pw_planets  # noqa: E402
import pw_ui  # noqa: E402
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

    # --- планеты ---
    #
    # Вид системы — единственное место игры, где на объект СМОТРЯТ, а не
    # читают его с карты. Поэтому планета здесь настоящая: сетка со сферы
    # и запечённая карта поверхности, а не круг из шейдера.
    print()
    print("  планеты...")
    build_planets(samples=max(8, samples // 2))

    # --- интерфейс ---
    print()
    print("  интерфейс...")
    build_ui(samples=max(16, samples // 2))

    if not keep_frames and os.path.isdir(WORK_DIR):
        shutil.rmtree(WORK_DIR)

    elapsed = time.time() - started
    print()
    print(f"  атлас        {atlas_size}x{atlas_size}, кадров {len(frames)}")
    for path in (albedo_png, mask_png, manifest, font_png, font_manifest):
        print(f"  {os.path.relpath(path, ROOT):40s} {os.path.getsize(path) // 1024:6d} КБ")
    print(f"\n  готово за {elapsed:.1f} с")
    return 0


def build_planets(samples: int) -> None:
    """Сетки и карты поверхности всех классов планет.

    Сцена своя: запекание требует другого состояния рендера, чем съёмка
    спрайтов, и мешать их в одной сцене — верный способ однажды испечь
    планету с тенями от корабельного света.
    """
    scene = pw_bake.reset_scene()
    scene.render.film_transparent = False

    mesh_dir = os.path.join(BUILD_DIR, "meshes")
    os.makedirs(mesh_dir, exist_ok=True)

    manifest: dict[str, object] = {
        "version": 1,
        "note": "Сгенерировано tools/blender/build_assets.py. Не редактировать руками.",
        "generator": f"blender {bpy.app.version_string}",
        "planets": [],
    }

    for index, spec in enumerate(pw_planets.planet_specs()):
        if spec.id == "station":
            obj = pw_planets.build_station(f"pw_{spec.id}")
        else:
            obj = pw_planets.build_sphere(f"pw_{spec.id}")

        material = pw_planets.build_surface_material(spec)
        texture_path = os.path.join(BUILD_DIR, f"planet_{spec.id}.png")
        pw_planets.bake_surface(obj, material, texture_path, samples=samples)

        mesh_path = os.path.join(mesh_dir, f"{spec.id}.pwm")
        vertices, indices = pw_planets.export_mesh(obj, mesh_path)

        entry = {
            "class": index,
            "id": spec.id,
            "name": spec.name,
            "mesh": os.path.relpath(mesh_path, BUILD_DIR).replace(os.sep, "/"),
            "texture": os.path.relpath(texture_path, BUILD_DIR).replace(os.sep, "/"),
            "gloss": spec.gloss,
            "ring": spec.ring,
        }
        manifest["planets"].append(entry)
        print(f"  {spec.name:16s} {vertices:5d} вершин  {indices // 3:5d} треугольников")

        bpy.data.objects.remove(obj, do_unlink=True)

    # --- светила ---
    #
    # Звезда в виде системы — шар, на который смотрят вблизи. Плоский
    # залитый круг на таком расстоянии выдаёт себя мгновенно, поэтому
    # у неё такая же поверхность, как у планеты.
    manifest["stars"] = []
    for index, spec in enumerate(pw_planets.star_surface_specs()):
        obj = pw_planets.build_sphere(f"pw_{spec.id}")
        material = pw_planets.build_surface_material(spec)
        texture_path = os.path.join(BUILD_DIR, f"{spec.id}.png")
        pw_planets.bake_surface(obj, material, texture_path, samples=samples,
                                width=512, height=256)
        manifest["stars"].append({
            "class": index,
            "id": spec.id,
            "name": spec.name,
            "texture": os.path.relpath(texture_path, BUILD_DIR).replace(os.sep, "/"),
        })
        print(f"  {spec.name:16s} карта поверхности")
        bpy.data.objects.remove(obj, do_unlink=True)

    # Кольцо — общая сетка на все планеты с кольцами: их различает поворот
    # и оттенок, а не геометрия.
    ring = pw_planets.build_ring("pw_ring")
    ring_material = pw_planets.build_ring_material()
    ring_texture = os.path.join(BUILD_DIR, "planet_ring.png")
    pw_planets.bake_surface(ring, ring_material, ring_texture, samples=samples,
                            width=512, height=64)
    ring_mesh = os.path.join(mesh_dir, "ring.pwm")
    ring_vertices, ring_indices = pw_planets.export_mesh(ring, ring_mesh)
    manifest["ring"] = {
        "mesh": os.path.relpath(ring_mesh, BUILD_DIR).replace(os.sep, "/"),
        "texture": os.path.relpath(ring_texture, BUILD_DIR).replace(os.sep, "/"),
    }
    print(f"  {'кольцо':16s} {ring_vertices:5d} вершин  {ring_indices // 3:5d} треугольников")
    bpy.data.objects.remove(ring, do_unlink=True)

    # --- постройки ---
    #
    # Игрок обязан видеть, ЧТО он захватывает: обжитой мир с верфью
    # и крепостью или голый камень. Список в панели этого не даёт —
    # цифры читаются, а не узнаются.
    manifest["structures"] = []
    structure_texture = os.path.join(BUILD_DIR, "structures.png")
    structure_material = pw_planets.build_structure_material()
    for index, kind in enumerate(pw_planets.STRUCTURE_IDS):
        obj = pw_planets.build_structure(kind, f"pw_build_{kind}")
        if index == 0:
            # Текстура одна на все постройки: различает их силуэт,
            # а не поверхность. Печём её на первой и переиспользуем.
            pw_planets.bake_surface(obj, structure_material, structure_texture,
                                    samples=samples, width=256, height=256)
        else:
            obj.data.materials.clear()
            obj.data.materials.append(structure_material)

        mesh_path = os.path.join(mesh_dir, f"build_{kind}.pwm")
        vertices, indices = pw_planets.export_mesh(obj, mesh_path)
        manifest["structures"].append({
            "building": index + 1,   # Building::None равен нулю
            "id": kind,
            "mesh": os.path.relpath(mesh_path, BUILD_DIR).replace(os.sep, "/"),
        })
        print(f"  постройка {kind:10s} {vertices:5d} вершин  {indices // 3:5d} треугольников")
        bpy.data.objects.remove(obj, do_unlink=True)

    manifest["structure_texture"] = os.path.relpath(structure_texture,
                                                    BUILD_DIR).replace(os.sep, "/")

    # --- корпуса в объёме ---
    #
    # Те же модели, что печатаются в спрайты карты, но целиком, сеткой.
    # Вид системы обязан показывать осаждающий флот: осада без видимого
    # осаждающего выглядит сломанной игрой, а не тихой угрозой.
    manifest["hulls"] = []
    hull_specs = load_specs()
    hull_materials = pw_hulls.build_materials()
    hull_texture = os.path.join(BUILD_DIR, "hulls.png")
    for index, spec in enumerate(hull_specs):
        obj = pw_hulls.build_hull_object(spec, hull_materials)
        bpy.context.view_layer.update()

        pw_planets.unwrap(obj)
        if index == 0:
            # Одна текстура на все корпуса: в системе корабль занимает
            # десяток пикселей, и различает их силуэт, а не поверхность.
            pw_planets.bake_surface(obj, pw_planets.build_structure_material(),
                                    hull_texture, samples=samples, width=256, height=256)

        mesh_path = os.path.join(mesh_dir, f"hull_{spec.id}.pwm")
        vertices, indices = pw_planets.export_mesh(obj, mesh_path)
        manifest["hulls"].append({
            "hull": index + 1,   # Hull::None равен нулю
            "id": spec.id,
            "mesh": os.path.relpath(mesh_path, BUILD_DIR).replace(os.sep, "/"),
        })
        print(f"  корпус {spec.id:12s} {vertices:5d} вершин  {indices // 3:5d} треугольников")
        bpy.data.objects.remove(obj, do_unlink=True)

    manifest["hull_texture"] = os.path.relpath(hull_texture, BUILD_DIR).replace(os.sep, "/")

    # --- задник ---
    #
    # Чёрный фон читается как «сцена не догрузилась». Живое небо стоит
    # одной текстуры и меняет ощущение картинки целиком.
    space = pw_planets.build_sphere("pw_space")
    space_texture = os.path.join(BUILD_DIR, "space.png")
    pw_planets.bake_surface(space, pw_planets.build_space_material(), space_texture,
                            samples=samples, width=2048, height=1024)
    manifest["space"] = {
        "texture": os.path.relpath(space_texture, BUILD_DIR).replace(os.sep, "/"),
    }
    print(f"  {'звёздное небо':16s} 2048x1024")
    bpy.data.objects.remove(space, do_unlink=True)

    with open(os.path.join(BUILD_DIR, "planets.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)


def build_ui(samples: int) -> None:
    """Атлас интерфейса: рамки, кнопки, полосы, слоты и иконки.

    Всё снимается как настоящие предметы: у панели есть фаска, у иконки —
    объём и тень. Нарисованный прямоугольник с градиентом глаз читает
    как рисунок поверх игры, а снятый предмет — как её часть.

    Иконка здания и иконка корабля — рендеры ТЕХ ЖЕ моделей, что стоят
    на планете и летают по карте. Значит они физически не могут разойтись
    с тем, что игрок видит в мире.
    """
    scene = pw_bake.reset_scene()
    pw_ui.setup_icon_scene(scene)

    work = os.path.join(ROOT, "assets", "cache", "ui")
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work, exist_ok=True)

    entries: list[tuple[str, int, str]] = []
    borders: dict[str, int] = {}

    def snapshot(obj, name: str, size: int, ortho: float, elevation: float):
        camera = pw_ui.setup_camera(scene, ortho_scale=ortho, elevation_deg=elevation)
        path = os.path.join(work, f"{name}.png")
        pw_ui.render_to(scene, path, size=size, samples=samples)
        entries.append((name, 0, path))
        bpy.data.objects.remove(camera, do_unlink=True)

    # --- рамки и кнопки ---
    #
    # Снимаются строго сверху: рамка растягивается по девяти частям, и
    # наклон камеры сделал бы её края непохожими друг на друга — растянутая
    # панель поехала бы вбок.
    plates = [
        # имя,            цвет подложки,          прозрачность, фаска
        ("panel",         (0.09, 0.11, 0.16),     0.92, 0.10),
        ("panel_light",   (0.14, 0.17, 0.24),     0.94, 0.10),
        ("panel_dark",    (0.05, 0.06, 0.10),     0.88, 0.08),
        ("button",        (0.17, 0.21, 0.30),     1.00, 0.13),
        ("button_hover",  (0.25, 0.33, 0.46),     1.00, 0.13),
        ("button_down",   (0.12, 0.16, 0.23),     1.00, 0.06),
        ("button_accent", (0.20, 0.42, 0.36),     1.00, 0.13),
        ("button_danger", (0.42, 0.19, 0.20),     1.00, 0.13),
        ("slot",          (0.07, 0.09, 0.13),     0.95, 0.08),
        ("slot_hover",    (0.14, 0.20, 0.28),     1.00, 0.10),
        ("bar_back",      (0.05, 0.06, 0.09),     0.95, 0.05),
    ]
    for name, color, alpha, bevel in plates:
        obj = pw_ui.rounded_plate(f"pw_ui_{name}", width=2.0, height=2.0,
                                  radius=0.42, bevel=bevel)
        obj.data.materials.append(pw_ui.flat_material(f"pw_ui_mat_{name}", color, alpha))
        snapshot(obj, name, size=pw_ui.PANEL_SIZE, ortho=2.02, elevation=90.0)
        borders[name] = pw_ui.PANEL_BORDER
        bpy.data.objects.remove(obj, do_unlink=True)

    # --- рамки в духе Stellaris: срезанные углы и светящаяся кромка ---
    #
    # Отдельный набор, а не замена старому: скруглённые пластины остались
    # под индикаторы и мелочь, где срез в четыре пикселя всё равно не виден.
    #
    # Подложки почти чёрные, кромки — холодная сталь. Это и есть весь
    # секрет космического интерфейса: цвет несёт кромка и текст, а панель
    # обязана уступить экран карте. Панель, выкрашенная в цвет, отбирает
    # внимание у того, ради чего она нарисована.
    hud_plates = [
        # имя,                подложка,               кромка,                 альфа, яркость кромки
        # Внешние рамки — светятся: им держать край поверх звёздного неба.
        ("hud_bar",           (0.020, 0.028, 0.048),  (0.24, 0.46, 0.66),     0.97, 1.30),
        ("hud_panel",         (0.030, 0.042, 0.070),  (0.21, 0.42, 0.60),     0.93, 1.25),
        ("hud_panel_deep",    (0.016, 0.023, 0.040),  (0.16, 0.32, 0.47),     0.95, 1.10),
        # Внутренности — почти не светятся. Когда светится всё, не светится
        # ничего: взгляд перестаёт различать, что здесь главное.
        ("hud_header",        (0.055, 0.080, 0.125),  (0.17, 0.30, 0.42),     0.94, 0.75),
        ("hud_row",           (0.048, 0.066, 0.100),  (0.11, 0.20, 0.30),     0.72, 0.55),
        ("hud_row_hover",     (0.100, 0.145, 0.210),  (0.22, 0.40, 0.56),     0.90, 0.80),
        ("hud_row_active",    (0.075, 0.150, 0.215),  (0.34, 0.62, 0.86),     0.95, 1.00),
        ("hud_button",        (0.062, 0.088, 0.135),  (0.20, 0.36, 0.50),     0.96, 0.85),
        ("hud_button_hover",  (0.105, 0.155, 0.220),  (0.34, 0.60, 0.84),     1.00, 1.10),
        ("hud_button_down",   (0.040, 0.058, 0.090),  (0.16, 0.29, 0.42),     1.00, 0.70),
        ("hud_button_accent", (0.065, 0.135, 0.195),  (0.34, 0.66, 0.92),     1.00, 1.20),
        ("hud_button_danger", (0.150, 0.062, 0.068),  (0.80, 0.34, 0.31),     1.00, 1.20),
        ("hud_slot",          (0.034, 0.047, 0.075),  (0.14, 0.26, 0.37),     0.90, 0.65),
        ("hud_slot_hover",    (0.082, 0.120, 0.175),  (0.32, 0.56, 0.78),     1.00, 1.00),
        ("hud_group",         (0.032, 0.045, 0.072),  (0.10, 0.19, 0.28),     0.50, 0.45),
    ]
    for name, body, rim, alpha, glow in hud_plates:
        obj = pw_ui.hud_plate(name, body=body, rim=rim, alpha=alpha, rim_glow=glow)
        snapshot(obj, name, size=pw_ui.PANEL_SIZE, ortho=2.02, elevation=90.0)
        borders[name] = pw_ui.HUD_BORDER
        bpy.data.objects.remove(obj, do_unlink=True)

    # Заливка индикатора — без скругления по краям: она обрезается
    # по длине, и скруглённый край при обрезке выглядел бы обломанным.
    for name, color in (("bar_fill", (0.36, 0.74, 0.92)),
                        ("bar_fill_warn", (0.92, 0.72, 0.30)),
                        ("bar_fill_bad", (0.86, 0.36, 0.34)),
                        ("white", (1.0, 1.0, 1.0))):
        obj = pw_ui.rounded_plate(f"pw_ui_{name}", width=2.0, height=2.0,
                                  radius=0.06, bevel=0.03)
        obj.data.materials.append(pw_ui.flat_material(f"pw_ui_mat_{name}", color, 1.0))
        snapshot(obj, name, size=32, ortho=2.02, elevation=90.0)
        borders[name] = 4
        bpy.data.objects.remove(obj, do_unlink=True)

    # --- иконки ресурсов ---
    resources = [
        ("res_energy",    "energy",    (0.98, 0.82, 0.34)),
        ("res_minerals",  "minerals",  (0.62, 0.78, 0.92)),
        ("res_alloys",    "alloys",    (0.88, 0.62, 0.42)),
        ("res_research",  "research",  (0.58, 0.84, 0.72)),
        ("res_influence", "influence", (0.78, 0.66, 0.92)),
    ]
    for name, kind, color in resources:
        obj = pw_ui.resource_icon(kind, f"pw_ui_{name}")
        obj.data.materials.clear()
        obj.data.materials.append(pw_ui.metal_material(f"pw_ui_mat_{name}", color))
        snapshot(obj, name, size=pw_ui.ICON_SIZE,
                 ortho=pw_ui.fit_object(obj), elevation=32.0)
        bpy.data.objects.remove(obj, do_unlink=True)

    # --- иконки зданий: те же модели, что стоят на планете ---
    building_colors = {
        "mine":     (0.78, 0.72, 0.60),
        "power":    (0.62, 0.82, 0.94),
        "foundry":  (0.90, 0.66, 0.44),
        "lab":      (0.68, 0.88, 0.78),
        "trade":    (0.86, 0.80, 0.56),
        "fortress": (0.74, 0.76, 0.82),
        "shipyard": (0.70, 0.78, 0.90),
        "depot":    (0.82, 0.76, 0.52),
        "shield":   (0.60, 0.86, 0.94),
        "drydock":  (0.72, 0.80, 0.86),
        "habitat":  (0.88, 0.84, 0.92),
        "garrison": (0.80, 0.74, 0.66),
    }
    for kind in pw_planets.STRUCTURE_IDS:
        obj = pw_planets.build_structure(kind, f"pw_ui_bld_{kind}")
        obj.data.materials.clear()
        obj.data.materials.append(
            pw_ui.metal_material(f"pw_ui_mat_bld_{kind}", building_colors[kind]))
        snapshot(obj, f"bld_{kind}", size=pw_ui.ICON_SIZE,
                 ortho=pw_ui.fit_object(obj), elevation=34.0)
        bpy.data.objects.remove(obj, do_unlink=True)

    # --- иконки корпусов: те же модели, что летают по карте ---
    hull_materials = pw_hulls.build_materials()
    for spec in load_specs():
        obj = pw_hulls.build_hull_object(spec, hull_materials)
        bpy.context.view_layer.update()
        snapshot(obj, f"hull_{spec.id}", size=pw_ui.ICON_SIZE,
                 ortho=pw_bake.fit_ortho_scale(obj) * 1.15, elevation=34.0)
        bpy.data.objects.remove(obj, do_unlink=True)

    # --- служебные значки ---
    #
    # У каждого своя высота камеры, и это не украшение.
    #
    # Значок-ПРЕДМЕТ (щит, часы, бак) снимается с наклона: наклон даёт
    # объём, и предмет читается предметом. Значок-ЗНАК (уголок списка,
    # треугольник тревоги, спираль галактики) обязан сниматься ПРЯМО
    # СВЕРХУ: под наклоном знак теряет симметрию и начинает означать
    # что-то другое. Проверено на себе — уголок «свернуть», снятый
    # под сорок градусов, читался как галочка, а треугольник тревоги
    # смотрел вбок.
    glyphs = [
        # имя,            модель,        цвет,                    высота камеры
        ("icon_close",    "close",    (0.92, 0.62, 0.62), 40.0),
        ("icon_back",     "back",     (0.82, 0.86, 0.94), 40.0),
        ("icon_enter",    "enter",    (0.82, 0.90, 0.98), 40.0),
        ("icon_siege",    "siege",    (0.94, 0.52, 0.44), 40.0),
        ("icon_defense",  "defense",  (0.62, 0.86, 0.72), 40.0),
        ("icon_planet",   "planet",   (0.72, 0.84, 0.96), 40.0),
        ("icon_fleet",    "fleet",    (0.86, 0.88, 0.94), 40.0),
        ("icon_clock",    "clock",    (0.88, 0.84, 0.66), 40.0),
        ("icon_demolish", "demolish", (0.90, 0.72, 0.58), 40.0),
        ("icon_plus",     "plus",     (0.62, 0.74, 0.90), 62.0),
        # Знаки: строго сверху.
        ("icon_chevron_down",  "chevron_down",  (0.70, 0.84, 0.98), 90.0),
        ("icon_chevron_right", "chevron_right", (0.70, 0.84, 0.98), 90.0),
        ("icon_crest",         "crest",         (0.88, 0.92, 1.00), 90.0),
        ("icon_galaxy",        "galaxy",        (0.74, 0.88, 1.00), 90.0),
        ("icon_alert",         "alert",         (0.99, 0.76, 0.32), 90.0),
        ("icon_star",          "star",          (0.99, 0.90, 0.58), 90.0),
        ("icon_prestige",      "prestige",      (0.95, 0.84, 0.54), 62.0),
    ]
    for name, kind, color, elevation in glyphs:
        obj = pw_ui.glyph_icon(kind, f"pw_ui_{name}")
        obj.data.materials.clear()
        obj.data.materials.append(pw_ui.metal_material(f"pw_ui_mat_{name}", color))
        snapshot(obj, name, size=pw_ui.ICON_SIZE,
                 ortho=pw_ui.fit_object(obj), elevation=elevation)
        bpy.data.objects.remove(obj, do_unlink=True)

    atlas_png = os.path.join(BUILD_DIR, "ui.png")
    frames, atlas_size = pw_atlas.pack(entries, atlas_png)

    manifest = {
        "version": 1,
        "note": "Сгенерировано tools/blender/build_assets.py. Не редактировать руками.",
        "generator": f"blender {bpy.app.version_string}",
        "texture": os.path.basename(atlas_png),
        "atlas_size": atlas_size,
        "sprites": [
            {
                "name": frame.hull,
                "x": frame.x, "y": frame.y, "w": frame.w, "h": frame.h,
                # Поле растяжки: столько пикселей у края НЕ тянется.
                # Ноль означает обычный спрайт, который тянется целиком.
                "border": borders.get(frame.hull, 0),
            }
            for frame in frames
        ],
    }
    with open(os.path.join(BUILD_DIR, "ui.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)

    shutil.rmtree(work, ignore_errors=True)
    print(f"  интерфейс   {atlas_size}x{atlas_size}, спрайтов {len(frames)}")


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
