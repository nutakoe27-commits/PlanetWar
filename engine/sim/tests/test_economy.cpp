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

    /// Отдать империи систему целиком — со всеми её планетами.
    ///
    /// Выработка достаётся владельцу ПЛАНЕТЫ, а не системы, поэтому
    /// проставить одно поле у системы больше недостаточно: она пустая
    /// запись, производят планеты.
    void own(uint32_t system, uint32_t who = 0) {
        world.get<Owner>(galaxy.systemEntity(system))->empire = who;
        world.each<Planet, Owner>([&](Entity, Planet& planet, Owner& owner) {
            if (planet.system == system) owner.empire = who;
        });
    }

    /// Планета с заданным классом и застройкой.
    ///
    /// Владелец ставится тут же: планета создаётся уже после
    /// initialiseControl, и навесить на неё владение задним числом
    /// значило бы менять таблицы посреди обхода.
    Entity colonise(uint32_t system, PlanetClass klass, Specialization spec,
                    std::initializer_list<Building> buildings, uint32_t who = 0) {
        const Entity planet = world.create();
        world.add<Planet>(planet, Planet{system, uint8_t(klass), kMaxSlots,
                                         uint8_t(spec), /*orbit=*/0});
        world.add<Owner>(planet, Owner{who, 0});
        world.add<PlanetDefense>(planet, PlanetDefense{fx::zero(), kReadinessMax});

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

TEST_CASE("экономика: ничья планета не даёт ничего") {
    Economy world;
    // Владельца не назначаем.
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Mine, Building::Mine, Building::Mine}, kNoEmpire);

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

TEST_CASE("крепость: поднимает потолок обороны СВОЕЙ планеты") {
    // Раньше крепости считались по системе, и крепость на пустом камне
    // укрепляла соседний мир-столицу: оборона строилась там, где дешевле,
    // а не там, где важно. Теперь укрепление — выбор конкретной планеты.
    Economy world;
    world.own(1);
    const Entity fortified = world.colonise(1, PlanetClass::Barren, Specialization::Fortress,
                                            {Building::Fortress, Building::Fortress});
    const Entity plain = world.colonise(1, PlanetClass::Barren, Specialization::None,
                                        {Building::Mine});

    TickContext context;
    planetDefenceCap(world.world, context);

    CHECK(world.world.get<PlanetDefense>(fortified)->maxReadiness ==
          kReadinessMax + kFortressReadiness * fx::fromInt(2));
    // Соседняя планета той же системы остаётся при базовом потолке.
    CHECK(world.world.get<PlanetDefense>(plain)->maxReadiness == kReadinessMax);
}

TEST_CASE("крепость: снос обрезает уже накопленную готовность") {
    Economy world;
    world.own(1);
    const Entity planet = world.colonise(1, PlanetClass::Barren, Specialization::None,
                                         {Building::Fortress});
    TickContext context;
    planetDefenceCap(world.world, context);

    PlanetDefense* defense = world.world.get<PlanetDefense>(planet);
    defense->readiness = defense->maxReadiness;
    CHECK(defense->readiness > kReadinessMax);

    // Крепость снесли — оборона обязана просесть сразу, а не остаться висеть.
    world.world.get<PlanetDevelopment>(planet)->buildings[0] = uint8_t(Building::None);
    planetDefenceCap(world.world, context);
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

// ---------------------------------------------------------------------------
// Инфраструктура и тормоз снежного кома
// ---------------------------------------------------------------------------

namespace {

/// Сколько энергии империя потеряла за секунду при заданном числе планет.
///
/// Считаем именно РАСХОД: содержание — это единственное, что зависит
/// от размера империи, и мерить его надо в отрыве от доходов.
double fleetUpkeepPerSecond(uint32_t planetCount, uint32_t depots) {
    Economy world;
    world.own(1);

    // Планеты БЕЗ застройки: нам нужен только их счёт, а не их доход.
    for (uint32_t index = 0; index < planetCount; ++index) {
        world.colonise(1, PlanetClass::Barren, Specialization::None, {});
    }
    for (uint32_t index = 0; index < depots; ++index) {
        world.colonise(1, PlanetClass::Barren, Specialization::None,
                       {Building::SupplyDepot});
    }
    world.garrison(1, makeFleet({{Hull::Battleship, 10}}));

    // Читаем ПОТОК из учётной книги, а не убыль казны: казна обрезается
    // по нулю, а стартовая энергия у империи как раз ноль — расход был бы
    // не виден вовсе. Ровно на этом первая версия теста и показала «0 > 0».
    world.run(1);
    return -world.ledger.at(0).energy.toDouble();
}

}  // namespace

TEST_CASE("содержание: большая империя платит за флот дороже") {
    // ГЛАВНОЕ ЧИСЛО ДЛЯ СЕРВЕРА НА ТЫСЯЧУ ИГРОКОВ. Без него первый, кто
    // вырвался вперёд, растёт быстрее всех просто потому, что уже впереди:
    // больше планет — больше дохода — больше флота — больше планет.
    // К третьей неделе сезона играть остаётся некому.
    const double small = fleetUpkeepPerSecond(/*планет=*/4, /*узлов=*/0);
    const double medium = fleetUpkeepPerSecond(/*планет=*/20, /*узлов=*/0);
    const double huge = fleetUpkeepPerSecond(/*планет=*/40, /*узлов=*/0);

    CHECK(medium > small);
    CHECK(huge > medium);

    // Первые восемь планет бесплатны: империя нормального размера
    // не должна чувствовать тормоз вовсе.
    CHECK(fleetUpkeepPerSecond(4, 0) == doctest::Approx(fleetUpkeepPerSecond(8, 0)));

    // Но и не разорительно: гигант платит вдвое с лишним, а не вдесятеро.
    // Иначе тормоз превращается в потолок, то есть в тот самый запрет,
    // от которого мы уходили.
    CHECK(huge < small * 3.0);
}

TEST_CASE("узел снабжения: платит за право расти дальше") {
    // Ограничитель обязан иметь ответ. Иначе он не задача, а стена.
    const double without = fleetUpkeepPerSecond(/*планет=*/40, /*узлов=*/0);
    const double withOne = fleetUpkeepPerSecond(/*планет=*/39, /*узлов=*/1);
    const double withFour = fleetUpkeepPerSecond(/*планет=*/36, /*узлов=*/4);

    CHECK(withOne < without);
    CHECK(withFour < withOne);

    // Но бесплатным флот не станет никогда.
    const double withMany = fleetUpkeepPerSecond(/*планет=*/30, /*узлов=*/10);
    CHECK(withMany > 0.0);
}

TEST_CASE("хабитат: добавляет слоты, и застройка в них работает") {
    Economy world;
    world.own(1);

    // Планета на четыре слота: хабитат, а дальше — три шахты и ещё две
    // сверх её собственного предела.
    const Entity planet = world.colonise(
        1, PlanetClass::Barren, Specialization::None,
        {Building::Habitat, Building::Mine, Building::Mine, Building::Mine,
         Building::Mine, Building::Mine});
    world.world.get<Planet>(planet)->slots = 4;

    const PlanetDevelopment& development = *world.world.get<PlanetDevelopment>(planet);
    const Planet& data = *world.world.get<Planet>(planet);

    CHECK(usableSlots(data, development) == 4 + kHabitatSlots);

    // Пять шахт работают, а не три: слоты за пределом собственного лимита
    // планеты открыл хабитат.
    world.run(kSecond * 100);
    const double perMine = kOutputMine.toDouble() * 100.0;
    CHECK(world.empire().minerals.toDouble() > perMine * 4.5);
}

TEST_CASE("хабитат: без него лишние слоты не работают") {
    Economy world;
    world.own(1);
    const Entity planet = world.colonise(
        1, PlanetClass::Barren, Specialization::None,
        {Building::Mine, Building::Mine, Building::Mine, Building::Mine,
         Building::Mine, Building::Mine});
    world.world.get<Planet>(planet)->slots = 4;

    world.run(kSecond * 100);
    // Ровно четыре шахты, а не шесть.
    const double perMine = kOutputMine.toDouble() * 100.0;
    CHECK(world.empire().minerals.toDouble() < perMine * 4.5);
}
