"""Ассеты интерфейса PlanetWar: рамки, кнопки, иконки.

ПОЧЕМУ ИНТЕРФЕЙС ТОЖЕ ИЗ BLENDER. Правило проекта — в графике игры вектора
нет, всё растровое и всё из моделей. Интерфейс не исключение, и это не
формальность: панель, у которой есть настоящая фаска и настоящий свет,
выглядит предметом, а нарисованный прямоугольник с градиентом — рисунком.
Разницу глаз замечает мгновенно, даже не понимая, в чём она.

ИКОНКА ЗДАНИЯ — ЭТО САМО ЗДАНИЕ. Не отдельный рисунок, а рендер той же
модели, что стоит на планете. Значит иконка и объект физически не могут
разойтись: поправили модель шахты — иконка шахты изменилась сама.
То же с кораблями.

Что печётся:
  рамка панели  — скруглённый прямоугольник с фаской, растягивается
                  по девяти частям (углы не тянутся, края тянутся)
  кнопка        — три состояния: обычное, под курсором, нажатое
  полоса        — подложка и заливка индикатора
  слот          — пустая и занятая ячейка застройки
  иконки        — ресурсы, здания, корпуса, состояния

На выходе один PNG и манифест с прямоугольниками и полями растяжки.
"""

from __future__ import annotations

import math
import os

import bpy

# Размер клетки иконки. Девяносто шесть пикселей хватает и на панель
# застройки, и на подсказку: интерфейс рисуется в пикселях экрана, и
# на 4K иконка размером с палец занимает как раз около сотни.
ICON_SIZE = 96

# Сторона плитки рамки. Из неё режутся девять частей: угол 24, край
# растягивается. Меньше — фаска перестаёт читаться, больше — впустую.
PANEL_SIZE = 96
PANEL_BORDER = 24


def _srgb(channel: float) -> float:
    """sRGB -> линейное. Blender считает свет в линейном пространстве,
    а палитры человек пишет глядя на экран."""
    if channel <= 0.04045:
        return channel / 12.92
    return ((channel + 0.055) / 1.055) ** 2.4


def _rgba(color, alpha: float = 1.0):
    return (*(_srgb(c) for c in color), alpha)


# ---------------------------------------------------------------------------
# Материалы
# ---------------------------------------------------------------------------


def flat_material(name: str, color, alpha: float = 1.0, emission: float = 1.0):
    """Плоский светящийся материал.

    Интерфейс не освещается сценой: его вид не имеет права зависеть
    от расстановки ламп. Свет и объём в панели дают фаска и запечённая
    тень, а не источник в кадре.
    """
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()

    output = tree.nodes.new("ShaderNodeOutputMaterial")
    shader = tree.nodes.new("ShaderNodeEmission")
    shader.inputs["Color"].default_value = _rgba(color, 1.0)
    shader.inputs["Strength"].default_value = emission

    if alpha >= 1.0:
        tree.links.new(shader.outputs["Emission"], output.inputs["Surface"])
        return material

    transparent = tree.nodes.new("ShaderNodeBsdfTransparent")
    mix = tree.nodes.new("ShaderNodeMixShader")
    mix.inputs["Fac"].default_value = alpha
    tree.links.new(transparent.outputs["BSDF"], mix.inputs[1])
    tree.links.new(shader.outputs["Emission"], mix.inputs[2])
    tree.links.new(mix.outputs["Shader"], output.inputs["Surface"])
    return material


def metal_material(name: str, color, roughness: float = 0.35):
    """Материал для иконок-моделей: чуть металлический, чтобы фаски читались."""
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()

    output = tree.nodes.new("ShaderNodeOutputMaterial")
    principled = tree.nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Base Color"].default_value = _rgba(color)
    principled.inputs["Roughness"].default_value = roughness
    principled.inputs["Metallic"].default_value = 0.35
    tree.links.new(principled.outputs["BSDF"], output.inputs["Surface"])
    return material


# ---------------------------------------------------------------------------
# Геометрия панелей
# ---------------------------------------------------------------------------


def rounded_plate(name: str, width: float, height: float, radius: float,
                  bevel: float, thickness: float = 0.12):
    """Скруглённая пластина с фаской.

    Скругление делается кольцом вершин по дуге, а не модификатором:
    модификаторы в headless-Blender зависят от порядка применения
    и от того, что было выделено, — а сборка ассетов обязана давать
    одинаковый результат при каждом запуске.
    """
    segments = 8
    half_w = width * 0.5 - radius
    half_h = height * 0.5 - radius

    outline = []
    corners = [(half_w, half_h, 0.0), (-half_w, half_h, math.pi * 0.5),
               (-half_w, -half_h, math.pi), (half_w, -half_h, math.pi * 1.5)]
    for cx, cy, start in corners:
        for step in range(segments + 1):
            angle = start + math.pi * 0.5 * step / segments
            outline.append((cx + radius * math.cos(angle),
                            cy + radius * math.sin(angle)))

    vertices = []
    faces = []

    # Верхняя грань: центр плюс веер.
    top_z = thickness * 0.5
    vertices.append((0.0, 0.0, top_z))
    inner_start = len(vertices)
    shrink = max(0.0, 1.0 - bevel / max(radius, 1e-3))
    for x, y in outline:
        vertices.append((x * shrink, y * shrink, top_z))
    count = len(outline)
    for i in range(count):
        faces.append((0, inner_start + i, inner_start + (i + 1) % count))

    # Фаска: от внутреннего контура к внешнему, чуть ниже.
    outer_start = len(vertices)
    for x, y in outline:
        vertices.append((x, y, top_z - bevel))
    for i in range(count):
        a = inner_start + i
        b = inner_start + (i + 1) % count
        c = outer_start + (i + 1) % count
        d = outer_start + i
        faces.append((a, b, c, d))

    # Бортик вниз, чтобы у панели была толщина, а не бумажный край.
    bottom_start = len(vertices)
    for x, y in outline:
        vertices.append((x, y, -thickness * 0.5))
    for i in range(count):
        a = outer_start + i
        b = outer_start + (i + 1) % count
        c = bottom_start + (i + 1) % count
        d = bottom_start + i
        faces.append((a, b, c, d))

    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


# ---------------------------------------------------------------------------
# Иконки-модели
# ---------------------------------------------------------------------------


def resource_icon(kind: str, name: str):
    """Значок ресурса. Предмет, а не пиктограмма: кристалл, слиток, ячейка.

    Предмет читается быстрее знака, потому что у него есть объём и тень,
    а знак приходится сначала опознать.
    """
    parts = []

    def add(obj):
        parts.append(obj)
        return obj

    if kind == "minerals":
        # Кристалл: две пирамиды основаниями друг к другу, плюс осколки.
        bpy.ops.mesh.primitive_cone_add(vertices=6, radius1=0.42, depth=0.9,
                                        location=(0.0, 0.0, 0.28))
        add(bpy.context.object)
        bpy.ops.mesh.primitive_cone_add(vertices=6, radius1=0.42, depth=0.5,
                                        location=(0.0, 0.0, -0.22),
                                        rotation=(math.pi, 0.0, 0.0))
        add(bpy.context.object)
        bpy.ops.mesh.primitive_cone_add(vertices=6, radius1=0.2, depth=0.5,
                                        location=(0.42, -0.1, -0.2))
        add(bpy.context.object)
    elif kind == "alloys":
        # Слиток: усечённая призма, сверху ещё один.
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, -0.2))
        obj = add(bpy.context.object)
        obj.scale = (0.9, 0.5, 0.22)
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.12))
        obj = add(bpy.context.object)
        obj.scale = (0.72, 0.42, 0.2)
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.42))
        obj = add(bpy.context.object)
        obj.scale = (0.54, 0.34, 0.18)
    elif kind == "energy":
        # Ячейка: цилиндр с контактом и ободком.
        bpy.ops.mesh.primitive_cylinder_add(vertices=16, radius=0.42, depth=0.9)
        add(bpy.context.object)
        bpy.ops.mesh.primitive_cylinder_add(vertices=16, radius=0.16, depth=0.2,
                                            location=(0.0, 0.0, 0.52))
        add(bpy.context.object)
        bpy.ops.mesh.primitive_torus_add(major_radius=0.44, minor_radius=0.06,
                                         major_segments=20, minor_segments=8,
                                         location=(0.0, 0.0, 0.1))
        add(bpy.context.object)
    elif kind == "research":
        # Колба: шар на ножке.
        bpy.ops.mesh.primitive_uv_sphere_add(segments=20, ring_count=12, radius=0.4,
                                             location=(0.0, 0.0, -0.12))
        add(bpy.context.object)
        bpy.ops.mesh.primitive_cylinder_add(vertices=14, radius=0.14, depth=0.6,
                                            location=(0.0, 0.0, 0.4))
        add(bpy.context.object)
        bpy.ops.mesh.primitive_torus_add(major_radius=0.17, minor_radius=0.05,
                                         major_segments=16, minor_segments=8,
                                         location=(0.0, 0.0, 0.66))
        add(bpy.context.object)
    else:  # influence
        # Знак влияния: кольцо с тремя лучами наружу.
        bpy.ops.mesh.primitive_torus_add(major_radius=0.34, minor_radius=0.1,
                                         major_segments=24, minor_segments=10)
        add(bpy.context.object)
        for index in range(3):
            angle = index * math.pi * 2.0 / 3.0
            bpy.ops.mesh.primitive_cone_add(vertices=10, radius1=0.12, depth=0.36,
                                            location=(0.56 * math.cos(angle),
                                                      0.56 * math.sin(angle), 0.0),
                                            rotation=(math.pi * 0.5, 0.0,
                                                      -angle + math.pi * 0.5))
            add(bpy.context.object)

    bpy.ops.object.select_all(action="DESELECT")
    for part in parts:
        part.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()

    obj = bpy.context.object
    obj.name = name
    for polygon in obj.data.polygons:
        polygon.use_smooth = kind in ("research", "influence", "energy")
    return obj


def glyph_icon(kind: str, name: str):
    """Служебный значок: стрелка, крест, щит, прицел.

    Тоже объёмный, тоже с фаской. Плоский значок рядом с объёмными
    выглядит наклейкой поверх интерфейса, а не его частью.
    """
    parts = []

    def add(obj):
        parts.append(obj)
        return obj

    if kind == "plus":
        # Плюс в пустой ячейке застройки. Без него шесть тёмных квадратов
        # читаются как рамка, а не как «сюда можно нажать».
        #
        # Перекладины ТОЛСТЫЕ. Значок рисуется размером в треть ячейки,
        # то есть пикселей в двадцать; при толщине в 0,15 от значка это
        # три пикселя на штрих, и после уменьшения плюс превращался
        # в синеватое пятно. Пятая часть — минимум, при котором крест
        # ещё читается крестом.
        for rotation in (0.0, math.pi * 0.5):
            bpy.ops.mesh.primitive_cube_add(size=1.0, rotation=(0.0, 0.0, rotation))
            obj = add(bpy.context.object)
            obj.scale = (0.78, 0.22, 0.22)
    elif kind == "close":
        for sign in (1.0, -1.0):
            bpy.ops.mesh.primitive_cube_add(size=1.0, rotation=(0.0, 0.0,
                                                                sign * math.pi * 0.25))
            obj = add(bpy.context.object)
            obj.scale = (0.62, 0.14, 0.14)
    elif kind == "back":
        bpy.ops.mesh.primitive_cone_add(vertices=3, radius1=0.42, depth=0.5,
                                        rotation=(math.pi * 0.5, 0.0, math.pi * 0.5),
                                        location=(-0.2, 0.0, 0.0))
        add(bpy.context.object)
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.24, 0.0, 0.0))
        obj = add(bpy.context.object)
        obj.scale = (0.4, 0.16, 0.16)
    elif kind == "enter":
        bpy.ops.mesh.primitive_cone_add(vertices=3, radius1=0.42, depth=0.5,
                                        rotation=(math.pi * 0.5, 0.0, -math.pi * 0.5),
                                        location=(0.2, 0.0, 0.0))
        add(bpy.context.object)
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(-0.24, 0.0, 0.0))
        obj = add(bpy.context.object)
        obj.scale = (0.4, 0.16, 0.16)
    elif kind == "siege":
        # Треснувший щит: осада — это когда щит перестаёт держать.
        bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=0.5, depth=0.2,
                                            rotation=(0.0, 0.0, math.pi / 6.0))
        obj = add(bpy.context.object)
        obj.scale = (1.0, 1.12, 1.0)
        bpy.ops.mesh.primitive_cube_add(size=1.0, rotation=(0.0, 0.0, 0.35),
                                        location=(0.0, 0.0, 0.14))
        obj = add(bpy.context.object)
        obj.scale = (0.1, 1.0, 0.16)
    elif kind == "defense":
        bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=0.5, depth=0.2,
                                            rotation=(0.0, 0.0, math.pi / 6.0))
        obj = add(bpy.context.object)
        obj.scale = (1.0, 1.12, 1.0)
        bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=0.3, depth=0.24,
                                            rotation=(0.0, 0.0, math.pi / 6.0),
                                            location=(0.0, 0.0, 0.06))
        obj = add(bpy.context.object)
        obj.scale = (1.0, 1.12, 1.0)
    elif kind == "planet":
        bpy.ops.mesh.primitive_uv_sphere_add(segments=22, ring_count=14, radius=0.4)
        add(bpy.context.object)
        bpy.ops.mesh.primitive_torus_add(major_radius=0.62, minor_radius=0.05,
                                         major_segments=28, minor_segments=8,
                                         rotation=(0.42, 0.0, 0.0))
        add(bpy.context.object)
    elif kind == "fleet":
        bpy.ops.mesh.primitive_cone_add(vertices=4, radius1=0.34, depth=0.9,
                                        rotation=(math.pi * 0.5, 0.0, -math.pi * 0.5))
        add(bpy.context.object)
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(-0.28, 0.0, 0.0))
        obj = add(bpy.context.object)
        obj.scale = (0.24, 0.7, 0.12)
    elif kind == "clock":
        bpy.ops.mesh.primitive_cylinder_add(vertices=24, radius=0.46, depth=0.16)
        add(bpy.context.object)
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.14, 0.1))
        obj = add(bpy.context.object)
        obj.scale = (0.08, 0.28, 0.08)
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.12, 0.0, 0.1))
        obj = add(bpy.context.object)
        obj.scale = (0.24, 0.08, 0.08)
    else:  # demolish — мусорный бак
        # БЫЛ МОЛОТОК, и на кнопке размером в строку он читался как цифра
        # «7»: длинная ручка наискось и короткий боёк. Значок, который
        # можно спутать с цифрой, хуже отсутствующего — игрок видит
        # «7 Снести шахту» и решает, что чего-то не понимает.
        #
        # Бак узнаётся силуэтом: трапеция, крышка шире корпуса, ручка
        # сверху. Ни на что другое в наборе не похож.
        bpy.ops.mesh.primitive_cone_add(vertices=4, radius1=0.30, radius2=0.38,
                                        depth=0.62, location=(0.0, 0.0, -0.06),
                                        rotation=(0.0, 0.0, math.pi * 0.25))
        add(bpy.context.object)
        # Крышка: шире корпуса, чтобы силуэт имел уступ.
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.28))
        obj = add(bpy.context.object)
        obj.scale = (0.62, 0.62, 0.11)
        # Ручка на крышке.
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.40))
        obj = add(bpy.context.object)
        obj.scale = (0.24, 0.24, 0.14)

    bpy.ops.object.select_all(action="DESELECT")
    for part in parts:
        part.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()

    obj = bpy.context.object
    obj.name = name
    return obj


# ---------------------------------------------------------------------------
# Сцена и съёмка
# ---------------------------------------------------------------------------


def setup_icon_scene(scene) -> None:
    """Свет для иконок: ключ сверху-слева, заполнение снизу, контур сзади.

    Схема ровно та же, что у корабельных спрайтов, и это осознанно:
    иконка и объект на экране обязаны выглядеть одним и тем же предметом,
    снятым с разных сторон, а не двумя разными вещами.
    """
    scene.render.film_transparent = True
    for name, energy, rotation, color in (
        ("key", 5.0, (math.radians(48.0), 0.0, math.radians(-38.0)), (1.0, 0.97, 0.93)),
        ("fill", 2.0, (math.radians(70.0), 0.0, math.radians(128.0)), (0.70, 0.80, 1.0)),
        ("rim", 3.4, (math.radians(112.0), 0.0, math.radians(176.0)), (0.82, 0.90, 1.0)),
    ):
        data = bpy.data.lights.new(f"pw_ui_{name}", type="SUN")
        data.energy = energy
        data.color = color
        data.angle = math.radians(4.0)
        light = bpy.data.objects.new(f"pw_ui_{name}", data)
        light.rotation_euler = rotation
        bpy.context.collection.objects.link(light)


def setup_camera(scene, ortho_scale: float, elevation_deg: float):
    data = bpy.data.cameras.new("pw_ui_camera")
    data.type = "ORTHO"
    data.ortho_scale = ortho_scale

    camera = bpy.data.objects.new("pw_ui_camera", data)
    bpy.context.collection.objects.link(camera)
    scene.camera = camera

    elevation = math.radians(elevation_deg)
    distance = 10.0
    camera.location = (0.0, -distance * math.cos(elevation), distance * math.sin(elevation))
    camera.rotation_euler = (math.radians(90.0) - elevation, 0.0, 0.0)
    return camera


def render_to(scene, path: str, size: int, samples: int) -> None:
    scene.render.resolution_x = size
    scene.render.resolution_y = size
    scene.render.resolution_percentage = 100
    scene.cycles.samples = samples
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


def fit_object(obj) -> float:
    """Габарит объекта с полями: иконка не должна упираться в край клетки.

    Считается по-настоящему, а не задаётся числом. Иначе кто-нибудь однажды
    сделает модель крупнее, и иконка молча обрежется по краю — это ровно та
    ошибка, которую видно только глазами и только если посмотреть.
    """
    largest = 0.0
    for corner in obj.bound_box:
        for axis, value in enumerate(corner):
            largest = max(largest, abs(value * obj.scale[axis]))
    # Поле в тридцать пять процентов: иконки стоят вплотную в сетке
    # и без воздуха сливаются в кашу.
    return largest * 2.0 * 1.35 + 1e-4
