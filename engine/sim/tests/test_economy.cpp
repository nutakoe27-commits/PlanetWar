#include "doctest.h"

#include <cmath>

#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

using namespace pw;
using namespace pw::sim;

namespace {

struct Economy {
    World world;
    Galaxy galaxy;
    Ledger ledger;
    Entity empireEntity;

    explicit Economy(uint32_t systems = 40) {
        registerGalaxyComponents(world);
        registerFleetComponents(world);
        registerControlComponents(world);
        registerEconomyComponents(world);

        GalaxyParams params;
        params.systemCount = systems;
        galaxy.generate(world, params);
        initialiseControl(world, galaxy);

        world.setResource(&galaxy);
        world.setResource(&ledger);

        empireEntity = world.create();
        world.add<Empire>(empireEntity, Empire{fx::zero(), fx::zero(), fx::zero(),
                                               fx::zero(), fx::zero(), /*id=*/0,
                                               /*capital=*/0});
    }

    Empire& empire() { return *world.get<Empire>(empireEntity); }

    void own(uint32_t system, uint32_t who = 0) {
        world.get<Owner>(galaxy.systemEntity(system))->empire = who;
    }

    /// Планета с заданным классом и застройкой.
    Entity colonise(uint32_t system, PlanetClass klass, Specialization spec,
                    std::initializer_list<Building> buildings) {
        const Entity planet = world.create();
        world.add<Planet>(planet, Planet{system, uint8_t(klass), kMaxSlots,
                                         uint8_t(spec), /*orbit=*/0});

        PlanetDevelopment development{};
        uint8_t slot = 0;
        for (Building building : buildings) {
            development.buildings[slot++] = uint8_t(building);
        }
        world.add<PlanetDevelopment>(planet, development);
        return planet;
    }

    Entity garrison(uint32_t system, const Fleet& composition, uint32_t who = 0) {
        const Entity e = world.create();
        world.add<Fleet>(e, composition);
        world.add<FleetLocation>(e, FleetLocation{system, system, fx::zero()});
        world.add<Owner>(e, Owner{who, 0});
        return e;
    }

    void run(int64_t ticks) {
        for (int64_t i = 0; i < ticks; ++i) {
            TickContext context;
            context.tick = uint64_t(i);
            systemEconomy(world, context);
        }
    }
};

constexpr int64_t kSecond = kTicksPerSecond;

/// Сравнение с допуском: накопление за тысячи тиков копит и ошибку округления.
bool near(fx value, double expected, double tolerance = 0.5) {
    return std::abs(value.toDouble() - expected) <= tolerance;
}

}  // namespace

// ---------------------------------------------------------------------------
// Производство
// ---------------------------------------------------------------------------

TEST_CASE("экономика: шахта даёт минералы") {
    Economy world;
    world.own(1);
    world.colonise(1, PlanetClass::Desert, Specialization::None, {Building::Mine});

    world.run(100 * kSecond);
    // 0.2 минерала в секунду на сто секунд.
    CHECK(near(world.empire().minerals, 20.0));
}

TEST_CASE("экономика: ничья система не даёт ничего") {
    Economy world;
    // Владельца не назначаем.
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Mine, Building::Mine, Building::Mine});

    world.run(100 * kSecond);
    CHECK(world.empire().minerals == fx::zero());
}

TEST_CASE("экономика: потеря системы отнимает её выработку") {
    Economy world;
    world.own(1);
    world.colonise(1, PlanetClass::Desert, Specialization::None, {Building::Mine});

    world.run(50 * kSecond);
    const fx afterFifty = world.empire().minerals;
    CHECK(afterFifty > fx::zero());

    // Систему захватили. Планеты уходят вместе с ней — это и делает захват
    // значимым событием, а не косметикой на карте.
    world.own(1, /*who=*/7);
    world.run(50 * kSecond);
    CHECK(world.empire().minerals == afterFifty);
}

TEST_CASE("экономика: специализация и класс планеты умножают выработку") {
    auto mineralsAfter = [](PlanetClass klass, Specialization spec) {
        Economy world;
        world.own(1);
        world.colonise(1, klass, spec, {Building::Mine});
        world.run(100 * kSecond);
        return world.empire().minerals.toDouble();
    };

    const double plain = mineralsAfter(PlanetClass::Ocean, Specialization::None);
    const double specialised = mineralsAfter(PlanetClass::Ocean, Specialization::Mining);
    const double onClass = mineralsAfter(PlanetClass::AsteroidBelt, Specialization::None);
    const double both = mineralsAfter(PlanetClass::AsteroidBelt, Specialization::Mining);

    CHECK(specialised > plain * 1.4);
    CHECK(onClass > plain * 1.2);
    // Профильное здание на профильной планете даёт почти вдвое — этого хватает,
    // чтобы специализироваться было выгодно, но непрофильное строительство
    // не становилось бессмысленным.
    CHECK(both > plain * 1.8);
    CHECK(both < plain * 2.0);
}

// ---------------------------------------------------------------------------
// Цепочка минералы -> сплавы
// ---------------------------------------------------------------------------

TEST_CASE("цепочка: завод плавит сплавы из минералов") {
    Economy world;
    world.own(1);
    // Шахтёрский мир и индустриальный: два завода и четыре шахты.
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Mine, Building::Mine, Building::Mine, Building::Mine});
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Foundry, Building::Foundry});

    world.run(100 * kSecond);

    // Два завода по 0.2 сплава в секунду.
    CHECK(near(world.empire().alloys, 40.0, 1.0));
    // Четыре шахты дали 80 минералов, два завода съели 80 — в остатке около нуля.
    CHECK(world.empire().minerals < fx::fromInt(5));
}

TEST_CASE("цепочка: без минералов завод встаёт") {
    Economy world;
    world.own(1);
    // Заводы без единой шахты. Индустриальный мир без шахтёрского бесполезен —
    // ровно то, ради чего цепочка и вводилась.
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Foundry, Building::Foundry});

    world.run(100 * kSecond);
    CHECK(world.empire().alloys == fx::zero());
    CHECK(world.ledger.at(0).foundryIdle > fx::zero());
}

TEST_CASE("цепочка: нехватки хватает на частичную работу") {
    Economy world;
    world.own(1);
    // Одна шахта на четыре завода: сырья хватает на половину мощности.
    world.colonise(1, PlanetClass::Desert, Specialization::None, {Building::Mine});
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Foundry, Building::Foundry});

    world.run(200 * kSecond);

    // Один завод обеспечен наполовину... точнее, шахта даёт 0.2, два завода
    // просят 0.8 — работает четверть мощности.
    const double alloys = world.empire().alloys.toDouble();
    CHECK(alloys > 5.0);
    CHECK(alloys < 30.0);
    CHECK(world.ledger.at(0).foundryIdle > fx::zero());
}

// ---------------------------------------------------------------------------
// Содержание
// ---------------------------------------------------------------------------

TEST_CASE("содержание: здания и флот едят энергию") {
    Economy world;
    world.own(1);
    world.empire().energy = fx::fromInt(1000);
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Mine, Building::Mine, Building::Mine, Building::Mine});

    world.run(100 * kSecond);
    const fx afterBuildings = world.empire().energy;
    // Четыре здания по 0.05 энергии в секунду — минус двадцать за сто секунд.
    CHECK(near(afterBuildings, 980.0));

    // Флот в двадцать корветов добавляет расход.
    world.garrison(1, Fleet{20, 0, 0, 0});
    world.run(100 * kSecond);
    CHECK(world.empire().energy < afterBuildings - fx::fromInt(20));
}

TEST_CASE("содержание: электростанция перекрывает расход") {
    Economy world;
    world.own(1);
    world.colonise(1, PlanetClass::GasGiant, Specialization::None,
                   {Building::PowerPlant, Building::Mine});

    world.run(100 * kSecond);
    // Станция на газовом гиганте даёт 0.5 * 1.25 = 0.625, два здания просят
    // 0.1 — империя в плюсе.
    CHECK(world.empire().energy > fx::fromInt(40));
}

TEST_CASE("содержание: ресурсы не уходят в минус") {
    Economy world;
    world.own(1);
    // Ни одного производящего здания, зато большой флот.
    world.colonise(1, PlanetClass::Desert, Specialization::None, {Building::Fortress});
    world.garrison(1, Fleet{0, 0, 0, 50});

    world.run(500 * kSecond);
    // Дефицит — это сигнал, а не долг: отключения появятся отдельной
    // механикой, но отрицательного склада быть не должно.
    CHECK(world.empire().energy == fx::zero());
}

// ---------------------------------------------------------------------------
// Крепости
// ---------------------------------------------------------------------------

TEST_CASE("крепость: поднимает потолок обороны системы") {
    Economy world;
    world.own(1);
    world.colonise(1, PlanetClass::Barren, Specialization::Fortress,
                   {Building::Fortress, Building::Fortress});

    TickContext context;
    systemDefenceCap(world.world, context);

    const SystemDefense* defense =
        world.world.get<SystemDefense>(world.galaxy.systemEntity(1));
    CHECK(defense->maxReadiness == kReadinessMax + kFortressReadiness * fx::fromInt(2));

    // А система без крепостей остаётся при базовом потолке.
    const SystemDefense* plain =
        world.world.get<SystemDefense>(world.galaxy.systemEntity(2));
    CHECK(plain->maxReadiness == kReadinessMax);
}

TEST_CASE("крепость: снос обрезает уже накопленную готовность") {
    Economy world;
    world.own(1);
    const Entity planet = world.colonise(1, PlanetClass::Barren, Specialization::None,
                                         {Building::Fortress});
    TickContext context;
    systemDefenceCap(world.world, context);

    SystemDefense* defense = world.world.get<SystemDefense>(world.galaxy.systemEntity(1));
    defense->readiness = defense->maxReadiness;
    CHECK(defense->readiness > kReadinessMax);

    // Крепость снесли — оборона обязана просесть сразу, а не остаться висеть.
    world.world.get<PlanetDevelopment>(planet)->buildings[0] = uint8_t(Building::None);
    systemDefenceCap(world.world, context);
    CHECK(defense->readiness == kReadinessMax);
}

// ---------------------------------------------------------------------------
// Детерминизм
// ---------------------------------------------------------------------------

TEST_CASE("экономика: воспроизводится тик в тик") {
    Economy first, second;
    for (Economy* world : {&first, &second}) {
        for (uint32_t system = 1; system <= 5; ++system) {
            world->own(system);
            world->colonise(system, PlanetClass(system % 6),
                            Specialization(system % 6),
                            {Building::Mine, Building::Foundry, Building::PowerPlant,
                             Building::Laboratory});
        }
        world->garrison(1, Fleet{12, 4, 2, 1});
    }
    REQUIRE(first.world.hash() == second.world.hash());

    first.run(3000 * kSecond);
    second.run(3000 * kSecond);

    CHECK(first.world.hash() == second.world.hash());
    // И экономика действительно работала, а не стояла.
    CHECK(first.empire().alloys > fx::zero());
    CHECK(first.empire().research > fx::zero());
}
