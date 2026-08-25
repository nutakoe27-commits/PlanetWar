"""Шрифтовой атлас для интерфейса.

Игроку надо видеть свои ресурсы, состав флота и что происходит в системе —
на экране, а не в терминале. Значит нужен текст, а текст на GPU — это
атлас глифов.

Пекётся тем же Blender'ом, что и остальная графика, и по той же причине:
пайплайн должен быть один. Шрифт лежит В РЕПОЗИТОРИИ (assets/src/fonts),
а не берётся из системы — иначе сборка на разных машинах давала бы разные
атласы, и надписи расползались бы у части игроков.

Шрифт моноширинный намеренно. Во-первых, сетка глифов становится точной
и координаты в атласе считаются без единой ошибки округления. Во-вторых,
цифры в интерфейсе стратегии постоянно меняются, и в пропорциональном
шрифте счётчик ресурсов дёргался бы каждый тик.
"""

from __future__ import annotations

import math
import os

import bpy


# Набор символов. Латиница, кириллица, цифры, знаки — всё, что встречается
# в интерфейсе. Порядок задаёт раскладку в атласе и НЕ должен меняться
# случайно: движок адресует глиф по индексу в этой строке.
CHARSET = (
    " !\"#$%&'()*+,-./0123456789:;<=>?@"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~"
    "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
    "абвгдеёжзийклмнопрстуфхцчшщъыьэюя"
    "·×—…°"
)

COLUMNS = 16


def build_font_atlas(scene, out_path: str, cell: int = 48, samples: int = 16) -> dict:
    """Отрисовать все глифы в один PNG и вернуть описание раскладки.

    Один рендер на весь набор, а не по глифу на рендер: сто восемьдесят
    отдельных запусков Cycles заняли бы минуты вместо секунд, а результат
    был бы тот же.
    """
    font_path = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "assets", "src", "fonts", "DejaVuSansMono.ttf")
    font = bpy.data.fonts.load(font_path)

    rows = int(math.ceil(len(CHARSET) / COLUMNS))
    width = COLUMNS * cell
    height = rows * cell

    # Материал: плоское белое свечение. Цвет надписи задаётся тоном
    # спрайта в шейдере — один атлас на все цвета интерфейса.
    material = bpy.data.materials.new("pw_font")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    emission = tree.nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])

    # Камера смотрит на сетку сверху вниз, ортографически. Одна клетка —
    # одна единица мира, поэтому пиксели и клетки связаны точно.
    camera_data = bpy.data.cameras.new("pw_font_camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = float(COLUMNS)
    camera = bpy.data.objects.new("pw_font_camera", camera_data)
    bpy.context.collection.objects.link(camera)
    scene.camera = camera
    camera.location = (COLUMNS / 2.0, -rows / 2.0, 10.0)
    camera.rotation_euler = (0.0, 0.0, 0.0)
    # Смещение кадра: камера квадратная по ширине, а сетка выше — двигаем
    # плёнку, а не камеру, чтобы связь «клетка = единица» не поехала.
    camera_data.shift_y = 0.0

    objects = []
    for index, character in enumerate(CHARSET):
        column = index % COLUMNS
        row = index // COLUMNS

        bpy.ops.object.text_add()
        text = bpy.context.active_object
        text.data.body = character
        text.data.font = font
        text.data.size = 0.78
        text.data.align_x = "CENTER"
        text.data.align_y = "CENTER"
        text.data.materials.append(material)
        # Центр клетки. Ось Y вниз, как в атласе.
        text.location = (column + 0.5, -(row + 0.5), 0.0)
        objects.append(text)

    bpy.context.view_layer.update()

    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = True
    scene.cycles.samples = samples
    scene.render.filepath = out_path
    bpy.ops.render.render(write_still=True)

    for obj in objects:
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.objects.remove(camera, do_unlink=True)

    return {
        "charset": CHARSET,
        "columns": COLUMNS,
        "rows": rows,
        "cell": cell,
        "width": width,
        "height": height,
        "texture": os.path.basename(out_path),
    }
