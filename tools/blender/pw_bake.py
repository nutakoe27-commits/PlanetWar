"""Запекание моделей Blender в спрайтовые кадры.

Камера ортографическая и наклонена — это и есть та самая «2.5D»: карта лежит
в плоскости, но корабли видно под углом, поэтому у них читается объём.
Наклон означает, что поворот НЕЛЬЗЯ сделать вращением спрайта в шейдере:
силуэт при повороте меняется. Отсюда пре-рендер по кругу.

Каждая модель печётся в двух проходах:
  albedo — обычный кадр с материалами и светом;
  mask   — плоская маска акцентных поверхностей. По ней движок красит корабль
           в цвет империи на лету. Без неё 500 игроков в одном бою неразличимы.

Никакого вектора: на выходе только растровые PNG.
"""

from __future__ import annotations

import math
import os

import bpy
from mathutils import Matrix

# Угол камеры над плоскостью карты. 62 градуса — компромисс: сверху читается
# позиция на карте, сбоку читается силуэт корабля.
CAMERA_ELEVATION_DEG = 62.0

PASS_ALBEDO = "albedo"
PASS_MASK = "mask"


def reset_scene() -> bpy.types.Scene:
    """Пустая сцена. Пайплайн обязан быть воспроизводим от запуска к запуску."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"          # headless и в CI — GPU не нужен
    scene.render.film_transparent = True  # спрайты с альфой
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    # Никакого тонмаппинга: цвета должны попасть в атлас ровно такими,
    # какими заданы в материалах, иначе маска перестанет быть маской.
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    return scene


def setup_camera(scene: bpy.types.Scene, ortho_scale: float):
    data = bpy.data.cameras.new("pw_camera")
    data.type = "ORTHO"
    data.ortho_scale = ortho_scale

    camera = bpy.data.objects.new("pw_camera", data)
    bpy.context.collection.objects.link(camera)
    scene.camera = camera

    elevation = math.radians(CAMERA_ELEVATION_DEG)
    distance = 12.0
    camera.location = (0.0, -distance * math.cos(elevation), distance * math.sin(elevation))
    camera.rotation_euler = (math.radians(90.0) - elevation, 0.0, 0.0)
    return camera


def setup_lights() -> None:
    """Трёхточечная схема на солнцах: дёшево в Cycles, предсказуемо в CI."""
    setups = [
        ("key",  4.2, (math.radians(52.0), 0.0, math.radians(-42.0)), (1.00, 0.97, 0.92)),
        ("fill", 1.5, (math.radians(66.0), 0.0, math.radians(118.0)), (0.72, 0.80, 1.00)),
        ("rim",  3.0, (math.radians(104.0), 0.0, math.radians(178.0)), (0.85, 0.90, 1.00)),
    ]
    for name, energy, rotation, color in setups:
        data = bpy.data.lights.new(f"pw_light_{name}", type="SUN")
        data.energy = energy
        data.color = color
        data.angle = math.radians(3.0)
        light = bpy.data.objects.new(f"pw_light_{name}", data)
        light.rotation_euler = rotation
        bpy.context.collection.objects.link(light)


def make_mask_materials() -> list[bpy.types.Material]:
    """Плоские излучающие материалы для прохода маски.

    Порядок слотов совпадает с pw_hulls: корпус, акцент, свечение.
    Белым горит только акцент — именно его движок перекрашивает.
    """
    colors = [
        ("pw_mask_hull", (0.0, 0.0, 0.0, 1.0)),
        ("pw_mask_accent", (1.0, 1.0, 1.0, 1.0)),
        ("pw_mask_glow", (0.0, 0.0, 0.0, 1.0)),
    ]
    made = []
    for name, color in colors:
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        tree = mat.node_tree
        tree.nodes.clear()
        output = tree.nodes.new("ShaderNodeOutputMaterial")
        emission = tree.nodes.new("ShaderNodeEmission")
        emission.inputs["Color"].default_value = color
        emission.inputs["Strength"].default_value = 1.0
        tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])
        made.append(mat)
    return made


def swap_materials(obj, materials: list[bpy.types.Material]) -> None:
    for index, mat in enumerate(materials):
        if index < len(obj.data.materials):
            obj.data.materials[index] = mat


def render_rotations(scene, obj, *, hull_id: str, pass_name: str, steps: int,
                     size: int, samples: int, out_dir: str) -> list[str]:
    """Отрендерить круг поворотов. Возвращает пути к кадрам по порядку."""
    scene.render.resolution_x = size
    scene.render.resolution_y = size
    scene.render.resolution_percentage = 100
    scene.cycles.samples = samples
    # Шумодав съедает тонкие детали на маленьких спрайтах и в проходе маски
    # размывает границу — выключаем, берём качество семплами.
    scene.cycles.use_denoising = False

    os.makedirs(out_dir, exist_ok=True)
    written = []

    for step in range(steps):
        turns = step / steps
        obj.rotation_euler = (0.0, 0.0, 2.0 * math.pi * turns)

        path = os.path.join(out_dir, f"{hull_id}_{pass_name}_{step:03d}.png")
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        written.append(path)

    obj.rotation_euler = (0.0, 0.0, 0.0)
    return written


def fit_ortho_scale(obj, padding: float = 1.18) -> float:
    """Подобрать масштаб камеры так, чтобы модель влезла при ЛЮБОМ повороте.

    Берём радиус описанной окружности в плоскости карты, а не габарит по осям:
    иначе на 45 градусах корабль вылезет за край спрайта.
    """
    dims = obj.dimensions
    planar_radius = math.hypot(dims.x, dims.y) * 0.5
    vertical = dims.z * 0.5
    return max(planar_radius, vertical) * 2.0 * padding
