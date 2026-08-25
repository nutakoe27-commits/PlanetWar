"""Параметрическая генерация корпусов кораблей в Blender.

Модели НЕ рисуются руками — они строятся из тех же чисел, что задают баланс.
Корвет с двумя оружейными слотами получает две турели, линкор с десятью —
десять. Художник и балансировщик физически не могут разойтись.

Ручные .blend тоже поддерживаются: если в assets/src/<id>.blend лежит объект,
пайплайн возьмёт его вместо процедурного. Генератор — это база, а не запрет.

Материалы разложены по трём слотам, и это не украшательство, а требование
геймплея: в MMO 500 игроков должны различать чьи корабли перед ними, поэтому
акцентные поверхности выделены в отдельный материал и пекутся отдельной
маской, которую движок красит в цвет империи на лету.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import bmesh
import bpy
from mathutils import Matrix, Vector

# Слоты материалов. Порядок фиксирован — на него опирается запекание масок.
MAT_HULL = 0
MAT_ACCENT = 1
MAT_GLOW = 2

# Продольный профиль корпуса: (доля длины, множитель ширины, множитель высоты).
# t = 0 — нос, t = 1 — корма.
#
# ВАЖНО ПРО ОСИ: нос смотрит в +X. Это не произвол, а стыковка с симуляцией:
# atan2Turns(dy, dx) даёт курс 0 для направления +X, поэтому кадр поворота 0
# обязан изображать корабль, идущий именно туда. Перепутать оси здесь — значит
# получить флот, летящий кормой вперёд, и заметить это только на рендере.
HULL_PROFILE = [
    (0.00, 0.06, 0.06),
    (0.10, 0.40, 0.34),
    (0.26, 0.78, 0.72),
    (0.48, 1.00, 1.00),
    (0.68, 0.96, 0.94),
    (0.86, 0.78, 0.80),
    (1.00, 0.60, 0.66),
]

_SECTION_SEGMENTS = 10


@dataclass
class HullSpec:
    """Один корпус. Поля совпадают со схемой ship_hull из таблиц баланса."""

    id: str
    display_name: str
    length: float
    beam: float
    height: float
    engines: int
    weapon_slots: int
    defense_slots: int
    utility_slots: int
    fins: int
    sprite_size: int

    @classmethod
    def from_dict(cls, raw: dict) -> "HullSpec":
        return cls(
            id=raw["id"],
            display_name=raw.get("display_name", raw["id"]),
            length=float(raw["length"]),
            beam=float(raw["beam"]),
            height=float(raw["height"]),
            engines=int(raw["engines"]),
            weapon_slots=int(raw["weapon_slots"]),
            defense_slots=int(raw["defense_slots"]),
            utility_slots=int(raw["utility_slots"]),
            fins=int(raw.get("fins", 2)),
            sprite_size=int(raw.get("sprite_size", 128)),
        )


# ---------------------------------------------------------------------------
# Материалы
# ---------------------------------------------------------------------------

def _principled(mat: bpy.types.Material) -> bpy.types.Node:
    return next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")


def _set_input(node: bpy.types.Node, names: tuple[str, ...], value) -> None:
    """Имена входов Principled BSDF менялись между версиями Blender.

    Пробуем варианты по очереди, а не падаем на первой же несостыковке —
    пайплайн должен переживать обновление Blender без правки кода.
    """
    for name in names:
        socket = node.inputs.get(name)
        if socket is not None:
            socket.default_value = value
            return


def build_materials() -> list[bpy.types.Material]:
    """Три материала: корпус, акцент под цвет империи, свечение двигателей."""
    made = []

    hull = bpy.data.materials.new("pw_hull")
    hull.use_nodes = True
    node = _principled(hull)
    _set_input(node, ("Base Color",), (0.46, 0.49, 0.55, 1.0))
    _set_input(node, ("Metallic",), 0.78)
    _set_input(node, ("Roughness",), 0.32)
    made.append(hull)

    # Акцент. В обычном проходе он нейтрально-светлый, в проходе маски
    # становится белым — движок по этой маске красит корабль в цвет империи.
    accent = bpy.data.materials.new("pw_accent")
    accent.use_nodes = True
    node = _principled(accent)
    _set_input(node, ("Base Color",), (0.78, 0.72, 0.62, 1.0))
    _set_input(node, ("Metallic",), 0.25)
    _set_input(node, ("Roughness",), 0.55)
    made.append(accent)

    glow = bpy.data.materials.new("pw_glow")
    glow.use_nodes = True
    node = _principled(glow)
    _set_input(node, ("Base Color",), (0.20, 0.55, 0.95, 1.0))
    _set_input(node, ("Emission Color", "Emission"), (0.30, 0.68, 1.0, 1.0))
    _set_input(node, ("Emission Strength",), 6.0)
    _set_input(node, ("Roughness",), 0.9)
    made.append(glow)

    return made


# ---------------------------------------------------------------------------
# Геометрия
# ---------------------------------------------------------------------------

def _section(half_w: float, half_h: float) -> list[Vector]:
    """Поперечное сечение корпуса: сплюснутый многоугольник.

    Низ слегка уплощён, верх скруглён — силуэт читается как корабль,
    а не как капсула, даже на спрайте в 64 пикселя.
    """
    verts = []
    for i in range(_SECTION_SEGMENTS):
        angle = 2.0 * math.pi * i / _SECTION_SEGMENTS
        y = math.cos(angle) * half_w
        z = math.sin(angle) * half_h
        if z < 0.0:
            z *= 0.62  # подрезанное днище
        verts.append(Vector((0.0, y, z)))
    return verts


def _add_cone(bm, segments, radius1, radius2, depth, matrix):
    """create_cone менял имена параметров между версиями Blender."""
    try:
        return bmesh.ops.create_cone(
            bm, cap_ends=True, cap_tris=False, segments=segments,
            radius1=radius1, radius2=radius2, depth=depth, matrix=matrix)
    except TypeError:
        return bmesh.ops.create_cone(
            bm, cap_ends=True, cap_tris=False, segments=segments,
            diameter1=radius1 * 2.0, diameter2=radius2 * 2.0,
            depth=depth, matrix=matrix)


def _tag(faces, index: int) -> None:
    for face in faces:
        face.material_index = index


def _build_body(bm, spec: HullSpec) -> None:
    """Корпус лофтом по продольному профилю."""
    half_beam = spec.beam * 0.5
    half_height = spec.height * 0.5
    rings = []

    for t, wide, tall in HULL_PROFILE:
        x = (0.5 - t) * spec.length  # t=0 -> нос в +X
        ring = [bm.verts.new((x, v.y * wide * half_beam, v.z * tall * half_height))
                for v in _section(1.0, 1.0)]
        rings.append(ring)

    made = []
    for a, b in zip(rings, rings[1:]):
        for i in range(_SECTION_SEGMENTS):
            j = (i + 1) % _SECTION_SEGMENTS
            made.append(bm.faces.new((a[i], a[j], b[j], b[i])))

    made.append(bm.faces.new(list(reversed(rings[0]))))   # нос
    made.append(bm.faces.new(rings[-1]))                  # корма
    _tag(made, MAT_HULL)


def _box(bm, corners: list[tuple[float, float, float]], material: int) -> None:
    """Шестигранник по восьми явным вершинам: 0-3 низ, 4-7 верх.

    Нужен там, где нужна СУЖАЮЩАЯСЯ форма. Масштабированный куб такого не даёт,
    а именно сужение отличает силуэт корабля от силуэта ящика.
    """
    v = [bm.verts.new(p) for p in corners]
    for indices in ((0, 1, 2, 3), (7, 6, 5, 4), (0, 4, 5, 1),
                    (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0)):
        bm.faces.new([v[i] for i in indices]).material_index = material


def _build_superstructure(bm, spec: HullSpec) -> None:
    """Мостик: сужающаяся кверху рубка позади миделя.

    Узкая и невысокая намеренно. На карте корабль будет размером в 30 пикселей,
    и всё, что шире трети корпуса, там сливается в кляксу.
    """
    back = -spec.length * 0.20
    front = spec.length * 0.06
    base_w = spec.beam * 0.30
    top_w = spec.beam * 0.17
    z0 = spec.height * 0.30
    z1 = spec.height * 0.86

    _box(bm, [
        (front, -base_w, z0), (front, base_w, z0),
        (back, base_w, z0), (back, -base_w, z0),
        (front - spec.length * 0.02, -top_w, z1), (front - spec.length * 0.02, top_w, z1),
        (back + spec.length * 0.03, top_w, z1), (back + spec.length * 0.03, -top_w, z1),
    ], MAT_HULL)


def _build_spine(bm, spec: HullSpec) -> None:
    """Продольная полоса по хребту — здесь живёт цвет империи.

    Полоса, а не плоскость: она читается как опознавательный знак при любом
    повороте и не разрушает силуэт. Это ответ на задачу MMO — в бою на 500
    кораблей игрок обязан за долю секунды понять, чьи перед ним.
    """
    front = spec.length * 0.30
    back = -spec.length * 0.40
    half = spec.beam * 0.11
    z0 = spec.height * 0.44
    z1 = spec.height * 0.52

    _box(bm, [
        (front, -half, z0), (front, half, z0), (back, half, z0), (back, -half, z0),
        (front + spec.length * 0.05, -half * 0.5, z1), (front + spec.length * 0.05, half * 0.5, z1),
        (back, half * 0.7, z1), (back, -half * 0.7, z1),
    ], MAT_ACCENT)


def _build_engines(bm, spec: HullSpec) -> None:
    """Двигатели у кормы. Сопло — светящийся материал."""
    radius = spec.height * 0.30
    depth = spec.length * 0.16
    stern = -spec.length * 0.5 + depth * 0.35  # корма в -X

    if spec.engines == 1:
        offsets = [0.0]
    else:
        span = spec.beam * 0.62
        step = span * 2.0 / (spec.engines - 1)
        offsets = [-span + i * step for i in range(spec.engines)]

    for offset in offsets:
        matrix = (Matrix.Translation((stern, offset, -spec.height * 0.06)) @
                  Matrix.Rotation(math.radians(90.0), 4, "Y"))
        nacelle = _add_cone(bm, 10, radius, radius * 0.82, depth, matrix)
        for vert in nacelle["verts"]:
            for face in vert.link_faces:
                face.material_index = MAT_HULL

        # Сопло: короткий диск в самом хвосте, он и светится.
        nozzle_matrix = (Matrix.Translation((stern - depth * 0.56, offset,
                                             -spec.height * 0.06)) @
                         Matrix.Rotation(math.radians(90.0), 4, "Y"))
        nozzle = _add_cone(bm, 10, radius * 0.80, radius * 0.62,
                           depth * 0.14, nozzle_matrix)
        for vert in nozzle["verts"]:
            for face in vert.link_faces:
                face.material_index = MAT_GLOW


def _build_turrets(bm, spec: HullSpec) -> None:
    """Турели по числу оружейных слотов — ровно столько, сколько в балансе."""
    count = max(spec.weapon_slots, 0)
    if count == 0:
        return

    size = spec.height * 0.34
    # Раскладываем парами по бортам, нечётную ставим по осевой линии.
    pairs, odd = divmod(count, 2)
    positions = []

    for i in range(pairs):
        t = (i + 1) / (pairs + 1)
        x = (0.62 - t * 0.98) * spec.length
        y = spec.beam * 0.40
        positions.append((x, y))
        positions.append((x, -y))
    if odd:
        positions.append((spec.length * 0.30, 0.0))

    for x, y in positions:
        z = spec.height * 0.36
        # Барбет: низкий цилиндр. Круглая башня читается на спрайте лучше
        # кубической — у неё нет ориентации, которая спорит с курсом корабля.
        turret = _add_cone(bm, 8, size * 0.62, size * 0.52, size * 0.72,
                           Matrix.Translation((x, y, z)))
        for vert in turret["verts"]:
            for face in vert.link_faces:
                face.material_index = MAT_HULL

        # Ствол смотрит вперёд, в +X — туда же, куда идёт корабль.
        barrel = _add_cone(bm, 6, size * 0.16, size * 0.13, size * 1.5,
                           Matrix.Translation((x + size * 0.7, y, z + size * 0.12)) @
                           Matrix.Rotation(math.radians(90.0), 4, "Y"))
        for vert in barrel["verts"]:
            for face in vert.link_faces:
                face.material_index = MAT_HULL


def _build_fins(bm, spec: HullSpec) -> None:
    """Стреловидные клинья у кормы.

    Тонкие, короткие и сужающиеся к концу. Первая версия делала их плитами
    в полторы ширины корпуса — корабль превращался в крест из коробок, а сам
    корпус переставал читаться. Размах намеренно меньше ширины корпуса.
    """
    if spec.fins <= 0:
        return

    rows = max(spec.fins // 2, 1)
    for row in range(rows):
        root_front = -spec.length * (0.06 + row * 0.17)
        root_back = root_front - spec.length * 0.26
        span = spec.beam * (0.62 - row * 0.14)
        sweep = spec.length * 0.13          # конец отнесён назад
        chord_tip = spec.length * 0.13      # хорда сужается
        thick = spec.height * 0.05
        y0 = spec.beam * 0.30

        for side in (1.0, -1.0):
            y1 = side * (y0 + span)
            ry = side * y0
            tip_front = root_front - sweep
            _box(bm, [
                (root_front, ry, -thick), (tip_front, y1, -thick),
                (tip_front - chord_tip, y1, -thick), (root_back, ry, -thick),
                (root_front, ry, thick), (tip_front, y1, thick),
                (tip_front - chord_tip, y1, thick), (root_back, ry, thick),
            ], MAT_ACCENT)


def build_hull_object(spec: HullSpec, materials: list[bpy.types.Material]):
    """Собрать один корабль. Возвращает объект Blender, уже слинкованный в сцену."""
    mesh = bpy.data.meshes.new(f"pw_mesh_{spec.id}")
    bm = bmesh.new()

    _build_body(bm, spec)
    _build_superstructure(bm, spec)
    _build_spine(bm, spec)
    _build_engines(bm, spec)
    _build_turrets(bm, spec)
    _build_fins(bm, spec)

    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.normal_update()

    # Фаска: без неё кромки не ловят свет и модель на спрайте выглядит плоской.
    try:
        bmesh.ops.bevel(bm, geom=list(bm.edges) + list(bm.verts),
                        offset=spec.height * 0.018, segments=2, affect="EDGES")
    except (TypeError, RuntimeError):
        pass  # версия Blender без этой сигнатуры — переживём без фаски

    bm.to_mesh(mesh)
    bm.free()

    for face in mesh.polygons:
        face.use_smooth = False

    obj = bpy.data.objects.new(f"pw_ship_{spec.id}", mesh)
    for mat in materials:
        obj.data.materials.append(mat)
    bpy.context.collection.objects.link(obj)
    return obj
