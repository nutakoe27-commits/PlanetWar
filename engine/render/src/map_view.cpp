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

/// Размер звезды на карте, в мировых единицах.
constexpr float kStarRadius = 7.0f;

/// Полный оборот. Своё имя, чтобы формулы читались как углы, а не
/// как арифметика с магическим числом.
constexpr float kTau = 6.28318530718f;

/// Половина размера флота на карте, по классу корпуса.
///
/// Задаётся В МИРОВЫХ ЕДИНИЦАХ, а не в пикселях атласа. Первая версия
/// брала ширину испечённого кадра и умножала на коэффициент — и флот
/// выходил втрое крупнее звезды и накрывал её собой. Размер спрайта
/// в атласе говорит о том, с каким разрешением его пекли, а не о том,
/// каким он должен быть на карте: это разные вещи, и связывать их
/// нельзя.
///
/// Флот заметно мельче звезды намеренно: система — это то, за что
/// воюют, а флот — то, чем воюют. Спутать их на карте нельзя.
constexpr float kShipHalfSize[] = {
    0.0f,    // Hull::None
    4.4f,    // корвет
    4.8f,    // тендер
    5.2f,    // колонизатор
    5.4f,    // эсминец
    7.0f,    // носитель
    6.8f,    // крейсер
    6.4f,    // монитор
    8.4f,    // линкор
    11.0f,   // титан
};
static_assert(sizeof(kShipHalfSize) / sizeof(kShipHalfSize[0]) ==
                  size_t(sim::Hull::Count),
              "размеры значков обязаны покрывать все корпуса");

/// На сколько сместить стоящий флот от звезды.
///
/// Флот в центре системы накрывает её собой, и игрок перестаёт видеть
/// класс светила и кольцо владения — то есть ровно то, по чему принимает
/// решение. В долях размера звезды.
constexpr float kFleetOffset = 1.9f;

/// Наименьшая доля высоты экрана, которую занимает звезда.
///
/// Первая версия задавала размер только в мировых единицах, и на общем
/// плане звёзды выродились в точки по четыре пикселя: карту было видно,
/// а играть по ней нельзя — ни разглядеть владение, ни попасть мышью.
/// Поэтому у звезды есть пол в ДОЛЯХ ЭКРАНА, и на дальнем зуме он
/// побеждает мировой размер.
constexpr float kMinStarScreenShare = 0.011f;
constexpr float kMinShipScreenShare = 0.009f;

/// Имя кадра звезды по её классу.
const char* starSprite(uint8_t starClass) {
    switch (sim::StarClass(starClass)) {
        case sim::StarClass::Red:       return "star_red";
        case sim::StarClass::Yellow:    return "star_yellow";
        case sim::StarClass::Blue:      return "star_blue";
        case sim::StarClass::Neutron:   return "star_neutron";
        case sim::StarClass::BlackHole: return "star_blackhole";
        default:                        return "star_yellow";
    }
}

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

/// Самый крупный корпус во флоте: им флот и рисуется.
///
/// Флот — это счётчики, а не отдельные корабли, и рисовать его одним
/// значком правильнее, чем сотней спрайтов: игрок принимает решение
/// по флоту целиком.
/// Каким кораблём рисовать отряд на карте.
///
/// САМЫМ КРУПНЫМ ПРИСУТСТВУЮЩИМ, а не самым многочисленным. Значок отвечает
/// на вопрос «с чем я столкнусь», и один титан в отряде корветов важнее
/// сотни корветов: именно он решит бой. Перебор идёт сверху вниз по
/// перечислению, то есть от самого дорогого корпуса к самому дешёвому.
sim::Hull dominantHull(const sim::Fleet& fleet) {
    for (uint8_t hull = uint8_t(sim::Hull::Count); hull-- > 1;) {
        if (fleet[sim::Hull(hull)] > 0) return sim::Hull(hull);
    }
    return sim::Hull::Corvette;
}

const char* hullSprite(sim::Hull hull) {
    switch (hull) {
        case sim::Hull::Corvette:   return "corvette";
        case sim::Hull::Tender:     return "tender";
        case sim::Hull::Destroyer:  return "destroyer";
        case sim::Hull::Carrier:    return "carrier";
        case sim::Hull::Cruiser:    return "cruiser";
        case sim::Hull::Monitor:    return "monitor";
        case sim::Hull::Battleship: return "battleship";
        case sim::Hull::Titan:      return "titan";
        default:                    return "corvette";
    }
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

    // Пол размера в долях экрана. На общем плане он побеждает мировой
    // размер, иначе звёзды вырождаются в точки и по карте нельзя играть.
    const float minStarHalf = camera.worldHeight * kMinStarScreenShare * 0.5f;
    const float minShipHalf = camera.worldHeight * kMinShipScreenShare * 0.5f;

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
        star.halfWidth = std::max(kStarRadius * scale, minStarHalf);
        star.halfHeight = star.halfWidth;

        // Цвет звезды — от её класса; владение показывает кольцо вокруг.
        //
        // Первая версия красила саму звезду в цвет империи, и класс
        // светила переставал читаться: голубой гигант у янтарной империи
        // выглядел как красный карлик у неё же. А класс — это ценность
        // системы, ради которой за неё и воюют.
        star.r = 1.0f;
        star.g = 1.0f;
        star.b = 1.0f;
        star.a = 1.0f;

        if (atlas_ != nullptr) {
            const uint8_t starClass = galaxy.starClass(index);
            if (const AtlasFrame* frame = atlas_->frame(starSprite(starClass), 0.0f)) {
                star.u0 = frame->u0;
                star.v0 = frame->v0;
                star.u1 = frame->u1;
                star.v1 = frame->v1;
            }
        }
        out.sprites.push_back(star);

        // Кольцо владения. Отдельно от звезды намеренно: цвет империи
        // и класс светила — два разных факта, и смешивать их в одном
        // пикселе значит потерять оба.
        if (owner != 0xFF) {
            pushCircle(out.lines, star.x, star.y, star.halfWidth * 1.35f, color, 0.95f, 20);
        }

        // СВОБОДНАЯ ЗЕМЛЯ: точки по кругу, по одной на ничью планету.
        //
        // Империя начинается с одной планеты и растёт только колонизацией,
        // поэтому «где есть свободное» — самый частый вопрос к карте.
        // Без ответа на него игрок щёлкает по звёздам по очереди.
        //
        // Точками, а не кольцом: кольцо сказало бы «здесь есть свободное»,
        // а точки говорят СКОЛЬКО, и решение «лететь пять прыжков ради
        // одной планеты или три ради трёх» принимается прямо с карты.
        // И точками, а не цифрой: цифра требует прочитать, точки —
        // сосчитать боковым зрением.
        const uint8_t freePlanets =
            index < world.systems.size() ? world.systems[index].freePlanets : 0;
        if (freePlanets > 0) {
            const float ring = star.halfWidth * 1.75f;
            const float dot = std::max(0.9f, star.halfWidth * 0.20f);
            for (uint8_t slot = 0; slot < freePlanets && slot < 12; ++slot) {
                const float angle =
                    kTau * (float(slot) / float(freePlanets < 3 ? 3 : freePlanets));
                pushCircle(out.lines, star.x + ring * std::cos(angle),
                           star.y + ring * std::sin(angle), dot,
                           EmpireColor{0.62f, 0.88f, 0.72f}, 0.85f, 6);
            }
        }

        // Осада: дуга вокруг звезды, тем длиннее, чем ближе к захвату.
        const uint8_t siegeEmpire =
            index < world.systems.size() ? world.systems[index].siegeEmpire : 0xFF;
        if (siegeEmpire != 0xFF) {
            const float progress =
                float(world.systems[index].siegeProgress) / 100.0f;
            // ДУГА ДЫШИТ, пока идёт осада.
            //
            // Осада длится ЧАСЫ, и её дуга почти не двигается: за минуту
            // наблюдения она прирастает на волосок. Неподвижная дуга среди
            // двух сотен звёзд не отличается от узора на карте, и игрок
            // перестаёт её замечать ровно тогда, когда она важнее всего.
            // Пульс с периодом в полторы секунды делает её единственным
            // движущимся местом карты — а движение глаз ловит сам.
            const float beat =
                clock_ > 0.0f
                    ? 1.0f + 0.12f * std::sin(clock_ * kTau / 1.5f)
                    : 1.0f;
            pushCircle(out.lines, star.x, star.y, star.halfWidth * 2.0f * beat,
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

            // СЛЕД ДВИГАТЕЛЕЙ. Перелёт по линии занимает часы, и на карте
            // корабль ползёт по пикселю в минуту: отличить идущий флот
            // от стоящего нельзя, пока не заметишь, что он сдвинулся.
            // След отвечает на два вопроса сразу — идёт ли и КУДА, —
            // и отвечает мгновенно, потому что у следа есть направление.
            const float length = std::sqrt((tx - fromX) * (tx - fromX) +
                                           (ty - fromY) * (ty - fromY));
            if (length > 0.0f) {
                const float dx = (tx - fromX) / length;
                const float dy = (ty - fromY) / length;
                const float tail = std::max(kStarRadius * 2.5f, minShipHalf * 3.0f);
                const EmpireColor& wake =
                    fleet.empire == 0xFF ? kNeutral : empireColor(fleet.empire);
                // Три звена, каждое тусклее предыдущего: сплошная линия
                // читалась бы как гиперлиния, которых на карте и так полно.
                for (int part = 0; part < 3; ++part) {
                    const float a = tail * float(part) / 3.0f;
                    const float b = tail * float(part + 1) / 3.0f;
                    // Дрожание следа — только если часы идут: неподвижный
                    // кадр обязан быть повторяемым.
                    const float flicker =
                        clock_ > 0.0f
                            ? 0.85f + 0.15f * std::sin((clock_ * 6.0f) + float(part))
                            : 1.0f;
                    pushLine(out.lines, x - dx * a, y - dy * a, x - dx * b, y - dy * b,
                             wake, (0.55f - 0.15f * float(part)) * flicker);
                }
            }
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

        const sim::Hull hull = dominantHull(fleet.composition);
        if (atlas_ != nullptr) {
            if (const AtlasFrame* frame = atlas_->frame(hullSprite(hull), rotation)) {
                sprite.u0 = frame->u0;
                sprite.v0 = frame->v0;
                sprite.u1 = frame->u1;
                sprite.v1 = frame->v1;
            }
        }

        // Крупный флот виден крупнее: игрок обязан отличать разведчика
        // от ударной группы, не наводя на неё курсор. Рост по корню, а не
        // линейный: иначе флот из тысячи корветов занял бы пол-экрана.
        const float tonnage = float(sim::fleetTonnage(fleet.composition));
        const float bulk = 1.0f + std::min(0.9f, std::sqrt(tonnage) * 0.06f);
        const float base = kShipHalfSize[int(hull)];
        sprite.halfWidth = std::max(base * bulk, minShipHalf);
        sprite.halfHeight = sprite.halfWidth;

        // Стоящий флот смещаем от звезды: в центре системы он накрывал бы
        // её собой, и игрок переставал видеть класс светила и владение —
        // ровно то, по чему принимает решение.
        if (fleet.nextSystem == fleet.system) {
            sprite.x += kStarRadius * kFleetOffset;
        }
        out.sprites.push_back(sprite);
    }

    // --- выделение ---
    if (selection.system < systemCount) {
        const float x = toFloat(galaxy.positionX(selection.system));
        const float y = toFloat(galaxy.positionY(selection.system));
        // Кольцо выделения ДЫШИТ. Оно и так ярче остальных, но на карте
        // из двух сотен колец владения ещё одно кольцо теряется. Медленное
        // дыхание — период две секунды — отличает «выбрано мной сейчас»
        // от «принадлежит кому-то» без второго цвета и без второй формы.
        const float breath =
            clock_ > 0.0f ? 1.0f + 0.07f * std::sin(clock_ * kTau / 2.0f) : 1.0f;
        pushCircle(out.lines, x, y,
                   std::max(kStarRadius * 2.4f, minStarHalf * 2.2f) * breath,
                   empireColor(empire), 0.95f, 28);

        // Линия к цели приказа: игрок видит, куда пойдёт флот, ещё до
        // нажатия. Это единственное, что клиент «предсказывает», и то
        // не игровое правило, а собственное намерение игрока.
        if (selection.hoverSystem < systemCount) {
            const float tx = toFloat(galaxy.positionX(selection.hoverSystem));
            const float ty = toFloat(galaxy.positionY(selection.hoverSystem));
            pushLine(out.lines, x, y, tx, ty, empireColor(empire), 0.7f);
            pushCircle(out.lines, tx, ty, std::max(kStarRadius * 1.8f, minStarHalf * 1.7f),
                       empireColor(empire), 0.7f, 20);
        }
    }

}

}  // namespace pw::render
