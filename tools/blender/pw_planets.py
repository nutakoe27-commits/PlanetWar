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


class PlanetSpec:
    """Как выглядит планета этого класса.

    Числа — это внешность, а не баланс: баланс живёт в pw_sim и по сети
    не ездит. Здесь только то, что видно глазом.
    """

    def __init__(self, ident, name, base, accent, roughness, detail,
                 bands=0.0, gloss=0.15, ring=False, clouds=0.0):
        self.id = ident
        self.name = name
        self.base = base            # основной цвет поверхности
        self.accent = accent        # цвет пятен и прожилок
        self.roughness = roughness  # масштаб шума: мелкий или крупный рельеф
        self.detail = detail        # число октав шума
        self.bands = bands          # сила широтной полосатости (газовые гиганты)
        self.gloss = gloss          # блик; у океана он есть, у камня нет
        self.ring = ring            # кольцо вокруг планеты
        self.clouds = clouds        # плотность облачного слоя


def planet_specs() -> list[PlanetSpec]:
    """Порядок ОБЯЗАН совпадать с PlanetClass из engine/sim/galaxy.h.

    Движок адресует текстуру и сетку номером класса, и разойдись порядок —
    океанический мир получил бы поверхность выжженного камня. Совпадение
    проверяется тестом, а не памятью человека.
    """
    return [
        PlanetSpec("barren", "Выжженный", (0.34, 0.30, 0.27), (0.20, 0.17, 0.15),
                   roughness=7.0, detail=6.0, gloss=0.05),
        PlanetSpec("desert", "Пустынный", (0.72, 0.54, 0.30), (0.50, 0.33, 0.17),
                   roughness=4.5, detail=5.0, gloss=0.08),
        PlanetSpec("ocean", "Океанический", (0.10, 0.32, 0.62), (0.18, 0.46, 0.28),
                   roughness=2.6, detail=7.0, gloss=0.85, clouds=0.55),
        PlanetSpec("volcanic", "Вулканический", (0.18, 0.12, 0.11), (0.85, 0.26, 0.06),
                   roughness=6.0, detail=8.0, gloss=0.20),
        PlanetSpec("gas_giant", "Газовый гигант", (0.78, 0.64, 0.44), (0.52, 0.36, 0.24),
                   roughness=2.0, detail=4.0, bands=9.0, gloss=0.10, ring=True),
        PlanetSpec("asteroid_belt", "Пояс астероидов", (0.40, 0.38, 0.36),
                   (0.24, 0.22, 0.21), roughness=12.0, detail=8.0, gloss=0.05),
        PlanetSpec("station", "Станция", (0.55, 0.58, 0.62), (0.20, 0.62, 0.75),
                   roughness=18.0, detail=2.0, gloss=0.55),
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
        faces.append((a, b, c, d))
        u0 = step / steps
        u1 = (step + 1) / steps
        uvs.append(((u0, 0.0), (u0, 1.0), (u1, 1.0), (u1, 0.0)))

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

    Шум по координатам ОБЪЕКТА, а не по развёртке: тогда узор не тянется
    к полюсам и не рвётся на шве. Развёртка тут только приёмник — в неё
    печётся результат.
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

    mixer = tree.nodes.new("ShaderNodeMixRGB")
    mixer.blend_type = "MIX"
    mixer.inputs["Color1"].default_value = (*spec.base, 1.0)
    mixer.inputs["Color2"].default_value = (*spec.accent, 1.0)

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

        level = tree.nodes.new("ShaderNodeMath")
        level.operation = "MULTIPLY_ADD"
        level.inputs[1].default_value = 0.5
        level.inputs[2].default_value = 0.5
        tree.links.new(sine.outputs["Value"], level.inputs[0])
        tree.links.new(level.outputs["Value"], mixer.inputs["Fac"])
    else:
        ramp = tree.nodes.new("ShaderNodeValToRGB")
        ramp.color_ramp.elements[0].position = 0.42
        ramp.color_ramp.elements[1].position = 0.58
        tree.links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
        tree.links.new(ramp.outputs["Color"], mixer.inputs["Fac"])

    tree.links.new(mixer.outputs["Color"], principled.inputs["Base Color"])
    tree.links.new(principled.outputs["BSDF"], output.inputs["Surface"])
    return material


def build_ring_material() -> bpy.types.Material:
    """Кольцо: концентрические полосы разной плотности."""
    material = bpy.data.materials.new("pw_planet_ring")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()

    output = tree.nodes.new("ShaderNodeOutputMaterial")
    principled = tree.nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Roughness"].default_value = 0.8

    coord = tree.nodes.new("ShaderNodeTexCoord")
    wave = tree.nodes.new("ShaderNodeTexWave")
    wave.wave_type = "RINGS"
    wave.inputs["Scale"].default_value = 6.0
    wave.inputs["Distortion"].default_value = 3.0
    wave.inputs["Detail"].default_value = 4.0
    tree.links.new(coord.outputs["Object"], wave.inputs["Vector"])

    ramp = tree.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.35, 0.30, 0.26, 1.0)
    ramp.color_ramp.elements[1].color = (0.86, 0.79, 0.68, 1.0)
    tree.links.new(wave.outputs["Fac"], ramp.inputs["Fac"])
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
