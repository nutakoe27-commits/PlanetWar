"""Планеты PlanetWar: сетки и текстуры поверхности из Blender.

Вид системы — единственное место игры, где на объект СМОТРЯТ, а не читают
его с карты. Поэтому планета здесь настоящая: сфера с развёрткой, запечённая
текстура поверхности, кольца у газовых гигантов, каркас у станции.

Ничего из этого не рисуется кодом на лету — ни кругом из шейдера, ни
градиентом. В графике игры вектора нет: геометрия строится в Blender,
поверхность печётся Cycles в равнопромежуточную (equirectangular) карту,
и в игру уезжают только сетка и растр.

Почему развёртка равнопромежуточная: у UV-сферы Blender она такая по
построению — долгота идёт по U, широта по V. Значит текстуру можно печь
прямо в UV-карту объекта, без ручного разворачивания и без швов в неожиданных
местах: шов ровно один, по нулевому меридиану, и он уходит на ночную сторону
при любом положении светила.
"""

from __future__ import annotations

import math
import os
import struct

import bpy

# Разрешение карты поверхности. Отношение 2:1 — требование равнопромежуточной
# развёртки: полный оборот по долготе против половины по широте.
TEXTURE_WIDTH = 1024
TEXTURE_HEIGHT = 512

# Плотность сетки сферы. Шестьдесят четыре сегмента дают силуэт, на котором
# гранёность не читается даже когда планета занимает половину экрана,
# и всего около четырёх тысяч треугольников — для десятка тел в кадре
# это ничто.
SPHERE_SEGMENTS = 64
SPHERE_RINGS = 32


def srgb_to_linear(channel: float) -> float:
    """Перевести цвет из sRGB, каким его видит человек, в линейный.

    Blender считает свет В ЛИНЕЙНОМ ПРОСТРАНСТВЕ, а палитры пишет человек,
    глядя на экран, то есть в sRGB. Без перевода 0.09 «почти чёрного»
    превращается на выходе в 0.33 — уверенный средне-серый, и вулканический
    мир получается пыльно-бежевым вместо базальтового. Поймали именно так:
    по запечённой карте, а не по коду.
    """
    if channel <= 0.04045:
        return channel / 12.92
    return ((channel + 0.055) / 1.055) ** 2.4


class PlanetSpec:
    """Как выглядит планета этого класса.

    Числа — это внешность, а не баланс: баланс живёт в pw_sim и по сети
    не ездит. Здесь только то, что видно глазом.
    """

    def __init__(self, ident, name, palette, roughness, detail,
                 bands=0.0, gloss=0.15, ring=False, ice=0.0, grain=0.12,
                 contrast=0.0):
        self.id = ident
        self.name = name
        # Палитра поверхности: список (положение, цвет). Не два цвета,
        # а лестница — иначе планета выходит двухтонной аппликацией.
        # Глубокая вода, отмель, берег, суша — это ЧЕТЫРЕ разных цвета,
        # и именно переходы между ними глаз читает как рельеф.
        self.palette = palette
        self.roughness = roughness  # масштаб шума: мелкий или крупный рельеф
        self.detail = detail        # число октав шума
        self.bands = bands          # сила широтной полосатости (газовые гиганты)
        self.gloss = gloss          # блик; у океана он есть, у камня нет
        self.ring = ring            # кольцо вокруг планеты
        self.ice = ice              # доля широты, занятая полярными шапками
        self.grain = grain          # сила мелкой зернистости поверхности
        self.contrast = contrast    # подъём контраста палитры


def planet_specs() -> list[PlanetSpec]:
    """Порядок ОБЯЗАН совпадать с PlanetClass из engine/sim/galaxy.h.

    Движок адресует текстуру и сетку номером класса, и разойдись порядок —
    океанический мир получил бы поверхность выжженного камня. Совпадение
    проверяется тестом, а не памятью человека.
    """
    return [
        # Выжженный: старая кора, кратеры, пыль в низинах.
        PlanetSpec("barren", "Выжженный", [
            (0.00, (0.13, 0.12, 0.11)),
            (0.38, (0.27, 0.24, 0.22)),
            (0.62, (0.42, 0.38, 0.34)),
            (1.00, (0.58, 0.54, 0.49)),
        ], roughness=6.0, detail=9.0, gloss=0.04, grain=0.22),

        # Пустынный: дюны, обнажённая порода, солончаки.
        PlanetSpec("desert", "Пустынный", [
            (0.00, (0.38, 0.24, 0.13)),
            (0.34, (0.66, 0.45, 0.22)),
            (0.66, (0.84, 0.66, 0.38)),
            (1.00, (0.93, 0.84, 0.63)),
        ], roughness=4.0, detail=7.0, gloss=0.07, grain=0.15),

        # Океанический: глубина, отмель, берег, лес. Именно переходы между
        # ними и делают из шара живую планету.
        PlanetSpec("ocean", "Океанический", [
            (0.00, (0.03, 0.10, 0.30)),
            (0.40, (0.06, 0.24, 0.52)),
            (0.50, (0.16, 0.52, 0.66)),
            (0.545, (0.78, 0.72, 0.50)),
            (0.60, (0.20, 0.45, 0.20)),
            (0.80, (0.11, 0.31, 0.14)),
            (1.00, (0.38, 0.42, 0.30)),
        ], roughness=2.4, detail=9.0, gloss=0.75, ice=0.93, grain=0.10,
           contrast=0.35),

        # Вулканический: базальт и раскалённые трещины.
        #
        # Лава занимает считанные проценты поверхности, и это принципиально:
        # первая версия подняла контраст, вся планета ушла в оранжевое,
        # и вулканический мир стало не отличить от звезды.
        PlanetSpec("volcanic", "Вулканический", [
            (0.00, (0.07, 0.06, 0.06)),
            (0.52, (0.17, 0.14, 0.13)),
            (0.74, (0.34, 0.16, 0.10)),
            (0.88, (0.78, 0.26, 0.05)),
            (1.00, (1.00, 0.76, 0.30)),
        ], roughness=5.5, detail=10.0, gloss=0.18, grain=0.18),

        # Газовый гигант: широтные полосы, закрученные шумом.
        PlanetSpec("gas_giant", "Газовый гигант", [
            (0.00, (0.30, 0.19, 0.14)),
            (0.22, (0.62, 0.34, 0.22)),
            (0.42, (0.78, 0.62, 0.40)),
            (0.60, (0.90, 0.84, 0.70)),
            (0.78, (0.60, 0.60, 0.62)),
            (1.00, (0.94, 0.90, 0.82)),
        ], roughness=1.8, detail=6.0, bands=9.0, gloss=0.10, ring=True,
           grain=0.06),

        # Пояс астероидов: крупный щебень без атмосферы.
        PlanetSpec("asteroid_belt", "Пояс астероидов", [
            (0.00, (0.14, 0.13, 0.12)),
            (0.45, (0.30, 0.28, 0.26)),
            (0.75, (0.46, 0.43, 0.40)),
            (1.00, (0.60, 0.56, 0.52)),
        ], roughness=14.0, detail=10.0, gloss=0.04, grain=0.30),

        # Станция: панели корпуса и светящиеся окна.
        PlanetSpec("station", "Станция", [
            (0.00, (0.22, 0.24, 0.27)),
            (0.45, (0.48, 0.51, 0.55)),
            (0.70, (0.66, 0.69, 0.72)),
            (0.86, (0.30, 0.34, 0.38)),
            (1.00, (0.35, 0.78, 0.90)),
        ], roughness=22.0, detail=3.0, gloss=0.50, grain=0.08),
    ]


# ---------------------------------------------------------------------------
# Геометрия
# ---------------------------------------------------------------------------


def build_sphere(name: str, radius: float = 1.0):
    """UV-сфера с развёрткой, готовой под запекание.

    Радиус единичный: масштаб планеты задаёт движок, и печь сферу нужного
    размера значило бы иметь по сетке на каждый класс ради одного числа.
    """
    bpy.ops.mesh.primitive_uv_sphere_add(segments=SPHERE_SEGMENTS,
                                         ring_count=SPHERE_RINGS,
                                         radius=radius)
    obj = bpy.context.object
    obj.name = name
    # Сглаженные нормали: гранёный шар — это шар, у которого видно, что он
    # многоугольник. Освещение в движке берёт нормали из сетки, и здесь
    # решается, будет ли терминатор гладким.
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    return obj


def build_ring(name: str, inner: float = 1.5, outer: float = 2.4):
    """Плоское кольцо газового гиганта.

    Отдельным объектом, а не частью планеты: у кольца своя текстура,
    своя прозрачность и свой материал, и склеивать их в одну сетку значило
    бы либо потерять прозрачность, либо тащить её на всю планету.
    """
    bpy.ops.mesh.primitive_circle_add(vertices=96, radius=outer, fill_type="NGON")
    obj = bpy.context.object
    obj.name = name

    # Вырезаем середину: круг превращается в кольцо выдавливанием внутрь.
    mesh = obj.data
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.delete(type="ONLY_FACE")
    bpy.ops.object.mode_set(mode="OBJECT")

    # Строим кольцо руками: два круга вершин и полоса четырёхугольников
    # между ними. Так надёжнее булевых операций, которые в headless-Blender
    # зависят от состояния выделения.
    vertices = []
    faces = []
    uvs = []
    steps = 96
    for step in range(steps):
        angle = 2.0 * math.pi * step / steps
        cos_a, sin_a = math.cos(angle), math.sin(angle)
        vertices.append((inner * cos_a, inner * sin_a, 0.0))
        vertices.append((outer * cos_a, outer * sin_a, 0.0))
    for step in range(steps):
        a = 2 * step
        b = 2 * step + 1
        c = (2 * step + 3) % (2 * steps)
        d = (2 * step + 2) % (2 * steps)
        u0 = step / steps
        u1 = (step + 1) / steps

        faces.append((a, b, c, d))
        uvs.append(((u0, 0.0), (u0, 1.0), (u1, 1.0), (u1, 0.0)))
        # И та же грань наизнанку. Кольцо ДВУСТОРОННЕЕ: движок отсекает
        # задние грани, и односторонний диск исчезал бы целиком, стоит
        # игроку опустить камеру под плоскость орбит.
        faces.append((d, c, b, a))
        uvs.append(((u1, 0.0), (u1, 1.0), (u0, 1.0), (u0, 0.0)))

    mesh.clear_geometry()
    mesh.from_pydata(vertices, [], faces)
    mesh.update()

    layer = mesh.uv_layers.new(name="UVMap")
    for face_index, face in enumerate(mesh.polygons):
        for corner, loop_index in enumerate(face.loop_indices):
            layer.data[loop_index].uv = uvs[face_index][corner]

    for polygon in mesh.polygons:
        polygon.use_smooth = True
    return obj


# Постройки на поверхности планеты.
#
# Смысл не декоративный. Захватывают теперь планеты, и игрок обязан видеть,
# ЧТО он захватывает: обжитой мир с верфью и крепостью или голый камень.
# Список в панели этого не даёт — цифры читаются, а не узнаются, и на
# десятке тел взгляд по ним не пробежит.
# ПОРЯДОК СОВПАДАЕТ С sim::Building, и это проверяется тестом: класс
# здания — индекс в этом списке, и переставленная строка выдала бы шахте
# силуэт верфи. Первые семь производят, остальные пять держат оборону
# и логистику — их силуэты намеренно «тяжёлые», без труб и мачт.
STRUCTURE_IDS = [
    "mine", "power", "foundry", "lab", "trade", "fortress", "shipyard",
    "depot", "shield", "drydock", "habitat", "garrison",
]


def build_structure(kind: str, name: str):
    """Одна постройка. Мелкая: на экране она размером с пиксель-другой,
    и подробность здесь ушла бы впустую. Важен СИЛУЭТ — по нему тип
    здания и узнаётся."""
    parts = []

    def cube(size, scale, location, rotation=(0.0, 0.0, 0.0)):
        bpy.ops.mesh.primitive_cube_add(size=size, location=location, rotation=rotation)
        obj = bpy.context.object
        obj.scale = scale
        parts.append(obj)
        return obj

    def cylinder(radius, depth, location, rotation=(0.0, 0.0, 0.0), vertices=12):
        bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=depth,
                                            location=location, rotation=rotation)
        parts.append(bpy.context.object)
        return bpy.context.object

    def dome(radius, location):
        bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=8, radius=radius,
                                             location=location)
        obj = bpy.context.object
        obj.scale = (1.0, 1.0, 0.62)
        parts.append(obj)
        return obj

    if kind == "mine":
        # Карьер: широкое основание и подъёмник над ним.
        cylinder(0.5, 0.16, (0.0, 0.0, 0.08), vertices=8)
        cube(0.2, (1.0, 1.0, 3.4), (0.18, 0.0, 0.34))
        cube(0.16, (2.6, 0.6, 0.6), (-0.1, 0.0, 0.62))
    elif kind == "power":
        # Реактор: башня с куполом.
        cylinder(0.26, 0.7, (0.0, 0.0, 0.35))
        dome(0.3, (0.0, 0.0, 0.7))
    elif kind == "foundry":
        # Литейная: корпус и две трубы.
        cube(0.5, (1.2, 0.8, 0.7), (0.0, 0.0, 0.18))
        cylinder(0.09, 0.9, (0.18, 0.12, 0.55))
        cylinder(0.09, 0.7, (-0.14, -0.1, 0.45))
    elif kind == "lab":
        # Лаборатория: низкое основание и большой купол.
        cube(0.6, (1.0, 1.0, 0.22), (0.0, 0.0, 0.07))
        dome(0.36, (0.0, 0.0, 0.16))
    elif kind == "trade":
        # Торговый узел: посадочная площадка с мачтой.
        cylinder(0.55, 0.1, (0.0, 0.0, 0.05), vertices=16)
        cylinder(0.06, 0.9, (0.0, 0.0, 0.5))
        cube(0.12, (2.4, 2.4, 0.4), (0.0, 0.0, 0.9))
    elif kind == "fortress":
        # Крепость: приземистый бункер с башней.
        cube(0.62, (1.0, 1.0, 0.4), (0.0, 0.0, 0.12))
        cylinder(0.2, 0.36, (0.0, 0.0, 0.38), vertices=8)
        cylinder(0.05, 0.5, (0.0, 0.18, 0.5), rotation=(math.radians(70.0), 0.0, 0.0))
    elif kind == "shipyard":
        # Верфь: стапель из двух опор и балки.
        cube(0.16, (1.0, 1.0, 6.0), (-0.35, 0.0, 0.48))
        cube(0.16, (1.0, 1.0, 6.0), (0.35, 0.0, 0.48))
        cube(0.16, (5.2, 1.0, 1.0), (0.0, 0.0, 0.92))
        cube(0.3, (1.0, 0.5, 0.5), (0.0, 0.0, 0.5))
    elif kind == "depot":
        # Узел снабжения: ряд цистерн и эстакада между ними. Силуэт
        # склада, а не завода: у него нет ни труб, ни мачты.
        for offset in (-0.26, 0.0, 0.26):
            cylinder(0.13, 0.46, (offset, 0.0, 0.23), rotation=(math.radians(90.0), 0.0, 0.0))
        cube(0.16, (5.0, 1.2, 0.5), (0.0, 0.0, 0.5))
        cube(0.14, (1.0, 1.0, 1.6), (-0.34, 0.0, 0.11))
        cube(0.14, (1.0, 1.0, 1.6), (0.34, 0.0, 0.11))
    elif kind == "shield":
        # Планетарный щит: три эмиттера кольцом и купол поля над ними.
        # Купол читается сразу и не спорит ни с одним другим силуэтом.
        # Эмиттеры стоят СНАРУЖИ купола, а не под ним: купол под ними
        # сливался бы с куполом лаборатории, и два разных здания читались
        # бы одинаково — а на поверхности планеты они размером с пиксель.
        for index in range(3):
            angle = index * math.tau / 3.0
            cylinder(0.08, 0.72, (math.cos(angle) * 0.52, math.sin(angle) * 0.52, 0.36),
                     vertices=6)
        dome(0.3, (0.0, 0.0, 0.14))
        cylinder(0.62, 0.05, (0.0, 0.0, 0.025), vertices=18)
    elif kind == "drydock":
        # Ремонтный док: открытая рама-ковш, в которую заводят корабль.
        # Именно раскрытая: закрытый ящик был бы неотличим от литейной.
        cube(0.14, (1.0, 1.0, 5.0), (-0.4, -0.3, 0.35))
        cube(0.14, (1.0, 1.0, 5.0), (-0.4, 0.3, 0.35))
        cube(0.14, (1.0, 1.0, 5.0), (0.4, -0.3, 0.35))
        cube(0.14, (1.0, 1.0, 5.0), (0.4, 0.3, 0.35))
        cube(0.14, (6.4, 1.0, 1.0), (0.0, -0.3, 0.68))
        cube(0.14, (6.4, 1.0, 1.0), (0.0, 0.3, 0.68))
        cube(0.2, (3.4, 3.0, 0.5), (0.0, 0.0, 0.10))
    elif kind == "habitat":
        # Хабитат: башня с двумя жилыми кольцами. Единственная высокая
        # постройка в наборе — на планете он и должен бросаться в глаза,
        # потому что это самая дорогая покупка в игре.
        cylinder(0.13, 1.15, (0.0, 0.0, 0.58))
        bpy.ops.mesh.primitive_torus_add(major_radius=0.34, minor_radius=0.055,
                                         major_segments=20, minor_segments=6,
                                         location=(0.0, 0.0, 0.46))
        parts.append(bpy.context.object)
        bpy.ops.mesh.primitive_torus_add(major_radius=0.26, minor_radius=0.05,
                                         major_segments=20, minor_segments=6,
                                         location=(0.0, 0.0, 0.86))
        parts.append(bpy.context.object)
        cylinder(0.3, 0.09, (0.0, 0.0, 0.05), vertices=16)
    else:  # garrison
        # Гарнизон: казарменный блок и две вышки по углам. Не крепость:
        # у крепости одна башня по центру, у гарнизона — низкий периметр.
        cube(0.5, (1.5, 0.9, 0.44), (0.0, 0.0, 0.11))
        for x, y in ((-0.36, -0.22), (0.36, 0.22)):
            cube(0.12, (1.0, 1.0, 3.6), (x, y, 0.22))
            cube(0.18, (1.0, 1.0, 0.5), (x, y, 0.44))
        cube(0.1, (7.4, 0.5, 0.5), (0.0, -0.26, 0.06))

    bpy.ops.object.select_all(action="DESELECT")
    for part in parts:
        part.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()

    obj = bpy.context.object
    obj.name = name
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(66.0), island_margin=0.02)
    bpy.ops.object.mode_set(mode="OBJECT")
    return obj


def unwrap(obj) -> None:
    """Развернуть объект под запекание.

    Отдельной функцией, потому что переход в режим правки требует
    АКТИВНОГО объекта, а объект, построенный не оператором, активным
    не становится. Забыть об этом — значит получить «Context missing
    active object» на ровном месте, причём в середине сборки ассетов.
    """
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(66.0), island_margin=0.02)
    bpy.ops.object.mode_set(mode="OBJECT")


def build_structure_material() -> bpy.types.Material:
    """Общий материал построек: металл с потёртостями и окнами.

    Один на все семь типов: на экране постройка занимает пиксель-другой,
    и различает их СИЛУЭТ, а не поверхность. Семь текстур ради этого
    были бы платой ни за что.
    """
    material = bpy.data.materials.new("pw_structure")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()

    output = tree.nodes.new("ShaderNodeOutputMaterial")
    principled = tree.nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Roughness"].default_value = 0.45

    coord = tree.nodes.new("ShaderNodeTexCoord")
    noise = tree.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 26.0
    noise.inputs["Detail"].default_value = 6.0
    tree.links.new(coord.outputs["Object"], noise.inputs["Vector"])

    ramp = tree.nodes.new("ShaderNodeValToRGB")
    elements = ramp.color_ramp.elements
    while len(elements) > 1:
        elements.remove(elements[-1])
    palette = [
        (0.00, (0.32, 0.34, 0.38)),
        (0.45, (0.55, 0.58, 0.62)),
        (0.72, (0.72, 0.74, 0.77)),
        (0.90, (0.46, 0.50, 0.55)),
        (1.00, (0.86, 0.78, 0.52)),
    ]
    for index, (position, color) in enumerate(palette):
        element = elements[0] if index == 0 else elements.new(position)
        element.position = position
        element.color = (*(srgb_to_linear(c) for c in color), 1.0)
    tree.links.new(noise.outputs["Fac"], ramp.inputs["Fac"])

    tree.links.new(ramp.outputs["Color"], principled.inputs["Base Color"])
    tree.links.new(principled.outputs["BSDF"], output.inputs["Surface"])
    return material


def build_station(name: str):
    """Орбитальная станция: тор с осью и панелями.

    Не планета, но владеть ею можно — у чёрной дыры это единственное тело,
    и без него ценнейшая система карты выпала бы из игры.
    """
    bpy.ops.mesh.primitive_torus_add(major_radius=1.0, minor_radius=0.22,
                                     major_segments=48, minor_segments=16)
    obj = bpy.context.object
    obj.name = name

    # Ось и четыре спицы. Соединяются в одну сетку: станция в кадре одна,
    # и делить её на объекты незачем.
    parts = []
    bpy.ops.mesh.primitive_cylinder_add(vertices=16, radius=0.16, depth=1.5,
                                        rotation=(math.radians(90.0), 0.0, 0.0))
    parts.append(bpy.context.object)
    for index in range(4):
        angle = index * math.pi / 2.0
        bpy.ops.mesh.primitive_cube_add(size=0.16)
        spoke = bpy.context.object
        spoke.scale = (5.0, 0.5, 0.35)
        spoke.rotation_euler = (0.0, 0.0, angle)
        spoke.location = (0.5 * math.cos(angle), 0.5 * math.sin(angle), 0.0)
        parts.append(spoke)

    bpy.ops.object.select_all(action="DESELECT")
    for part in parts:
        part.select_set(True)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.join()

    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    # Развёртка нужна под запекание: без неё запекать некуда.
    bpy.ops.uv.smart_project(angle_limit=math.radians(66.0), island_margin=0.02)
    bpy.ops.object.mode_set(mode="OBJECT")

    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    return obj


# ---------------------------------------------------------------------------
# Материалы
# ---------------------------------------------------------------------------


def build_surface_material(spec: PlanetSpec) -> bpy.types.Material:
    """Процедурная поверхность планеты.

    Шум берётся по координатам ОБЪЕКТА, а не по развёртке: тогда узор
    не растягивается к полюсам и не рвётся на шве. Развёртка тут только
    приёмник — в неё печётся результат.

    Слоёв три, и каждый отвечает за своё:
      крупный шум  — материки, полосы, поля лавы;
      мелкий шум   — зернистость, из-за которой поверхность перестаёт
                     выглядеть размытым пятном при близкой камере;
      широта       — полярные шапки там, где они уместны.
    """
    material = bpy.data.materials.new(f"pw_planet_{spec.id}")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()

    output = tree.nodes.new("ShaderNodeOutputMaterial")
    principled = tree.nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Roughness"].default_value = 1.0 - spec.gloss

    coord = tree.nodes.new("ShaderNodeTexCoord")

    noise = tree.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = spec.roughness
    noise.inputs["Detail"].default_value = spec.detail
    noise.inputs["Roughness"].default_value = 0.55
    tree.links.new(coord.outputs["Object"], noise.inputs["Vector"])

    # --- чем управляется палитра ---
    if spec.bands > 0.0:
        # Газовый гигант — это широтные полосы, и без них он неотличим
        # от каменного шара в пятнах. Полосы берутся из координаты Z,
        # то есть из широты, а шум их коробит.
        separate = tree.nodes.new("ShaderNodeSeparateXYZ")
        tree.links.new(coord.outputs["Object"], separate.inputs["Vector"])

        warp = tree.nodes.new("ShaderNodeMath")
        warp.operation = "MULTIPLY_ADD"
        warp.inputs[1].default_value = 0.35
        tree.links.new(noise.outputs["Fac"], warp.inputs[0])
        tree.links.new(separate.outputs["Z"], warp.inputs[2])

        wave = tree.nodes.new("ShaderNodeMath")
        wave.operation = "MULTIPLY"
        wave.inputs[1].default_value = spec.bands
        tree.links.new(warp.outputs["Value"], wave.inputs[0])

        sine = tree.nodes.new("ShaderNodeMath")
        sine.operation = "SINE"
        tree.links.new(wave.outputs["Value"], sine.inputs[0])

        driver = tree.nodes.new("ShaderNodeMath")
        driver.operation = "MULTIPLY_ADD"
        driver.inputs[1].default_value = 0.5
        driver.inputs[2].default_value = 0.5
        tree.links.new(sine.outputs["Value"], driver.inputs[0])
        driver_socket = driver.outputs["Value"]
    else:
        driver_socket = noise.outputs["Fac"]

    if spec.contrast > 0.0:
        # Подъём контраста разводит соседние ступени палитры и превращает
        # мягкую кляксу в береговую линию.
        lift = tree.nodes.new("ShaderNodeMath")
        lift.operation = "MULTIPLY_ADD"
        lift.inputs[1].default_value = 1.0 + spec.contrast * 2.0
        lift.inputs[2].default_value = -spec.contrast
        tree.links.new(driver_socket, lift.inputs[0])

        clamp_node = tree.nodes.new("ShaderNodeClamp")
        tree.links.new(lift.outputs["Value"], clamp_node.inputs["Value"])
        driver_socket = clamp_node.outputs["Result"]

    ramp = tree.nodes.new("ShaderNodeValToRGB")
    elements = ramp.color_ramp.elements
    while len(elements) > 1:
        elements.remove(elements[-1])
    for index, (position, color) in enumerate(spec.palette):
        element = elements[0] if index == 0 else elements.new(position)
        element.position = position
        element.color = (*(srgb_to_linear(c) for c in color), 1.0)
    tree.links.new(driver_socket, ramp.inputs["Fac"])

    surface = ramp.outputs["Color"]

    # --- полярные шапки ---
    if spec.ice > 0.0:
        separate_ice = tree.nodes.new("ShaderNodeSeparateXYZ")
        tree.links.new(coord.outputs["Object"], separate_ice.inputs["Vector"])

        latitude = tree.nodes.new("ShaderNodeMath")
        latitude.operation = "ABSOLUTE"
        tree.links.new(separate_ice.outputs["Z"], latitude.inputs[0])

        # Край шапки коробим тем же шумом: ровная белая полоса по широте
        # читается как ошибка рендера, а не как лёд.
        rough_edge = tree.nodes.new("ShaderNodeMath")
        rough_edge.operation = "MULTIPLY_ADD"
        rough_edge.inputs[1].default_value = 0.10
        tree.links.new(noise.outputs["Fac"], rough_edge.inputs[0])
        tree.links.new(latitude.outputs["Value"], rough_edge.inputs[2])

        edge = tree.nodes.new("ShaderNodeMapRange")
        edge.inputs["From Min"].default_value = spec.ice
        edge.inputs["From Max"].default_value = spec.ice + 0.14
        edge.clamp = True
        tree.links.new(rough_edge.outputs["Value"], edge.inputs["Value"])

        ice_mix = tree.nodes.new("ShaderNodeMixRGB")
        ice_mix.inputs["Color2"].default_value = (
            *(srgb_to_linear(c) for c in (0.90, 0.94, 0.98)), 1.0)
        tree.links.new(edge.outputs["Result"], ice_mix.inputs["Fac"])
        tree.links.new(surface, ice_mix.inputs["Color1"])
        surface = ice_mix.outputs["Color"]

    # --- зернистость ---
    #
    # Без неё поверхность на близкой камере — размытое пятно: крупный шум
    # даёт материки, но не даёт ощущения материала.
    if spec.grain > 0.0:
        grain_noise = tree.nodes.new("ShaderNodeTexNoise")
        grain_noise.inputs["Scale"].default_value = spec.roughness * 9.0
        grain_noise.inputs["Detail"].default_value = 6.0
        grain_noise.inputs["Roughness"].default_value = 0.7
        tree.links.new(coord.outputs["Object"], grain_noise.inputs["Vector"])

        grain_level = tree.nodes.new("ShaderNodeMath")
        grain_level.operation = "MULTIPLY_ADD"
        grain_level.inputs[1].default_value = spec.grain
        grain_level.inputs[2].default_value = 1.0 - spec.grain * 0.5
        tree.links.new(grain_noise.outputs["Fac"], grain_level.inputs[0])

        grain_mix = tree.nodes.new("ShaderNodeMixRGB")
        grain_mix.blend_type = "MULTIPLY"
        grain_mix.inputs["Fac"].default_value = 1.0
        tree.links.new(surface, grain_mix.inputs["Color1"])

        grain_rgb = tree.nodes.new("ShaderNodeCombineXYZ")
        tree.links.new(grain_level.outputs["Value"], grain_rgb.inputs["X"])
        tree.links.new(grain_level.outputs["Value"], grain_rgb.inputs["Y"])
        tree.links.new(grain_level.outputs["Value"], grain_rgb.inputs["Z"])
        tree.links.new(grain_rgb.outputs["Vector"], grain_mix.inputs["Color2"])
        surface = grain_mix.outputs["Color"]

    tree.links.new(surface, principled.inputs["Base Color"])
    tree.links.new(principled.outputs["BSDF"], output.inputs["Surface"])
    return material


def build_ring_material() -> bpy.types.Material:
    """Кольцо: полосы разной плотности ПОПЕРЁК радиуса.

    Полосы берутся из развёртки, а не из объёмного шума. У кольца
    развёртка простая и осмысленная: U идёт по окружности, V поперёк
    радиуса, — значит полосы это функция одного V. Объёмная текстура
    здесь давала узор по углу, и кольцо выходило исчерченным поперёк
    движения, как расчёска, а не как кольцо.

    Тёмные полосы почти чёрные намеренно: в игре кольцо рисуется
    складывающимся светом, и чёрное в нём становится прозрачным само.
    Так получаются щели без отдельной карты прозрачности.
    """
    material = bpy.data.materials.new("pw_planet_ring")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()

    output = tree.nodes.new("ShaderNodeOutputMaterial")
    principled = tree.nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Roughness"].default_value = 0.9

    coord = tree.nodes.new("ShaderNodeTexCoord")
    separate = tree.nodes.new("ShaderNodeSeparateXYZ")
    tree.links.new(coord.outputs["UV"], separate.inputs["Vector"])

    # Две несоизмеримые частоты: одна даёт крупные пояса, вторая — тонкие
    # прожилки внутри них. Одна частота читается как обои в полоску.
    coarse = tree.nodes.new("ShaderNodeMath")
    coarse.operation = "MULTIPLY"
    coarse.inputs[1].default_value = 17.0
    tree.links.new(separate.outputs["Y"], coarse.inputs[0])

    coarse_sine = tree.nodes.new("ShaderNodeMath")
    coarse_sine.operation = "SINE"
    tree.links.new(coarse.outputs["Value"], coarse_sine.inputs[0])

    fine = tree.nodes.new("ShaderNodeMath")
    fine.operation = "MULTIPLY"
    fine.inputs[1].default_value = 61.0
    tree.links.new(separate.outputs["Y"], fine.inputs[0])

    fine_sine = tree.nodes.new("ShaderNodeMath")
    fine_sine.operation = "SINE"
    tree.links.new(fine.outputs["Value"], fine_sine.inputs[0])

    blend = tree.nodes.new("ShaderNodeMath")
    blend.operation = "MULTIPLY_ADD"
    blend.inputs[1].default_value = 0.35
    tree.links.new(fine_sine.outputs["Value"], blend.inputs[0])
    tree.links.new(coarse_sine.outputs["Value"], blend.inputs[2])

    level = tree.nodes.new("ShaderNodeMath")
    level.operation = "MULTIPLY_ADD"
    level.inputs[1].default_value = 0.5
    level.inputs[2].default_value = 0.5
    tree.links.new(blend.outputs["Value"], level.inputs[0])

    ramp = tree.nodes.new("ShaderNodeValToRGB")
    elements = ramp.color_ramp.elements
    while len(elements) > 1:
        elements.remove(elements[-1])
    palette = [
        (0.00, (0.02, 0.02, 0.02)),
        (0.30, (0.10, 0.09, 0.08)),
        (0.55, (0.46, 0.40, 0.33)),
        (0.80, (0.78, 0.70, 0.56)),
        (1.00, (0.92, 0.87, 0.76)),
    ]
    for index, (position, color) in enumerate(palette):
        element = elements[0] if index == 0 else elements.new(position)
        element.position = position
        element.color = (*(srgb_to_linear(c) for c in color), 1.0)
    tree.links.new(level.outputs["Value"], ramp.inputs["Fac"])

    tree.links.new(ramp.outputs["Color"], principled.inputs["Base Color"])
    tree.links.new(principled.outputs["BSDF"], output.inputs["Surface"])
    return material


# ---------------------------------------------------------------------------
# Запекание и экспорт
# ---------------------------------------------------------------------------


def bake_surface(obj, material, path: str, samples: int,
                 width: int = TEXTURE_WIDTH, height: int = TEXTURE_HEIGHT) -> None:
    """Испечь материал объекта в PNG по его развёртке.

    Печём ДИФФУЗНЫЙ цвет без освещения: свет в игре считает движок, и
    запечённая тень намертво прибила бы источник к одному положению —
    планета выглядела бы освещённой не своей звездой.
    """
    image = bpy.data.images.new(f"bake_{obj.name}", width=width, height=height, alpha=True)

    obj.data.materials.clear()
    obj.data.materials.append(material)

    tree = material.node_tree
    target = tree.nodes.new("ShaderNodeTexImage")
    target.image = image
    tree.nodes.active = target

    scene = bpy.context.scene
    scene.cycles.samples = samples
    scene.render.bake.use_pass_direct = False
    scene.render.bake.use_pass_indirect = False
    scene.render.bake.use_pass_color = True
    # Поля вокруг островов развёртки: без них по швам вылезает фон,
    # и на планете появляется тонкая тёмная линия по нулевому меридиану.
    scene.render.bake.margin = 8

    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.bake(type="DIFFUSE")

    image.filepath_raw = path
    image.file_format = "PNG"
    image.save()

    tree.nodes.remove(target)
    bpy.data.images.remove(image)


def star_surface_specs() -> list[PlanetSpec]:
    """Поверхности светил. Порядок — как в StarClass из engine/sim/galaxy.h.

    Звезда в виде системы — это шар, на который смотрят вблизи, а не точка
    на карте. Значит ей нужна поверхность: гранулы фотосферы, пятна,
    протуберанцы у края. Плоский залитый круг на таком расстоянии выдаёт
    себя мгновенно.
    """
    return [
        PlanetSpec("star_red", "Красный карлик", [
            (0.00, (0.55, 0.10, 0.04)),
            (0.45, (0.85, 0.26, 0.08)),
            (0.75, (1.00, 0.48, 0.16)),
            (1.00, (1.00, 0.74, 0.42)),
        ], roughness=9.0, detail=8.0, gloss=0.0, grain=0.20),

        PlanetSpec("star_yellow", "Жёлтая звезда", [
            (0.00, (0.86, 0.42, 0.10)),
            (0.42, (1.00, 0.72, 0.24)),
            (0.76, (1.00, 0.88, 0.52)),
            (1.00, (1.00, 0.98, 0.86)),
        ], roughness=10.0, detail=8.0, gloss=0.0, grain=0.18),

        PlanetSpec("star_blue", "Голубой гигант", [
            (0.00, (0.16, 0.34, 0.82)),
            (0.42, (0.42, 0.66, 1.00)),
            (0.76, (0.72, 0.86, 1.00)),
            (1.00, (0.96, 0.99, 1.00)),
        ], roughness=11.0, detail=8.0, gloss=0.0, grain=0.16),

        PlanetSpec("star_neutron", "Нейтронная", [
            (0.00, (0.52, 0.72, 0.92)),
            (0.45, (0.80, 0.92, 1.00)),
            (1.00, (1.00, 1.00, 1.00)),
        ], roughness=16.0, detail=6.0, gloss=0.0, grain=0.10),

        # У чёрной дыры «поверхности» нет: это аккреционный диск, и на шар
        # он ложится тёмной сердцевиной со светящимся краем.
        PlanetSpec("star_blackhole", "Чёрная дыра", [
            (0.00, (0.02, 0.01, 0.03)),
            (0.62, (0.06, 0.03, 0.08)),
            (0.88, (0.42, 0.18, 0.52)),
            (1.00, (1.00, 0.62, 0.30)),
        ], roughness=8.0, detail=9.0, gloss=0.0, grain=0.24),
    ]


def build_space_material() -> bpy.types.Material:
    """Задник: звёздное поле и туманность.

    Чёрный фон в виде системы читается как «сцена не догрузилась». Живое
    небо стоит одной текстуры и меняет ощущение картинки целиком — это
    самая дешёвая правка на весь вид системы.

    Печётся так же, как поверхность планеты, в равнопромежуточную развёртку
    сферы: в игре она натягивается изнутри на огромный шар вокруг камеры,
    и небо честно поворачивается вместе с ней.
    """
    material = bpy.data.materials.new("pw_space")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()

    output = tree.nodes.new("ShaderNodeOutputMaterial")
    principled = tree.nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Roughness"].default_value = 1.0

    coord = tree.nodes.new("ShaderNodeTexCoord")

    # --- туманность ---
    #
    # Два слоя крупного шума разного цвета. Один слой даёт равномерную
    # заливку, из которой глаз не может выделить ни формы, ни глубины.
    nebula = tree.nodes.new("ShaderNodeTexNoise")
    nebula.inputs["Scale"].default_value = 1.6
    nebula.inputs["Detail"].default_value = 8.0
    nebula.inputs["Roughness"].default_value = 0.62
    tree.links.new(coord.outputs["Object"], nebula.inputs["Vector"])

    nebula_ramp = tree.nodes.new("ShaderNodeValToRGB")
    elements = nebula_ramp.color_ramp.elements
    while len(elements) > 1:
        elements.remove(elements[-1])
    nebula_palette = [
        (0.00, (0.012, 0.014, 0.030)),
        (0.38, (0.028, 0.036, 0.078)),
        (0.58, (0.115, 0.062, 0.170)),
        (0.76, (0.045, 0.105, 0.195)),
        (1.00, (0.185, 0.110, 0.215)),
    ]
    for index, (position, color) in enumerate(nebula_palette):
        element = elements[0] if index == 0 else elements.new(position)
        element.position = position
        element.color = (*(srgb_to_linear(c) for c in color), 1.0)
    tree.links.new(nebula.outputs["Fac"], nebula_ramp.inputs["Fac"])

    # --- звёзды ---
    #
    # Мелкий шум, обрезанный по высокому порогу: остаются редкие яркие
    # точки. Порог именно высокий — небо, засыпанное звёздами равномерно,
    # выглядит шумом, а не небом.
    field = tree.nodes.new("ShaderNodeTexNoise")
    field.inputs["Scale"].default_value = 420.0
    field.inputs["Detail"].default_value = 2.0
    field.inputs["Roughness"].default_value = 0.4
    tree.links.new(coord.outputs["Object"], field.inputs["Vector"])

    threshold = tree.nodes.new("ShaderNodeMapRange")
    threshold.inputs["From Min"].default_value = 0.74
    threshold.inputs["From Max"].default_value = 0.80
    threshold.clamp = True
    tree.links.new(field.outputs["Fac"], threshold.inputs["Value"])

    stars_mix = tree.nodes.new("ShaderNodeMixRGB")
    stars_mix.inputs["Color2"].default_value = (1.0, 1.0, 1.0, 1.0)
    tree.links.new(threshold.outputs["Result"], stars_mix.inputs["Fac"])
    tree.links.new(nebula_ramp.outputs["Color"], stars_mix.inputs["Color1"])

    tree.links.new(stars_mix.outputs["Color"], principled.inputs["Base Color"])
    tree.links.new(principled.outputs["BSDF"], output.inputs["Surface"])
    return material


MESH_MAGIC = b"PWM1"


def export_mesh(obj, path: str) -> tuple[int, int]:
    """Сохранить сетку в собственный двоичный формат.

    Свой формат, а не glTF или OBJ: движку нужны ровно положение, нормаль
    и развёртка, и разбирать ради трёх атрибутов чужой контейнер значит
    тащить в pw_render либо разбор JSON с base64, либо зависимость.
    Файл коммитится, как и SPIR-V, и по той же причине: сборка не должна
    зависеть от того, стоит ли у человека Blender.

    Треугольники, и только они: индексный буфер в Vulkan другого не знает.
    """
    mesh = obj.data
    mesh.calc_loop_triangles()

    # Ключ вершины — положение, нормаль и развёртка вместе. Разные грани
    # у шва имеют общее положение, но РАЗНУЮ развёртку, и склеивать их
    # по положению значило бы протянуть текстуру через весь шар.
    index_of: dict[tuple, int] = {}
    vertices: list[tuple] = []
    indices: list[int] = []

    uv_layer = mesh.uv_layers.active
    for triangle in mesh.loop_triangles:
        for loop_index in triangle.loops:
            loop = mesh.loops[loop_index]
            position = mesh.vertices[loop.vertex_index].co
            normal = loop.normal if triangle.use_smooth else triangle.normal
            uv = uv_layer.data[loop_index].uv if uv_layer else (0.0, 0.0)

            key = (round(position.x, 5), round(position.y, 5), round(position.z, 5),
                   round(normal.x, 4), round(normal.y, 4), round(normal.z, 4),
                   round(uv[0], 5), round(uv[1], 5))
            found = index_of.get(key)
            if found is None:
                found = len(vertices)
                index_of[key] = found
                vertices.append((position.x, position.y, position.z,
                                 normal.x, normal.y, normal.z, uv[0], uv[1]))
            indices.append(found)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(MESH_MAGIC)
        handle.write(struct.pack("<II", len(vertices), len(indices)))
        for vertex in vertices:
            handle.write(struct.pack("<8f", *vertex))
        for index in indices:
            handle.write(struct.pack("<I", index))
    return len(vertices), len(indices)
