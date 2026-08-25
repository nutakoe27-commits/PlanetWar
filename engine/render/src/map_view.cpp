#include "pw/render/map_view.h"

#include <algorithm>
#include <cmath>

#include "pw/sim/control.h"

namespace pw::render {

namespace {

/// Восемь цветов империй.
///
/// Различаются не только тоном, но и светлотой: при дальтонизме тон может
/// совпасть, светлота — нет. Владение системой читается с карты мгновенно,
/// и ошибка здесь стоит игроку хода.
constexpr EmpireColor kEmpireColors[] = {
    {0.91f, 0.64f, 0.24f},   // янтарь
    {0.37f, 0.61f, 0.85f},   // сталь
    {0.53f, 0.78f, 0.42f},   // хвоя
    {0.85f, 0.40f, 0.44f},   // кирпич
    {0.72f, 0.52f, 0.86f},   // ирис
    {0.35f, 0.78f, 0.74f},   // бирюза
    {0.93f, 0.85f, 0.45f},   // латунь
    {0.62f, 0.62f, 0.68f},   // олово
};
constexpr EmpireColor kNeutral{0.42f, 0.45f, 0.52f};

/// Размер звезды на карте. В мировых единицах, поэтому при зуме она
/// растёт вместе с картой — как и должна: это объект, а не значок.
constexpr float kStarRadius = 7.0f;
constexpr float kShipScale = 0.55f;

float toFloat(fx value) { return float(value.toDouble()); }

void pushLine(std::vector<rhi::LineVertex>& out, float x0, float y0, float x1, float y1,
              const EmpireColor& color, float alpha) {
    out.push_back(rhi::LineVertex{x0, y0, color.r, color.g, color.b, alpha});
    out.push_back(rhi::LineVertex{x1, y1, color.r, color.g, color.b, alpha});
}

/// Кружок отрезками. Для полосы осады и обводки выделения.
void pushCircle(std::vector<rhi::LineVertex>& out, float x, float y, float radius,
                const EmpireColor& color, float alpha, int segments = 24,
                float fraction = 1.0f) {
    const int count = std::max(1, int(float(segments) * std::clamp(fraction, 0.0f, 1.0f)));
    for (int i = 0; i < count; ++i) {
        const float a0 = float(i) / float(segments) * 6.28318530718f;
        const float a1 = float(i + 1) / float(segments) * 6.28318530718f;
        out.push_back(rhi::LineVertex{x + std::cos(a0) * radius, y + std::sin(a0) * radius,
                                      color.r, color.g, color.b, alpha});
        out.push_back(rhi::LineVertex{x + std::cos(a1) * radius, y + std::sin(a1) * radius,
                                      color.r, color.g, color.b, alpha});
    }
}

/// Название корпуса по составу флота: рисуем самый крупный из имеющихся.
///
/// Флот — это счётчики, а не отдельные корабли, и рисовать его одним
/// значком правильнее, чем сотней спрайтов: игрок принимает решение
/// по флоту целиком.
const char* dominantHull(const sim::Fleet& fleet) {
    if (fleet.battleships > 0) return "battleship";
    if (fleet.cruisers > 0) return "cruiser";
    if (fleet.destroyers > 0) return "destroyer";
    return "corvette";
}

}  // namespace

const EmpireColor& empireColor(uint32_t empire) {
    constexpr uint32_t count = sizeof(kEmpireColors) / sizeof(kEmpireColors[0]);
    if (empire >= count) return kNeutral;
    return kEmpireColors[empire];
}

const EmpireColor& neutralColor() { return kNeutral; }

uint32_t MapView::pick(const sim::Galaxy& galaxy, float worldX, float worldY,
                       float worldHeight) {
    // Радиус попадания растёт с отдалением: на общем плане звезда занимает
    // несколько пикселей, и требовать попасть в неё точно значило бы
    // сделать карту неуправляемой на дальнем зуме.
    const float radius = std::max(kStarRadius, worldHeight * 0.012f);
    const float radiusSquared = radius * radius;

    uint32_t best = 0xFFFFFFFFu;
    float bestDistance = radiusSquared;

    for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
        const float dx = toFloat(galaxy.positionX(index)) - worldX;
        const float dy = toFloat(galaxy.positionY(index)) - worldY;
        const float distance = dx * dx + dy * dy;
        if (distance > bestDistance) continue;
        bestDistance = distance;
        best = index;
    }
    return best;
}

void MapView::build(const sim::Galaxy& galaxy, const game::WorldView& world, uint32_t empire,
                    const Selection& selection, const rhi::Camera& camera,
                    MapFrame& out) const {
    out.clear();
    const uint32_t systemCount = galaxy.systemCount();
    if (systemCount == 0) return;

    // --- гиперлинии ---
    //
    // Рисуются первыми, чтобы звёзды легли поверх. Каждое ребро один раз:
    // граф неориентированный, и рисовать его дважды значит удвоить работу
    // и получить двойную яркость.
    for (uint32_t from = 0; from < systemCount; ++from) {
        for (uint32_t k = 0; k < galaxy.neighborCount(from); ++k) {
            const uint32_t to = galaxy.neighbors(from)[k];
            if (to <= from) continue;

            // Линия между двумя своими системами ярче: связная территория
            // должна читаться одним взглядом.
            const uint8_t ownerA = from < world.systems.size() ? world.systems[from].owner : 0xFF;
            const uint8_t ownerB = to < world.systems.size() ? world.systems[to].owner : 0xFF;
            const bool mine = ownerA == uint8_t(empire) && ownerB == uint8_t(empire);

            pushLine(out.lines, toFloat(galaxy.positionX(from)), toFloat(galaxy.positionY(from)),
                     toFloat(galaxy.positionX(to)), toFloat(galaxy.positionY(to)),
                     mine ? empireColor(empire) : kNeutral, mine ? 0.55f : 0.18f);
        }
    }

    // --- звёзды ---
    for (uint32_t index = 0; index < systemCount; ++index) {
        const uint8_t owner = index < world.systems.size() ? world.systems[index].owner : 0xFF;
        const EmpireColor& color =
            owner == 0xFF ? kNeutral : empireColor(owner);

        rhi::SpriteInstance star;
        star.x = toFloat(galaxy.positionX(index));
        star.y = toFloat(galaxy.positionY(index));
        // Размер по числу планет: система с восемью планетами и правда
        // важнее пустой, и это должно быть видно, не открывая её.
        const float scale = 1.0f + float(galaxy.planetCount(index)) * 0.08f;
        star.halfWidth = kStarRadius * scale;
        star.halfHeight = kStarRadius * scale;
        star.r = color.r;
        star.g = color.g;
        star.b = color.b;
        star.a = owner == 0xFF ? 0.75f : 1.0f;

        // Звёзды рисуются кадром корвета: своих спрайтов у них пока нет,
        // и подставить круг из шейдера было бы нарушением правила
        // «вся графика из Blender».
        if (atlas_ != nullptr) {
            if (const AtlasFrame* frame = atlas_->frame("corvette", 0.0f)) {
                star.u0 = frame->u0;
                star.v0 = frame->v0;
                star.u1 = frame->u1;
                star.v1 = frame->v1;
            }
        }
        out.sprites.push_back(star);

        // Осада: дуга вокруг звезды, тем длиннее, чем ближе к захвату.
        const uint8_t siegeEmpire =
            index < world.systems.size() ? world.systems[index].siegeEmpire : 0xFF;
        if (siegeEmpire != 0xFF) {
            const float progress =
                float(world.systems[index].siegeProgress) / 100.0f;
            pushCircle(out.lines, star.x, star.y, star.halfWidth * 1.8f,
                       empireColor(siegeEmpire), 0.9f, 32, progress);
        }
    }

    // --- флоты ---
    for (const auto& [id, fleet] : world.fleets) {
        if (fleet.system >= systemCount) continue;

        const float fromX = toFloat(galaxy.positionX(fleet.system));
        const float fromY = toFloat(galaxy.positionY(fleet.system));
        float x = fromX;
        float y = fromY;
        float rotation = 0.0f;

        if (fleet.nextSystem != fleet.system && fleet.nextSystem < systemCount) {
            // Флот в пути стоит между узлами ровно там, где говорит
            // сервер. Никакого предсказания: клиент не применяет правил.
            const float t = std::clamp(toFloat(fleet.progress), 0.0f, 1.0f);
            const float tx = toFloat(galaxy.positionX(fleet.nextSystem));
            const float ty = toFloat(galaxy.positionY(fleet.nextSystem));
            x += (tx - x) * t;
            y += (ty - y) * t;
            // Нос по направлению движения, в оборотах.
            rotation = std::atan2(ty - fromY, tx - fromX) / 6.28318530718f;
        }

        const EmpireColor& color =
            fleet.empire == 0xFF ? kNeutral : empireColor(fleet.empire);

        rhi::SpriteInstance sprite;
        sprite.x = x;
        sprite.y = y;
        sprite.rotationTurns = rotation;
        sprite.r = color.r;
        sprite.g = color.g;
        sprite.b = color.b;
        sprite.a = fleet.empire == uint8_t(empire) ? 1.0f : 0.85f;

        const char* hull = dominantHull(fleet.composition);
        float size = kStarRadius * 1.4f;
        if (atlas_ != nullptr) {
            if (const AtlasFrame* frame = atlas_->frame(hull, rotation)) {
                sprite.u0 = frame->u0;
                sprite.v0 = frame->v0;
                sprite.u1 = frame->u1;
                sprite.v1 = frame->v1;
                size = float(frame->width) * kShipScale;
            }
        }
        // Крупный флот виден крупнее: игрок обязан отличать разведчика
        // от ударной группы, не наводя на неё курсор.
        const float tonnage = float(sim::fleetTonnage(fleet.composition));
        const float bulk = 1.0f + std::min(1.2f, std::sqrt(tonnage) * 0.08f);
        sprite.halfWidth = size * 0.5f * bulk;
        sprite.halfHeight = size * 0.5f * bulk;
        out.sprites.push_back(sprite);
    }

    // --- выделение ---
    if (selection.system < systemCount) {
        const float x = toFloat(galaxy.positionX(selection.system));
        const float y = toFloat(galaxy.positionY(selection.system));
        pushCircle(out.lines, x, y, kStarRadius * 2.4f, empireColor(empire), 0.95f, 28);

        // Линия к цели приказа: игрок видит, куда пойдёт флот, ещё до
        // нажатия. Это единственное, что клиент «предсказывает», и то
        // не игровое правило, а собственное намерение игрока.
        if (selection.hoverSystem < systemCount) {
            const float tx = toFloat(galaxy.positionX(selection.hoverSystem));
            const float ty = toFloat(galaxy.positionY(selection.hoverSystem));
            pushLine(out.lines, x, y, tx, ty, empireColor(empire), 0.7f);
            pushCircle(out.lines, tx, ty, kStarRadius * 1.8f, empireColor(empire), 0.7f, 20);
        }
    }

    (void)camera;
}

}  // namespace pw::render
