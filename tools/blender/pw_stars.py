"""Модели звёзд и значков карты.

Звезда на карте — не декорация, а главный элемент интерфейса: по ней
игрок читает владение, ценность системы и происходящее в ней. Поэтому
она делается так же, как корабли: настоящая трёхмерная модель, снятая
Blender'ом в спрайт. Никакого рисования кругов шейдером — в графике
игры вектора нет.

Пять классов светил из docs/01-GAME-DESIGN.md, у каждого свой облик:
красный карлик тусклый, голубой гигант яркий, чёрная дыра тёмная
с диском. Класс должен читаться с карты, не открывая систему.
"""

from __future__ import annotations

import math

import bpy


# Класс -> (радиус ядра, цвет, яркость свечения, радиус ореола)
#
# Сила свечения НЕ БОЛЬШЕ ЕДИНИЦЫ. Сцена рендерится без тонмаппинга
# (view_transform = Standard), поэтому всё, что ярче единицы, упирается
# в потолок и становится белым. Первая версия ставила красному карлику 2.2,
# и он выходил из печи бледно-розовым, а нейтронная звезда — чисто белой:
# пять классов светил превращались в один.
#
# Яркость класса передаётся не силой свечения, а РАЗМЕРОМ и цветом:
# голубой гигант крупнее красного карлика почти вдвое, и это видно с карты.
STAR_CLASSES = [
    ("red",       0.62, (0.96, 0.28, 0.14), 0.92, 1.55),
    ("yellow",    0.75, (1.00, 0.80, 0.34), 0.98, 1.70),
    ("blue",      0.88, (0.42, 0.66, 1.00), 1.00, 2.00),
    ("neutron",   0.42, (0.80, 0.92, 1.00), 1.00, 2.40),
    ("blackhole", 0.55, (0.10, 0.08, 0.14), 0.00, 2.30),
]


def _emission_material(name: str, color, strength: float) -> bpy.types.Material:
    """Светящийся материал.

    Звезда светится сама, а не отражает свет сцены: иначе её вид зависел
    бы от расстановки ламп, и красный карлик рядом с ярким источником
    выглядел бы как жёлтый.
    """
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (*color, 1.0)
    emission.inputs["Strength"].default_value = strength

    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def _halo_material(name: str, color) -> bpy.types.Material:
    """Ореол: прозрачный к краям, чтобы звезда не была плоским кружком."""
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (*color, 1.0)
    emission.inputs["Strength"].default_value = 0.85

    transparent = nodes.new("ShaderNodeBsdfTransparent")

    # Свечение к краю сходит на нет — по френелю, а не по маске: так ореол
    # выглядит объёмным с любого ракурса.
    fresnel = nodes.new("ShaderNodeFresnel")
    fresnel.inputs["IOR"].default_value = 1.45

    mix = nodes.new("ShaderNodeMixShader")
    links.new(fresnel.outputs["Fac"], mix.inputs["Fac"])
    links.new(transparent.outputs["BSDF"], mix.inputs[1])
    links.new(emission.outputs["Emission"], mix.inputs[2])

    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(mix.outputs["Shader"], output.inputs["Surface"])
    material.blend_method = "BLEND"
    return material


def build_star_object(star_class: str, core_radius: float, color, strength: float,
                      halo_radius: float):
    """Собрать звезду: ядро плюс ореол.

    Чёрная дыра — особый случай: ядро тёмное, а светится диск вокруг.
    Ради неё же ореол делается отдельным объектом, а не материалом ядра.
    """
    bpy.ops.mesh.primitive_uv_sphere_add(radius=core_radius, segments=32, ring_count=16)
    core = bpy.context.active_object
    core.name = f"star_{star_class}_core"
    bpy.ops.object.shade_smooth()

    if star_class == "blackhole":
        core.data.materials.append(_emission_material(f"star_{star_class}_core_mat",
                                                      (0.02, 0.02, 0.03), 0.0))
        # Аккреционный диск: тор, наклонённый к камере.
        bpy.ops.mesh.primitive_torus_add(major_radius=core_radius * 2.0,
                                         minor_radius=core_radius * 0.32,
                                         major_segments=48, minor_segments=12)
        disk = bpy.context.active_object
        disk.name = f"star_{star_class}_disk"
        disk.rotation_euler = (math.radians(12.0), 0.0, 0.0)
        disk.data.materials.append(
            _emission_material(f"star_{star_class}_disk_mat", (1.00, 0.55, 0.16), 1.0))
        bpy.ops.object.shade_smooth()
    else:
        core.data.materials.append(
            _emission_material(f"star_{star_class}_core_mat", color, strength))

        bpy.ops.mesh.primitive_uv_sphere_add(radius=core_radius * halo_radius,
                                             segments=32, ring_count=16)
        halo = bpy.context.active_object
        halo.name = f"star_{star_class}_halo"
        halo.data.materials.append(_halo_material(f"star_{star_class}_halo_mat", color))
        bpy.ops.object.shade_smooth()

    # Сводим в один объект: пайплайн рендерит по одному объекту за раз.
    bpy.ops.object.select_all(action="DESELECT")
    for obj in bpy.context.scene.objects:
        if obj.name.startswith(f"star_{star_class}_"):
            obj.select_set(True)
    bpy.context.view_layer.objects.active = core
    bpy.ops.object.join()

    joined = bpy.context.active_object
    joined.name = f"star_{star_class}"
    return joined


def star_specs():
    """Список звёзд для сборки: (идентификатор, параметры)."""
    return list(STAR_CLASSES)
