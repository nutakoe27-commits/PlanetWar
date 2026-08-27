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
    # ПРОПОРЦИОНАЛЬНЫЙ, А НЕ МОНОШИРИННЫЙ.
    #
    # До этого атлас пёкся из DejaVu Sans Mono, и весь интерфейс читался
    # как вывод терминала: «Система 0» с одинаковыми промежутками между
    # буквами, «свободно слотов 5 из 5» шириной в пол-панели. Моноширинный
    # шрифт в игровом интерфейсе — самый заметный признак программистской
    # вёрстки, и заметен он раньше, чем что-либо ещё.
    #
    # Пропорциональный даёт две вещи сразу: текст занимает на четверть
    # меньше места (панели становятся плотнее без потери кегля) и перестаёт
    # выглядеть служебным. Метрики атлас считает сам — они снимаются
    # с готовой картинки, а не берутся из файла шрифта, поэтому смена
    # начертания не требует ничего, кроме смены имени файла.
    #
    # Цифры при этом остаются одной ширины (см. measure_glyphs): числа
    # в интерфейсе меняются каждую секунду, и прыгающая ширина цифры
    # заставляет всю строку дёргаться.
    font_path = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "assets", "src", "fonts", "DejaVuSans.ttf")
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
        # Кегль в клетке.
        #
        # Было 0,78 — и при высоте строки в семнадцать пикселей прописная
        # выходила девять, а сама буква пять пикселей в ширину. На снимке
        # с двукратным увеличением это видно сразу: между буквами воздуха
        # больше, чем краски. Клетка квадратная и рисуется высотой в строку,
        # поэтому кегль — это ПРЯМО доля высоты строки, которую занимает
        # буква, и 0,78 для неё мало.
        #
        # 0,92 тоже оказалось мало — но по другой причине, и увидеть её
        # можно было только на пятикратном увеличении. Кегль задаёт ПОЛНУЮ
        # высоту шрифта, от нижнего выноса до верхнего; строчная буква
        # без выносов занимает от неё чуть больше половины. При 0,92
        # «свободно слотов 6 из 6» читалось как разрядка: краски меньше,
        # чем воздуха между буквами, хотя продвижения посчитаны верно.
        #
        # 1,25 подобрано так, чтобы строчная выходила примерно в две трети
        # строки — обычная пропорция наборного шрифта. Что при этом ничего
        # не обрезалось, проверяет сам конвейер: `measure_glyphs` кричит,
        # если краска дошла до края клетки.
        text.data.size = 1.25
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
        "metrics": measure_glyphs(out_path, cell, rows),
    }


def measure_glyphs(png_path: str, cell: int, rows: int) -> list[dict]:
    """Померить, сколько места на самом деле занимает каждая буква.

    ЗАЧЕМ. Атлас — это сетка одинаковых клеток, и первая версия рисовала
    каждую букву целой клеткой с одинаковым шагом. Получался моноширинный
    шрифт: у «ш» и у «i» одна и та же ширина, и половина строки уходила
    в пустоту. Цифры от этого выигрывают — столбик чисел не дёргается, —
    а связный текст проигрывает: подсказка на две строки читается как
    телеграмма.

    Меряем НЕ по метрикам TTF, а по самому отрендеренному атласу: между
    шрифтом и пикселями стоит Blender со своим растеризатором, и верить
    надо тому, что получилось, а не тому, что было задумано.

    Возвращаем на каждый глиф долю клетки: где начинается краска, где
    заканчивается и на сколько сдвигать курсор.
    """
    image = bpy.data.images.load(png_path)
    width, height = image.size
    # Пиксели идут снизу вверх, по четыре числа на точку.
    pixels = list(image.pixels)

    metrics = []
    for index, character in enumerate(CHARSET):
        column = index % COLUMNS
        row = index // COLUMNS

        x0 = column * cell
        # Строка 0 сетки — верхняя, а у изображения снизу. Переворачиваем.
        y0 = (rows - 1 - row) * cell

        left, right = cell, -1
        for y in range(y0, min(y0 + cell, height)):
            base = y * width * 4
            for x in range(x0, min(x0 + cell, width)):
                if pixels[base + x * 4 + 3] > 0.02:
                    local = x - x0
                    if local < left: left = local
                    if local > right: right = local

        # ОБРЕЗКА ПО КРАЮ КЛЕТКИ — молчаливая порча. Буква, упёршаяся
        # в границу, теряет часть краски, и заметить это на строке
        # в четырнадцать пикселей нельзя. Поэтому конвейер проверяет сам.
        if right >= cell - 1 or (left <= 0 and right >= left):
            print(f"  ВНИМАНИЕ: глиф {character!r} упирается в край клетки — "
                  f"уменьшите кегль")

        if right < left:
            # Пробел: краски нет, мерить нечего — ширину задаём.
            #
            # Моноширинный шрифт даёт пробелу ту же ширину, что и букве,
            # и в связном тексте это читается как разрядка: «жёлтая
            # звезда · ваша» разъезжается на полстроки. Набирают пробел
            # примерно в треть кегля — вчетверо шире межбуквенного
            # просвета, и этого хватает, чтобы слова не слипались.
            metrics.append({"char": character, "left": 0.0, "right": 0.0,
                            "advance": 0.28})
            continue

        # Полпикселя с каждой стороны — это край растра, а не буква.
        ink_left = left / cell
        ink_right = (right + 1) / cell
        # Боковые поля: одинаковые слева и справа.
        #
        # Заданы В ДОЛЯХ КЛЕТКИ, а не кегля, поэтому при росте кегля их
        # надо уменьшать — иначе поле остаётся тем же, а буква растёт,
        # и они перестают быть полями. 0,030 при кегле 1,25 даёт примерно
        # тот же просвет, что 0,045 давало при 0,92.
        side = 0.030
        metrics.append({
            "char": character,
            "left": round(ink_left, 4),
            "right": round(ink_right, 4),
            "advance": round(ink_right - ink_left + side * 2.0, 4),
        })

    # ЦИФРЫ ОСТАЮТСЯ МОНОШИРИННЫМИ. Это не непоследовательность, а то же
    # правило, что в наборе таблиц: столбик чисел, где «1» уже «8», рябит
    # при каждом изменении значения, а счётчик ресурсов меняется постоянно.
    digits = [m for m in metrics if m["char"].isdigit()]
    if digits:
        widest = max(m["advance"] for m in digits)
        for m in digits:
            # Сдвигаем краску в середину общей ширины.
            m["left"] -= (widest - m["advance"]) * 0.5
            m["right"] -= (widest - m["advance"]) * 0.5
            m["advance"] = widest

    bpy.data.images.remove(image)
    return metrics
