#include "doctest.h"

#include <map>
#include <vector>

#include "pw/sim/galaxy.h"

using namespace pw;
using namespace pw::sim;

namespace {

struct Built {
    World world;
    Galaxy galaxy;

    explicit Built(uint64_t seed = 0xC0FFEE, uint32_t count = 400) {
        registerGalaxyComponents(world);
        GalaxyParams params;
        params.seed = seed;
        params.systemCount = count;
        galaxy.generate(world, params);
    }
};

}  // namespace

TEST_CASE("галактика: генерируется нужное число систем") {
    Built built;
    // Расстановка с отбраковкой по минимальной дистанции может не добрать
    // до цели, но недобор должен быть символическим.
    CHECK(built.galaxy.systemCount() > 380);
    CHECK(built.galaxy.systemCount() <= 400);
}

TEST_CASE("галактика: один сид даёт одну и ту же галактику") {
    Built first(0x1234), second(0x1234);
    CHECK(first.galaxy.hash() == second.galaxy.hash());
    CHECK(first.galaxy.systemCount() == second.galaxy.systemCount());
    CHECK(first.galaxy.laneCount() == second.galaxy.laneCount());
    // И содержимое мира тоже: планеты, классы звёзд, слоты.
    CHECK(first.world.hash() == second.world.hash());
}

TEST_CASE("галактика: разные сиды дают разные галактики") {
    Built first(0x1111), second(0x2222);
    CHECK(first.galaxy.hash() != second.galaxy.hash());
    CHECK(first.world.hash() != second.world.hash());
}

TEST_CASE("галактика: граф связен") {
    // Несвязная галактика — это игроки, до которых нельзя ни долететь,
    // ни довоевать. Такая карта просто сломана, поэтому проверяем на
    // нескольких сидах, а не на одном удачном.
    for (uint64_t seed : {0x1ull, 0x2ull, 0xABCDEFull, 0x50414E45ull}) {
        Built built(seed, 300);
        CAPTURE(seed);
        CHECK(built.galaxy.connected());
    }
}

TEST_CASE("галактика: звёзды не слипаются") {
    Built built;
    GalaxyParams params;
    const fx minSquared = params.minSpacing * params.minSpacing;

    const uint32_t count = built.galaxy.systemCount();
    for (uint32_t i = 0; i < count; ++i) {
        const StarSystem* a = built.world.get<StarSystem>(built.galaxy.systemEntity(i));
        for (uint32_t j = i + 1; j < count; ++j) {
            const StarSystem* b = built.world.get<StarSystem>(built.galaxy.systemEntity(j));
            const fx dx = a->x - b->x, dy = a->y - b->y;
            CHECK((dx * dx + dy * dy) >= minSquared);
        }
    }
}

TEST_CASE("галактика: степень вершин пригодна для стратегии") {
    Built built;
    const uint32_t count = built.galaxy.systemCount();

    uint32_t isolated = 0, total = 0, maxDegree = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t degree = built.galaxy.neighborCount(i);
        if (degree == 0) ++isolated;
        if (degree > maxDegree) maxDegree = degree;
        total += degree;
    }

    // Ни одной системы без связей: тупик, из которого нет выхода, сломал бы
    // и логистику, и войну.
    CHECK(isolated == 0);

    // Средняя степень. Слишком мало — карта превращается в цепочку,
    // слишком много — исчезают горлышки, а с ними позиционная игра.
    const double average = double(total) / double(count);
    CHECK(average > 2.5);
    CHECK(average < 7.0);

    // Каждое ребро посчитано с обеих сторон.
    CHECK(total == built.galaxy.laneCount() * 2);
}

TEST_CASE("галактика: соседи взаимны и отсортированы") {
    Built built(0x777, 200);
    const uint32_t count = built.galaxy.systemCount();

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* list = built.galaxy.neighbors(i);
        const uint32_t degree = built.galaxy.neighborCount(i);

        for (uint32_t k = 0; k + 1 < degree; ++k) {
            CHECK(list[k] < list[k + 1]);  // по возрастанию, обход стабилен
        }
        for (uint32_t k = 0; k < degree; ++k) {
            const uint32_t other = list[k];
            CHECK(other != i);  // петель нет

            // Гиперлиния работает в обе стороны.
            bool mutualLink = false;
            for (uint32_t m = 0; m < built.galaxy.neighborCount(other); ++m) {
                if (built.galaxy.neighbors(other)[m] == i) mutualLink = true;
            }
            CHECK(mutualLink);
        }
    }
}

TEST_CASE("галактика: расстояние в прыжках симметрично и конечно") {
    Built built(0x999, 200);
    const uint32_t count = built.galaxy.systemCount();
    REQUIRE(built.galaxy.connected());

    CHECK(built.galaxy.hopDistance(0, 0) == 0);
    for (uint32_t target : {1u, 50u, 150u, count - 1}) {
        if (target >= count) continue;
        const int32_t forward = built.galaxy.hopDistance(0, target);
        const int32_t backward = built.galaxy.hopDistance(target, 0);
        CAPTURE(target);
        CHECK(forward > 0);
        CHECK(forward == backward);
    }
    CHECK(built.galaxy.hopDistance(0, count + 100) == -1);
}

TEST_CASE("галактика: кольца заполнены от ядра до фронтира") {
    Built built;
    std::map<uint8_t, int> byRing;
    built.world.each<StarSystem>([&](Entity, StarSystem& s) { ++byRing[s.ring]; });

    // Все четыре кольца должны существовать: ядро дорогое, фронтир просторный.
    CHECK(byRing.size() == 4);
    for (const auto& [ring, systems] : byRing) {
        CAPTURE(ring);
        CHECK(systems > 0);
    }
    // Площадь кольца растёт с радиусом, поэтому внешних систем больше.
    CHECK(byRing[3] > byRing[0]);
}

TEST_CASE("галактика: планеты принадлежат существующим системам") {
    Built built;
    const uint32_t count = built.galaxy.systemCount();

    std::vector<int> planetsPerSystem(count, 0);
    built.world.each<Planet>([&](Entity, Planet& p) {
        REQUIRE(p.system < count);
        REQUIRE(p.planetClass < uint8_t(PlanetClass::Count));
        CHECK(p.slots >= 2);
        CHECK(p.slots <= 12);
        ++planetsPerSystem[p.system];
    });

    // Счётчик в системе обязан сойтись с числом реально созданных планет.
    for (uint32_t i = 0; i < count; ++i) {
        const StarSystem* star = built.world.get<StarSystem>(built.galaxy.systemEntity(i));
        CAPTURE(i);
        CHECK(int(star->planetCount) == planetsPerSystem[i]);
    }
}

TEST_CASE("галактика: у чёрных дыр планет нет") {
    Built built;
    built.world.each<StarSystem>([](Entity, StarSystem& s) {
        if (s.starClass == uint8_t(StarClass::BlackHole)) {
            CHECK(s.planetCount == 0);
        }
    });
}

TEST_CASE("галактика: редкие светила встречаются, но остаются редкими") {
    Built built(0x424242, 500);
    std::map<uint8_t, int> byClass;
    built.world.each<StarSystem>([&](Entity, StarSystem& s) { ++byClass[s.starClass]; });

    // Все классы представлены — иначе часть контента мёртвая.
    CHECK(byClass.size() == size_t(StarClass::Count));

    const int total = int(built.galaxy.systemCount());
    // Чёрная дыра — событие, а не фон.
    CHECK(byClass[uint8_t(StarClass::BlackHole)] < total / 10);
    CHECK(byClass[uint8_t(StarClass::Red)] > total / 10);
}

TEST_CASE("галактика: диаметр карты пригоден для игры") {
    // Диаметр в прыжках прямо задаёт темп войны. Слишком большой — флот идёт
    // через галактику полсезона и стратегической глубины нет, только дорога;
    // слишком малый — исчезает логистика, а с ней смысл специализации
    // регионов и перехвата конвоев.
    //
    // Границы стерегут регрессию: параметры формы теперь настраиваемые,
    // и неудачное значение легко сделает карту неиграбельной, не уронив
    // ни одной другой проверки.
    Built built(0xD1A, 500);
    REQUIRE(built.galaxy.connected());

    int32_t diameter = 0;
    for (uint32_t target = 0; target < built.galaxy.systemCount(); ++target) {
        const int32_t hops = built.galaxy.hopDistance(0, target);
        if (hops > diameter) diameter = hops;
    }
    CAPTURE(diameter);
    CHECK(diameter >= 12);
    CHECK(diameter <= 42);
}

TEST_CASE("галактика: диаметр устойчив к сиду") {
    // Первая версия давала от 36 до 59 прыжков в зависимости от сида: часть
    // карт выходила вдвое длиннее других и попросту неиграбельной. Настройка
    // по одному сиду этого не показывает — нужен замер по многим.
    //
    // Проход срезок сжал разброс. Тест стережёт именно разброс, а не среднее:
    // регрессия здесь означает, что части игроков достанется плохая карта.
    int32_t best = 1000, worst = 0;
    for (uint64_t seed : {1ull, 3ull, 8ull, 34ull, 144ull}) {
        World world;
        registerGalaxyComponents(world);
        GalaxyParams params;
        params.seed = seed;
        params.systemCount = 400;
        Galaxy galaxy;
        galaxy.generate(world, params);
        REQUIRE(galaxy.connected());

        int32_t diameter = 0;
        for (uint32_t i = 0; i < galaxy.systemCount(); ++i) {
            const int32_t hops = galaxy.hopDistance(0, i);
            if (hops > diameter) diameter = hops;
        }
        if (diameter < best) best = diameter;
        if (diameter > worst) worst = diameter;
    }
    CAPTURE(best);
    CAPTURE(worst);
    CHECK(worst - best <= 16);
}

TEST_CASE("галактика: срезки не разрушают горлышки") {
    // Срезка добавляется там, где геометрия её и так подсказывает, поэтому
    // связей должно прибавиться немного. Резкий рост степени означал бы, что
    // карта превратилась в решётку, где обороняться негде.
    auto averageDegree = [](uint32_t rounds) {
        World world;
        registerGalaxyComponents(world);
        GalaxyParams params;
        params.seed = 0xF00D;
        params.systemCount = 400;
        params.shortcutRounds = rounds;
        Galaxy galaxy;
        galaxy.generate(world, params);
        uint32_t total = 0;
        for (uint32_t i = 0; i < galaxy.systemCount(); ++i) {
            total += galaxy.neighborCount(i);
        }
        return double(total) / double(galaxy.systemCount());
    };

    const double without = averageDegree(0);
    const double with = averageDegree(3);
    CHECK(with > without);          // связи добавились
    CHECK(with - without < 0.6);    // но немного
}

TEST_CASE("галактика: форма настраивается и параметры действуют") {
    // Доля систем в рукавах — доминирующий рычаг: чем плотнее системы
    // жмутся к рукавам, тем длиннее пути, потому что мостами между
    // соседними рукавами работает именно межрукавное поле.
    auto diameterFor = [](uint32_t armPercent, uint32_t lanes) {
        World world;
        registerGalaxyComponents(world);
        GalaxyParams params;
        params.seed = 0xB0A;
        params.systemCount = 400;
        params.armFractionPercent = armPercent;
        params.lanesPerSystem = lanes;
        Galaxy galaxy;
        galaxy.generate(world, params);
        REQUIRE(galaxy.connected());

        int32_t worst = 0;
        for (uint32_t i = 0; i < galaxy.systemCount(); ++i) {
            const int32_t hops = galaxy.hopDistance(0, i);
            if (hops > worst) worst = hops;
        }
        return worst;
    };

    CHECK(diameterFor(70, 3) < diameterFor(92, 3));
    // Больше линий на систему — короче пути, но и горлышек меньше.
    CHECK(diameterFor(80, 4) < diameterFor(80, 3));
}

TEST_CASE("галактика: размер задаётся параметром") {
    Built small(0x5, 60), large(0x5, 600);
    CHECK(small.galaxy.systemCount() < large.galaxy.systemCount());
    CHECK(small.galaxy.connected());
    CHECK(large.galaxy.connected());
}
