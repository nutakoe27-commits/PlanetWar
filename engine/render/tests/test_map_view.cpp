#include "doctest.h"

#include <cmath>
#include <set>

#include "pw/render/map_view.h"

using namespace pw;
using namespace pw::render;

namespace {

/// Галактика и мир для проверки кадра.
///
/// Ни окна, ни видеокарты: MapView отдаёт массивы, и всё, что нужно
/// проверить, — их содержимое. Именно ради этого рендер разделён на
/// «что рисовать» и «чем рисовать».
struct Scene {
    sim::World world;
    sim::Galaxy galaxy;
    game::WorldView view;
    MapView map;
    MapFrame frame;
    rhi::Camera camera;

    explicit Scene(uint32_t systems = 40) {
        sim::registerGalaxyComponents(world);
        sim::GalaxyParams params;
        params.seed = 0x4A17;
        params.systemCount = systems;
        galaxy.generate(world, params);
        view.resize(systems);
        camera.worldHeight = float(galaxy.extent().toDouble()) * 2.0f;
    }

    void build(uint32_t empire = 0, const Selection& selection = {}) {
        map.build(galaxy, view, empire, selection, camera, frame);
    }

    size_t spriteCount() const { return frame.sprites.size(); }
    size_t lineCount() const { return frame.lines.size() / 2; }
};

}  // namespace

TEST_CASE("карта: каждая система даёт спрайт") {
    Scene scene(40);
    scene.build();
    CHECK(scene.spriteCount() == 40);
}

TEST_CASE("карта: каждая гиперлиния рисуется ровно один раз") {
    // Граф неориентированный. Нарисовать его дважды — удвоить работу
    // и получить двойную яркость на каждой линии.
    Scene scene(60);
    scene.build();
    CHECK(scene.lineCount() == scene.galaxy.laneCount());
}

TEST_CASE("карта: владение показано кольцом, а не цветом самой звезды") {
    // Цвет империи и класс светила — два разных факта, и смешивать их
    // в одном пикселе значит потерять оба: голубой гигант у янтарной
    // империи выглядел бы как красный карлик у неё же. А класс — это
    // ценность системы, ради которой за неё и воюют.
    Scene scene(30);
    scene.build();
    const size_t withoutOwners = scene.lineCount();

    scene.view.systems[5].owner = 0;
    scene.view.systems[6].owner = 3;
    scene.build(/*empire=*/0);

    // Звёзды остались белыми: цвет придёт из текстуры класса.
    CHECK(scene.frame.sprites[5].r == doctest::Approx(1.0f));
    CHECK(scene.frame.sprites[5].g == doctest::Approx(1.0f));
    CHECK(scene.frame.sprites[5].b == doctest::Approx(1.0f));

    // Зато появились кольца владения.
    CHECK(scene.lineCount() > withoutOwners);
}

TEST_CASE("карта: кольцо владения красится цветом империи") {
    Scene scene(30);
    scene.view.systems[5].owner = 3;
    scene.build(/*empire=*/0);

    const EmpireColor& theirs = empireColor(3);
    const float x = float(scene.galaxy.positionX(5).toDouble());
    const float y = float(scene.galaxy.positionY(5).toDouble());

    // Ищем вершину кольца: рядом с системой и нужного цвета.
    bool found = false;
    for (const auto& vertex : scene.frame.lines) {
        if (std::fabs(vertex.x - x) > 40.0f || std::fabs(vertex.y - y) > 40.0f) continue;
        if (std::fabs(vertex.r - theirs.r) > 0.01f) continue;
        if (std::fabs(vertex.g - theirs.g) > 0.01f) continue;
        found = true;
        break;
    }
    CHECK(found);
}

TEST_CASE("карта: класс светила виден на карте") {
    // Класс — это ценность системы: голубой гигант богат энергией,
    // чёрная дыра ценнейшая. Игрок обязан читать это с карты, не
    // открывая систему.
    Scene scene(200);
    std::set<uint8_t> classes;
    for (uint32_t index = 0; index < scene.galaxy.systemCount(); ++index) {
        classes.insert(scene.galaxy.starClass(index));
    }
    // Галактика на двести систем обязана содержать хотя бы три класса,
    // иначе разведка теряет смысл.
    CHECK(classes.size() >= 3);
}

TEST_CASE("карта: цвета империй различимы по светлоте") {
    // Не «красный против зелёного»: при дальтонизме тон может совпасть,
    // светлота — нет. Владение читается с карты мгновенно, и ошибка
    // здесь стоит игроку хода.
    std::vector<float> luminance;
    for (uint32_t empire = 0; empire < 8; ++empire) {
        const EmpireColor& color = empireColor(empire);
        luminance.push_back(0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b);
    }
    for (size_t a = 0; a < luminance.size(); ++a) {
        for (size_t b = a + 1; b < luminance.size(); ++b) {
            const EmpireColor& first = empireColor(uint32_t(a));
            const EmpireColor& second = empireColor(uint32_t(b));
            const float tone = std::fabs(first.r - second.r) + std::fabs(first.g - second.g) +
                               std::fabs(first.b - second.b);
            const float light = std::fabs(luminance[a] - luminance[b]);
            // Либо заметно разный тон, либо заметно разная светлота.
            CHECK((tone > 0.35f || light > 0.10f));
        }
    }
}

TEST_CASE("карта: флот виден и стоит в своей системе") {
    Scene scene(30);
    game::FleetView fleet;
    fleet.id = 1;
    fleet.empire = 0;
    fleet.system = 7;
    fleet.nextSystem = 7;
    fleet.composition = sim::makeFleet({{sim::Hull::Corvette, 4}});
    scene.view.fleets[1] = fleet;

    scene.build();
    REQUIRE(scene.spriteCount() == 31);   // тридцать звёзд и флот

    // Стоящий флот смещён от звезды: в центре системы он накрывал бы её
    // собой, и игрок переставал видеть класс светила и владение — ровно
    // то, по чему принимает решение.
    const auto& sprite = scene.frame.sprites.back();
    const float starX = float(scene.galaxy.positionX(7).toDouble());
    const float starY = float(scene.galaxy.positionY(7).toDouble());
    CHECK(sprite.x > starX);
    CHECK(sprite.x - starX < 40.0f);
    CHECK(sprite.y == doctest::Approx(starY));
}

TEST_CASE("карта: флот не крупнее системы, за которую воюют") {
    // Система — то, за что воюют, флот — то, чем воюют. Спутать их
    // на карте нельзя. Первая версия брала размер спрайта из ширины
    // испечённого кадра, и флот выходил втрое крупнее звезды.
    Scene scene(30);
    game::FleetView fleet;
    fleet.id = 1;
    fleet.empire = 0;
    fleet.system = 7;
    fleet.nextSystem = 7;
    // СОРОК ЛИНКОРОВ, как и написано. Раньше здесь стояло `Fleet{0,0,0,40}`
    // с этим же комментарием — и означало сорок эсминцев, потому что
    // позиционная запись писалась на четыре класса корпусов, а их стало
    // девять. Проверка на «флот не крупнее звезды» проверяла корабль
    // вдвое мельче того, ради которого затевалась.
    fleet.composition = sim::makeFleet({{sim::Hull::Battleship, 40}});
    scene.view.fleets[1] = fleet;
    scene.build();

    const auto& ship = scene.frame.sprites.back();
    const auto& star = scene.frame.sprites[7];
    CHECK(ship.halfWidth <= star.halfWidth * 2.0f);
}

TEST_CASE("карта: летящий флот стоит между узлами ровно там, где сказал сервер") {
    // Никакого предсказания: клиент не применяет игровых правил, он
    // рисует присланное. Половина пути — значит ровно середина отрезка.
    Scene scene(30);
    const uint32_t from = 3;
    REQUIRE(scene.galaxy.neighborCount(from) > 0);
    const uint32_t to = scene.galaxy.neighbors(from)[0];

    game::FleetView fleet;
    fleet.id = 1;
    fleet.empire = 0;
    fleet.system = from;
    fleet.nextSystem = to;
    fleet.progress = fx::fromFraction(1, 2);
    fleet.composition = sim::makeFleet({{sim::Hull::Corvette, 1}});
    scene.view.fleets[1] = fleet;

    scene.build();
    const auto& sprite = scene.frame.sprites.back();
    const float expectedX = float((scene.galaxy.positionX(from).toDouble() +
                                   scene.galaxy.positionX(to).toDouble()) *
                                  0.5);
    const float expectedY = float((scene.galaxy.positionY(from).toDouble() +
                                   scene.galaxy.positionY(to).toDouble()) *
                                  0.5);
    CHECK(sprite.x == doctest::Approx(expectedX).epsilon(0.01));
    CHECK(sprite.y == doctest::Approx(expectedY).epsilon(0.01));
}

TEST_CASE("карта: нос флота смотрит туда, куда он летит") {
    Scene scene(30);
    const uint32_t from = 3;
    const uint32_t to = scene.galaxy.neighbors(from)[0];

    game::FleetView fleet;
    fleet.id = 1;
    fleet.empire = 0;
    fleet.system = from;
    fleet.nextSystem = to;
    fleet.progress = fx::fromFraction(1, 4);
    fleet.composition = sim::makeFleet({{sim::Hull::Corvette, 1}});
    scene.view.fleets[1] = fleet;
    scene.build();

    const float dx = float(scene.galaxy.positionX(to).toDouble() -
                           scene.galaxy.positionX(from).toDouble());
    const float dy = float(scene.galaxy.positionY(to).toDouble() -
                           scene.galaxy.positionY(from).toDouble());
    const float expected = std::atan2(dy, dx) / 6.28318530718f;
    CHECK(scene.frame.sprites.back().rotationTurns == doctest::Approx(expected).epsilon(0.001));
}

TEST_CASE("карта: крупный флот виден крупнее") {
    // Игрок обязан отличать разведчика от ударной группы, не наводя
    // на неё курсор.
    Scene scene(20);
    game::FleetView small;
    small.id = 1;
    small.empire = 0;
    small.system = 2;
    small.nextSystem = 2;
    small.composition = sim::makeFleet({{sim::Hull::Corvette, 1}});

    game::FleetView big = small;
    big.id = 2;
    big.system = 4;
    big.nextSystem = 4;
    // Значок растёт и от тоннажа, и от старшего корпуса — поэтому в крупном
    // отряде есть и то, и другое.
    big.composition = sim::makeFleet({{sim::Hull::Corvette, 40}, {sim::Hull::Destroyer, 20},
                                 {sim::Hull::Cruiser, 10}, {sim::Hull::Battleship, 5}});

    scene.view.fleets[1] = small;
    scene.view.fleets[2] = big;
    scene.build();

    REQUIRE(scene.spriteCount() == 22);
    const auto& first = scene.frame.sprites[20];
    const auto& second = scene.frame.sprites[21];
    CHECK(second.halfWidth > first.halfWidth);
}

TEST_CASE("карта: осада рисуется дугой по прогрессу") {
    Scene scene(20);
    scene.build();
    const size_t quiet = scene.lineCount();

    scene.view.systems[4].siegeEmpire = 2;
    scene.view.systems[4].siegeProgress = 50;
    scene.build();
    const size_t half = scene.lineCount();

    scene.view.systems[4].siegeProgress = 100;
    scene.build();
    const size_t full = scene.lineCount();

    CHECK(half > quiet);
    CHECK(full > half);
}

TEST_CASE("карта: выделение и цель приказа видны") {
    Scene scene(20);
    scene.build();
    const size_t plain = scene.lineCount();

    Selection selection;
    selection.system = 5;
    scene.build(0, selection);
    const size_t selected = scene.lineCount();

    selection.hoverSystem = 9;
    scene.build(0, selection);
    const size_t aiming = scene.lineCount();

    CHECK(selected > plain);
    CHECK(aiming > selected);
}

TEST_CASE("карта: своя территория выделяется яркими линиями") {
    // Связная территория должна читаться одним взглядом.
    Scene scene(40);
    scene.build(0);
    float dimTotal = 0.0f;
    for (const auto& vertex : scene.frame.lines) dimTotal += vertex.a;

    for (auto& system : scene.view.systems) system.owner = 0;
    scene.build(0);
    float brightTotal = 0.0f;
    for (const auto& vertex : scene.frame.lines) brightTotal += vertex.a;

    CHECK(brightTotal > dimTotal * 2.0f);
}

// ---------------------------------------------------------------------------
// Выбор мышью
// ---------------------------------------------------------------------------

TEST_CASE("выбор: попадание в звезду") {
    Scene scene(40);
    for (uint32_t index = 0; index < scene.galaxy.systemCount(); ++index) {
        const float x = float(scene.galaxy.positionX(index).toDouble());
        const float y = float(scene.galaxy.positionY(index).toDouble());
        CHECK(MapView::pick(scene.galaxy, x, y, 100.0f) == index);
    }
}

TEST_CASE("выбор: мимо всех — ничего") {
    Scene scene(40);
    // Далеко за пределами галактики.
    const float far = float(scene.galaxy.extent().toDouble()) * 10.0f;
    CHECK(MapView::pick(scene.galaxy, far, far, 100.0f) == 0xFFFFFFFFu);
}

TEST_CASE("выбор: радиус попадания растёт с отдалением") {
    // На общем плане звезда занимает несколько пикселей. Требовать попасть
    // в неё точно значило бы сделать карту неуправляемой.
    Scene scene(40);
    const float x = float(scene.galaxy.positionX(0).toDouble());
    const float y = float(scene.galaxy.positionY(0).toDouble());

    // Промах на 30 единиц: вблизи мимо, издалека попадание.
    CHECK(MapView::pick(scene.galaxy, x + 30.0f, y, 100.0f) != 0);
    CHECK(MapView::pick(scene.galaxy, x + 30.0f, y, 5000.0f) == 0);
}

TEST_CASE("карта: пустая галактика не роняет сборку") {
    sim::World world;
    sim::Galaxy galaxy;
    game::WorldView view;
    MapView map;
    MapFrame frame;
    rhi::Camera camera;

    map.build(galaxy, view, 0, Selection{}, camera, frame);
    CHECK(frame.sprites.empty());
    CHECK(frame.lines.empty());
}

TEST_CASE("карта: флот в несуществующей системе не роняет сборку") {
    // Номера приходят по сети. Клиент обязан пережить любое их значение.
    Scene scene(20);
    game::FleetView fleet;
    fleet.id = 1;
    fleet.empire = 0;
    fleet.system = 999999;
    fleet.nextSystem = 888888;
    fleet.composition = sim::makeFleet({{sim::Hull::Corvette, 1}});
    scene.view.fleets[1] = fleet;

    scene.build();
    CHECK(scene.spriteCount() == 20);   // флот пропущен, звёзды на месте
}
