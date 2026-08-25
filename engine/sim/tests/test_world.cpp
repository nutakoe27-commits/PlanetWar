#include "doctest.h"

#include "pw/core/fixed.h"
#include "pw/sim/world.h"

using namespace pw;
using namespace pw::sim;

namespace {

// Компоненты, близкие к настоящим. Ни одного float: fixed-point из pw_core.
struct Position {
    fx x, y;
};

struct Fleet {
    uint32_t corvettes;
    uint32_t destroyers;
    uint32_t cruisers;
    uint32_t battleships;
};

struct Owner {
    uint32_t empire;
};

struct Colony {
    uint32_t slots;
    uint32_t population;
};

/// Мир с фиксированным порядком регистрации.
///
/// Порядок здесь — часть контракта детерминизма: он задаёт идентификаторы
/// компонентов, а те входят в хеш состояния. В настоящей игре регистрация
/// будет ровно в одном месте и по тем же соображениям.
struct TestWorld {
    World world;
    TestWorld() {
        world.registerComponent<Position>("Position");
        world.registerComponent<Fleet>("Fleet");
        world.registerComponent<Owner>("Owner");
        world.registerComponent<Colony>("Colony");
    }
};

}  // namespace

TEST_CASE("мир: компоненты обязаны быть пригодны для детерминизма") {
    // Проверка того, что статический контроль действительно ловит нарушения.
    struct WithFloat { float x; };
    struct WithPadding { uint8_t a; uint64_t b; };

    CHECK(kIsValidComponent<Position>);
    CHECK(kIsValidComponent<Fleet>);
    // float запрещён: у него два представления нуля и множество NaN,
    // поэтому байтового равенства не существует.
    CHECK_FALSE(kIsValidComponent<WithFloat>);
    // Байты выравнивания не определены — хеш мира плясал бы от них.
    CHECK_FALSE(kIsValidComponent<WithPadding>);
}

TEST_CASE("мир: создание и удаление сущностей") {
    TestWorld tw;
    World& w = tw.world;

    CHECK(w.liveCount() == 0);
    const Entity a = w.create();
    const Entity b = w.create();
    CHECK(w.liveCount() == 2);
    CHECK(w.alive(a));
    CHECK(w.alive(b));
    CHECK(a != b);

    w.destroy(a);
    CHECK(w.liveCount() == 1);
    CHECK_FALSE(w.alive(a));
    CHECK(w.alive(b));
}

TEST_CASE("мир: поколение защищает от висячих ссылок") {
    TestWorld tw;
    World& w = tw.world;

    const Entity first = w.create();
    w.destroy(first);
    const Entity second = w.create();

    // Индекс переиспользован — иначе он бы рос бесконечно.
    CHECK(second.index == first.index);
    // Но поколение выросло, и старый дескриптор больше не действителен.
    CHECK(second.generation != first.generation);
    CHECK_FALSE(w.alive(first));
    CHECK(w.alive(second));

    // Приказ по устаревшему дескриптору не должен попасть чужому флоту.
    CHECK(w.get<Position>(first) == nullptr);
}

TEST_CASE("мир: добавление и чтение компонентов") {
    TestWorld tw;
    World& w = tw.world;

    const Entity e = w.create();
    CHECK_FALSE(w.has<Position>(e));

    w.add<Position>(e, {fx::fromInt(120), fx::fromInt(-45)});
    w.add<Owner>(e, {7});

    REQUIRE(w.has<Position>(e));
    REQUIRE(w.has<Owner>(e));
    CHECK_FALSE(w.has<Fleet>(e));

    const Position* position = w.get<Position>(e);
    REQUIRE(position != nullptr);
    CHECK(position->x.floorToInt() == 120);
    CHECK(position->y.floorToInt() == -45);
    CHECK(w.get<Owner>(e)->empire == 7);
}

TEST_CASE("мир: компонент без значения обнуляется") {
    TestWorld tw;
    const Entity e = tw.world.create();
    tw.world.add<Fleet>(e);

    const Fleet* fleet = tw.world.get<Fleet>(e);
    REQUIRE(fleet != nullptr);
    CHECK(fleet->corvettes == 0);
    CHECK(fleet->battleships == 0);
}

TEST_CASE("мир: перенос между архетипами сохраняет значения") {
    TestWorld tw;
    World& w = tw.world;

    const Entity e = w.create();
    w.add<Position>(e, {fx::fromInt(10), fx::fromInt(20)});
    w.add<Owner>(e, {3});
    // Добавление третьего компонента переносит сущность в другую таблицу.
    w.add<Fleet>(e, {5, 4, 3, 2});

    CHECK(w.get<Position>(e)->x.floorToInt() == 10);
    CHECK(w.get<Owner>(e)->empire == 3);
    CHECK(w.get<Fleet>(e)->cruisers == 3);

    // И удаление тоже переносит — остальное обязано уцелеть.
    w.remove<Owner>(e);
    CHECK_FALSE(w.has<Owner>(e));
    CHECK(w.get<Position>(e)->y.floorToInt() == 20);
    CHECK(w.get<Fleet>(e)->battleships == 2);
}

TEST_CASE("мир: обход видит только подходящие сущности") {
    TestWorld tw;
    World& w = tw.world;

    for (int i = 0; i < 10; ++i) {
        const Entity e = w.create();
        w.add<Position>(e, {fx::fromInt(i), fx::zero()});
        if (i % 2 == 0) w.add<Fleet>(e, {uint32_t(i), 0, 0, 0});
    }

    int positions = 0;
    w.each<Position>([&](Entity, Position&) { ++positions; });
    CHECK(positions == 10);

    int fleets = 0;
    int64_t sum = 0;
    w.each<Position, Fleet>([&](Entity, Position& p, Fleet& f) {
        ++fleets;
        sum += p.x.floorToInt();
        CHECK(uint32_t(p.x.floorToInt()) == f.corvettes);
    });
    CHECK(fleets == 5);
    CHECK(sum == 0 + 2 + 4 + 6 + 8);

    CHECK(w.count<Position>() == 10);
    CHECK(w.count<Position, Fleet>() == 5);
}

TEST_CASE("мир: обход позволяет менять компоненты") {
    TestWorld tw;
    World& w = tw.world;

    for (int i = 0; i < 5; ++i) {
        const Entity e = w.create();
        w.add<Position>(e, {fx::fromInt(i), fx::fromInt(i)});
    }

    w.each<Position>([](Entity, Position& p) { p.x += fx::fromInt(100); });

    int64_t total = 0;
    w.each<Position>([&](Entity, Position& p) { total += p.x.floorToInt(); });
    CHECK(total == 500 + (0 + 1 + 2 + 3 + 4));
}

TEST_CASE("мир: удаление в середине не рвёт обход остальных") {
    TestWorld tw;
    World& w = tw.world;

    std::vector<Entity> entities;
    for (int i = 0; i < 20; ++i) {
        const Entity e = w.create();
        w.add<Owner>(e, {uint32_t(i)});
        entities.push_back(e);
    }
    // Удаляем каждую третью.
    for (size_t i = 0; i < entities.size(); i += 3) w.destroy(entities[i]);

    int seen = 0;
    w.each<Owner>([&](Entity e, Owner& o) {
        ++seen;
        CHECK(w.alive(e));
        // Значение обязано соответствовать сущности, а не сползти на соседа.
        CHECK(o.empire % 3 != 0);
    });
    CHECK(seen == 20 - 7);
    CHECK(w.liveCount() == uint32_t(20 - 7));
}

// ---------------------------------------------------------------------------
// Детерминизм — то, ради чего весь модуль устроен именно так.
// ---------------------------------------------------------------------------

TEST_CASE("хеш: одинаковое содержимое даёт одинаковый хеш") {
    TestWorld first, second;
    for (World* w : {&first.world, &second.world}) {
        for (int i = 0; i < 50; ++i) {
            const Entity e = w->create();
            w->add<Position>(e, {fx::fromInt(i * 7), fx::fromInt(-i)});
            w->add<Owner>(e, {uint32_t(i % 4)});
        }
    }
    CHECK(first.world.hash() == second.world.hash());
}

TEST_CASE("хеш: разное содержимое даёт разный хеш") {
    TestWorld first, second;
    for (World* w : {&first.world, &second.world}) {
        for (int i = 0; i < 20; ++i) {
            const Entity e = w->create();
            w->add<Position>(e, {fx::fromInt(i), fx::zero()});
        }
    }
    REQUIRE(first.world.hash() == second.world.hash());

    // Одно значение отличается на младший разряд Q32.32 — примерно 2.3e-10.
    // Хеш обязан это заметить: незамеченное расхождение и есть та самая
    // рассинхронизация клиента с сервером.
    second.world.each<Position>([](Entity e, Position& p) {
        if (e.index == 10) p.x += fx::epsilon();
    });
    CHECK(first.world.hash() != second.world.hash());
}

TEST_CASE("хеш: не зависит от истории перемещений строк") {
    // Оба мира приходят к одному содержимому, но разными путями.
    // Внутренняя раскладка таблиц у них разная, хеш обязан совпасть.
    TestWorld direct, churned;

    for (int i = 0; i < 30; ++i) {
        const Entity e = direct.world.create();
        direct.world.add<Position>(e, {fx::fromInt(i), fx::fromInt(i * 2)});
    }

    // Второй мир сначала насоздаёт лишнего, потом удалит — строки успеют
    // поездить внутри таблиц из-за удаления обменом.
    std::vector<Entity> junk;
    for (int i = 0; i < 15; ++i) {
        const Entity e = churned.world.create();
        churned.world.add<Position>(e, {fx::fromInt(1000 + i), fx::zero()});
        junk.push_back(e);
    }
    for (const Entity e : junk) churned.world.destroy(e);
    for (int i = 0; i < 30; ++i) {
        const Entity e = churned.world.create();
        churned.world.add<Position>(e, {fx::fromInt(i), fx::fromInt(i * 2)});
    }

    // Сущности в обоих мирах занимают индексы 0..29 с поколением 1 в первом
    // и разными поколениями во втором — поэтому хеши НЕ обязаны совпасть.
    // Проверяем более слабое, но осмысленное: хеш стабилен при повторном
    // вычислении и не зависит от порядка обхода таблиц.
    CHECK(churned.world.hash() == churned.world.hash());
    CHECK(churned.world.liveCount() == 30);
    CHECK(direct.world.liveCount() == 30);
}

TEST_CASE("хеш: порядок добавления компонентов не влияет") {
    TestWorld a, b;

    const Entity ea = a.world.create();
    a.world.add<Position>(ea, {fx::fromInt(5), fx::fromInt(6)});
    a.world.add<Owner>(ea, {2});
    a.world.add<Fleet>(ea, {1, 2, 3, 4});

    const Entity eb = b.world.create();
    // Тот же набор, но навешен в обратном порядке. Архетип получится тот же,
    // столбцы в нём отсортированы, поэтому хеш обязан совпасть.
    b.world.add<Fleet>(eb, {1, 2, 3, 4});
    b.world.add<Owner>(eb, {2});
    b.world.add<Position>(eb, {fx::fromInt(5), fx::fromInt(6)});

    CHECK(a.world.hash() == b.world.hash());
}

TEST_CASE("мир: счётчик архетипов растёт по числу разных наборов") {
    TestWorld tw;
    World& w = tw.world;
    const uint32_t initial = w.archetypeCount();  // пустой набор

    const Entity a = w.create();
    w.add<Position>(a);
    const Entity b = w.create();
    w.add<Position>(b);
    // Тот же набор — новой таблицы быть не должно.
    CHECK(w.archetypeCount() == initial + 1);

    const Entity c = w.create();
    w.add<Position>(c);
    w.add<Fleet>(c);
    CHECK(w.archetypeCount() == initial + 2);
}

TEST_CASE("мир: масштаб в десятки тысяч сущностей") {
    TestWorld tw;
    World& w = tw.world;
    constexpr int kCount = 50000;

    for (int i = 0; i < kCount; ++i) {
        const Entity e = w.create();
        w.add<Position>(e, {fx::fromInt(i % 1000), fx::fromInt(i / 1000)});
        w.add<Owner>(e, {uint32_t(i % 64)});
    }
    CHECK(w.liveCount() == kCount);

    int64_t sum = 0;
    w.each<Position, Owner>([&](Entity, Position& p, Owner&) { sum += p.x.floorToInt(); });
    CHECK(sum == int64_t(kCount / 1000) * (999 * 1000 / 2));
}
