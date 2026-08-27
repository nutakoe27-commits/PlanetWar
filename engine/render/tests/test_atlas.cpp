#include "doctest.h"

#include "assets_path.h"

#include <string>

#include "pw/render/atlas.h"
#include "pw/sim/fleet.h"

using namespace pw;
using namespace pw::render;
using namespace pw::render::testing;

namespace {

/// Имя корпуса в ассетах. Совпадает с id в assets/src/hulls.json.
std::string hullAssetId(sim::Hull hull) {
    switch (hull) {
        case sim::Hull::Corvette:   return "corvette";
        case sim::Hull::Tender:     return "tender";
        case sim::Hull::Colonizer:  return "colonizer";
        case sim::Hull::Destroyer:  return "destroyer";
        case sim::Hull::Carrier:    return "carrier";
        case sim::Hull::Cruiser:    return "cruiser";
        case sim::Hull::Monitor:    return "monitor";
        case sim::Hull::Battleship: return "battleship";
        case sim::Hull::Titan:      return "titan";
        default:                    return {};
    }
}

/// Атлас собирается отдельным шагом и в свежем клоне его нет.
bool loadShips(Atlas& atlas) {
    const std::string path = testing::findAsset("assets/build/ships.json");
    return !path.empty() && atlas.load(path);
}

}  // namespace

TEST_CASE("атлас: описание из Blender читается") {
    Atlas atlas;
    if (!loadShips(atlas)) {
        MESSAGE("атлас не собран — пропускаю (tools/blender/build_assets.py)");
        return;
    }

    CHECK(atlas.valid());
    CHECK(atlas.textureWidth() > 0);
    CHECK(atlas.textureWidth() == atlas.textureHeight());
    CHECK(atlas.rotationSteps() > 0);

    // СОСТАВ, А НЕ РАЗМЕР. Раньше здесь стояли 1024, 8 поворотов и
    // «4 корпуса × 8 + 5 звёзд» — числа CI-качества. Тест при этом молча
    // пропускался из каталога сборки (пути к ассетам искались тремя «..»,
    // а до корня их четыре), поэтому годами никто не замечал, что он
    // проверяет конкретный ПРОФИЛЬ сборки, а не пайплайн.
    //
    // Проверять надо инвариант: у каждого корпуса ровно столько кадров,
    // сколько в манифесте поворотов, а у каждой звезды — один. Он верен
    // при любом качестве, и именно он ломается, когда корпус забыли
    // добавить в сборку.
    const size_t steps = size_t(atlas.rotationSteps());
    for (uint8_t hull = 1; hull < uint8_t(sim::Hull::Count); ++hull) {
        const std::string id = hullAssetId(sim::Hull(hull));
        CAPTURE(id);
        size_t found = 0;
        for (const AtlasFrame& frame : atlas.frames()) {
            if (frame.hull == id) ++found;
        }
        CHECK(found == steps);
    }
    for (const char* star : {"star_red", "star_yellow", "star_blue", "star_neutron",
                             "star_blackhole"}) {
        CAPTURE(star);
        size_t found = 0;
        for (const AtlasFrame& frame : atlas.frames()) {
            if (frame.hull == star) ++found;
        }
        // Звезде хватает одного ракурса: шар одинаков со всех сторон.
        CHECK(found == 1);
    }
    CHECK(atlas.frames().size() == steps * sim::kHullClasses + 5);
}

TEST_CASE("атлас: у каждого класса светила есть свой кадр") {
    Atlas atlas;
    if (!loadShips(atlas)) return;

    for (const char* star : {"star_red", "star_yellow", "star_blue", "star_neutron",
                             "star_blackhole"}) {
        CAPTURE(star);
        const AtlasFrame* frame = atlas.frame(star, 0.0f);
        REQUIRE(frame != nullptr);
        CHECK(frame->width > 0);
    }
}

TEST_CASE("атлас: звёзды НЕ принимают цвет империи") {
    // Класс светила — свойство мира, а не игрока. Если маска акцента
    // накроет звезду, все светила станут одного цвета, и класс перестанет
    // читаться с карты. Первая версия ошиблась ровно здесь: маска красила
    // ореол белым, и звёзды выходили одинаково белыми.
    Atlas atlas;
    if (!loadShips(atlas)) return;
    if (atlas.accentMask().empty()) return;

    const int width = atlas.textureWidth();
    for (const AtlasFrame& frame : atlas.frames()) {
        if (frame.hull.rfind("star_", 0) != 0) continue;
        CAPTURE(frame.hull);

        long accent = 0;
        for (int y = frame.y; y < frame.y + frame.height; ++y) {
            for (int x = frame.x; x < frame.x + frame.width; ++x) {
                accent += atlas.accentMask()[size_t(y) * size_t(width) + size_t(x)].r;
            }
        }
        // Порог, а не строгий ноль: рендер оставляет по краям геометрии
        // единицы из 255 — шум округления sRGB, который глазу не виден
        // и на цвет не влияет. Требовать точного нуля значило бы получить
        // тест, падающий от смены версии Blender.
        //
        // Замер на текущем атласе: у звёзд средняя маска 0,08 из 255,
        // у кораблей 12–15. Порог 2 лежит между ними с большим запасом
        // в обе стороны.
        const long pixels = long(frame.width) * long(frame.height);
        CHECK(accent < pixels * 2);
    }
}

TEST_CASE("атлас: корабли ПРИНИМАЮТ цвет империи") {
    // Обратная сторона: если маска пуста и у кораблей, все флоты будут
    // серыми, и владение флотом перестанет читаться.
    Atlas atlas;
    if (!loadShips(atlas)) return;
    if (atlas.accentMask().empty()) return;

    const int width = atlas.textureWidth();
    for (const char* hull : {"corvette", "destroyer", "cruiser", "battleship"}) {
        const AtlasFrame* frame = atlas.frame(hull, 0.0f);
        REQUIRE(frame != nullptr);
        CAPTURE(std::string(hull));

        long accent = 0;
        for (int y = frame->y; y < frame->y + frame->height; ++y) {
            for (int x = frame->x; x < frame->x + frame->width; ++x) {
                accent += atlas.accentMask()[size_t(y) * size_t(width) + size_t(x)].r;
            }
        }
        // Существенно выше шума: акцент на корабле — это реальные пиксели,
        // а не единицы округления по краям (замер: 12–15 против 0,08).
        const long pixels = long(frame->width) * long(frame->height);
        CHECK(accent > pixels * 5);
    }
}

TEST_CASE("атлас: у каждого корпуса есть все направления") {
    Atlas atlas;
    if (!loadShips(atlas)) return;

    for (const char* hull : {"corvette", "destroyer", "cruiser", "battleship"}) {
        for (uint32_t step = 0; step < atlas.rotationSteps(); ++step) {
            const float turns = float(step) / float(atlas.rotationSteps());
            const AtlasFrame* frame = atlas.frame(hull, turns);
            REQUIRE(frame != nullptr);
            CHECK(frame->hull == hull);
            CHECK(frame->rotation == step);
        }
    }
}

TEST_CASE("атлас: координаты кадров лежат внутри текстуры") {
    // Кадр, вылезший за край, притащил бы в картинку соседний кадр —
    // и корабль был бы наполовину другим кораблём.
    Atlas atlas;
    if (!loadShips(atlas)) return;

    for (const AtlasFrame& frame : atlas.frames()) {
        CAPTURE(frame.hull);
        CHECK(frame.u0 >= 0.0f);
        CHECK(frame.v0 >= 0.0f);
        CHECK(frame.u1 <= 1.0f);
        CHECK(frame.v1 <= 1.0f);
        CHECK(frame.u1 > frame.u0);
        CHECK(frame.v1 > frame.v0);
        CHECK(frame.width > 0);
        CHECK(frame.height > 0);
    }
}

TEST_CASE("атлас: кадры не перекрываются") {
    Atlas atlas;
    if (!loadShips(atlas)) return;

    const auto& frames = atlas.frames();
    for (size_t a = 0; a < frames.size(); ++a) {
        for (size_t b = a + 1; b < frames.size(); ++b) {
            const bool apart = frames[a].u1 <= frames[b].u0 || frames[b].u1 <= frames[a].u0 ||
                               frames[a].v1 <= frames[b].v0 || frames[b].v1 <= frames[a].v0;
            CHECK(apart);
        }
    }
}

TEST_CASE("атлас: поворот приводится к ближайшему испечённому направлению") {
    Atlas atlas;
    if (!loadShips(atlas)) return;
    const int steps = atlas.rotationSteps();
    REQUIRE(steps > 0);

    // Формулируем через ЧИСЛО НАПРАВЛЕНИЙ из манифеста, а не через
    // восьмёрку: число поворотов задаётся качеством сборки, а правило
    // округления — нет.
    CHECK(atlas.frame("cruiser", 0.0f)->rotation == 0);
    CHECK(atlas.frame("cruiser", 0.49f)->rotation == steps / 2);

    // Половина шага округляется вверх, чуть меньше половины — вниз.
    const float step = 1.0f / float(steps);
    CHECK(atlas.frame("cruiser", step * 0.49f)->rotation == 0);
    CHECK(atlas.frame("cruiser", step * 0.51f)->rotation == 1);

    // Полный оборот и отрицательный угол — то же направление.
    CHECK(atlas.frame("cruiser", 1.0f)->rotation == 0);
    CHECK(atlas.frame("cruiser", -0.25f)->rotation == steps * 3 / 4);
    CHECK(atlas.frame("cruiser", 2.5f)->rotation == steps / 2);
}

TEST_CASE("атлас: несуществующий корпус не роняет") {
    Atlas atlas;
    if (!loadShips(atlas)) return;
    CHECK(atlas.frame("нет такого") == nullptr);
    CHECK(atlas.frame("нет такого", 0.3f) == nullptr);
}

TEST_CASE("атлас: отсутствующий файл даёт внятную ошибку, а не падение") {
    Atlas atlas;
    CHECK_FALSE(atlas.load("такого/файла/нет.json"));
    CHECK_FALSE(atlas.error().empty());
    CHECK_FALSE(atlas.valid());
}

TEST_CASE("атлас: сборная текстура того же размера, что и альбедо") {
    Atlas atlas;
    if (!loadShips(atlas)) return;

    const auto composite = atlas.composite();
    CHECK(composite.size() == atlas.albedo().size());
    CHECK(composite.size() == size_t(atlas.textureWidth()) * size_t(atlas.textureHeight()));

    // Маска акцента должна что-то менять: если сборка совпадает с альбедо
    // байт в байт, цвет империи не ляжет никуда, и все флоты будут серыми.
    if (!atlas.accentMask().empty()) {
        size_t changed = 0;
        for (size_t i = 0; i < composite.size(); ++i) {
            if (composite[i].r != atlas.albedo()[i].r) ++changed;
        }
        CHECK(changed > 0);
    }
}
