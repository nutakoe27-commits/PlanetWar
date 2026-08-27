#include "doctest.h"

#include "pw/sim/combat.h"
#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/production.h"

using namespace pw;
using namespace pw::sim;

namespace {

struct Yard {
    World world;
    Galaxy galaxy;
    Ledger ledger;
    Commands commands;
    Entity empireEntity;

    explicit Yard(uint32_t systems = 30) {
        registerGalaxyComponents(world);
        registerFleetComponents(world);
        registerControlComponents(world);
        registerEconomyComponents(world);
        registerProductionComponents(world);

        GalaxyParams params;
        params.systemCount = systems;
        galaxy.generate(world, params);
        initialiseControl(world, galaxy);
        initialiseProduction(world, galaxy);

        world.setResource(&galaxy);
        world.setResource(&ledger);
        world.setResource(&commands);

        empireEntity = world.create();
        world.add<Empire>(empireEntity, Empire{fx::zero(), fx::zero(), fx::zero(),
                                               fx::zero(), fx::zero(), 0, 0});
        world.get<Owner>(galaxy.systemEntity(1))->empire = 0;
    }

    Empire& empire() { return *world.get<Empire>(empireEntity); }
    BuildQueue& queue(uint32_t system) {
        return *world.get<BuildQueue>(galaxy.systemEntity(system));
    }

    /// Планета с верфями. Владелец обязателен: верфь служит хозяину
    /// ПЛАНЕТЫ, и безхозная не строит никому.
    Entity buildShipyards(uint32_t system, int count, uint32_t who = 0) {
        const Entity planet = world.create();
        world.add<Planet>(planet, Planet{system, uint8_t(PlanetClass::Desert),
                                         kMaxSlots, uint8_t(Specialization::Shipyard), 0});
        world.add<Owner>(planet, Owner{who, 0});
        PlanetDevelopment development{};
        for (int i = 0; i < count && i < kMaxSlots; ++i) {
            development.buildings[i] = uint8_t(Building::Shipyard);
        }
        world.add<PlanetDevelopment>(planet, development);
        world.add<PlanetConstruction>(planet, emptyConstruction());
        return planet;
    }

    /// Пустая планета под стройку.
    Entity colony(uint32_t system, uint32_t who = 0, uint8_t slots = kMaxSlots) {
        const Entity planet = world.create();
        world.add<Planet>(planet, Planet{system, uint8_t(PlanetClass::Desert), slots,
                                         uint8_t(Specialization::None), 0});
        world.add<Owner>(planet, Owner{who, 0});
        world.add<PlanetDevelopment>(planet, PlanetDevelopment{});
        world.add<PlanetConstruction>(planet, emptyConstruction());
        return planet;
    }

    void runConstruction(int64_t ticks) {
        for (int64_t i = 0; i < ticks; ++i) {
            TickContext context;
            context.tick = uint64_t(i);
            planetConstructionTick(world, context);
        }
    }

    void run(int64_t ticks) {
        for (int64_t i = 0; i < ticks; ++i) {
            TickContext context;
            context.tick = uint64_t(i);
            systemProduction(world, context);
            systemDisbandEmpty(world, context);
            systemApplyCommands(world, context);
        }
    }

    uint32_t fleetsOf(uint32_t empire) {
        uint32_t total = 0;
        world.each<Fleet, Owner>([&](Entity, Fleet&, Owner& owner) {
            if (owner.empire == empire) ++total;
        });
        return total;
    }

    uint32_t shipsIn(uint32_t system) {
        uint32_t total = 0;
        world.each<Fleet, FleetLocation>([&](Entity, Fleet& fleet, FleetLocation& location) {
            if (location.system == system) total += fleetTonnage(fleet);
        });
        return total;
    }
};

constexpr int64_t kSecond = kTicksPerSecond;

}  // namespace

// ---------------------------------------------------------------------------
// Буфер команд
// ---------------------------------------------------------------------------

TEST_CASE("команды: применяются в порядке добавления") {
    Yard yard;
    yard.commands.spawnFleet(0, 1, Fleet{1, 0, 0, 0});
    yard.commands.spawnFleet(0, 2, Fleet{0, 1, 0, 0});
    CHECK(yard.commands.size() == 2);

    TickContext context;
    systemApplyCommands(yard.world, context);

    CHECK(yard.commands.empty());
    CHECK(yard.fleetsOf(0) == 2);
    CHECK(yard.shipsIn(1) == 1u);   // корвет
    CHECK(yard.shipsIn(2) == 3u);   // эсминец
}

TEST_CASE("команды: удаление безопасно повторять") {
    Yard yard;
    yard.commands.spawnFleet(0, 1, Fleet{1, 0, 0, 0});
    TickContext context;
    systemApplyCommands(yard.world, context);

    Entity target = kNoEntity;
    yard.world.each<Fleet>([&](Entity e, Fleet&) { target = e; });
    REQUIRE(target.valid());

    yard.commands.destroy(target);
    yard.commands.destroy(target);  // дважды — не должно ломаться
    systemApplyCommands(yard.world, context);
    CHECK(yard.fleetsOf(0) == 0);
}

TEST_CASE("команды: созданный флот сразу пригоден к использованию") {
    Yard yard;
    yard.commands.spawnFleet(0, 5, Fleet{2, 0, 0, 0});
    TickContext context;
    systemApplyCommands(yard.world, context);

    yard.world.each<Fleet, FleetLocation, MoveOrder, Owner>(
        [&](Entity, Fleet& fleet, FleetLocation& location, MoveOrder& order, Owner& owner) {
            CHECK(fleet.corvettes == 2u);
            CHECK(location.system == 5u);
            CHECK(location.nextSystem == 5u);   // стоит, а не летит в никуда
            CHECK(order.target == kNoSystem);    // без приказа
            CHECK(owner.empire == 0u);
        });
}

// ---------------------------------------------------------------------------
// Постройка
// ---------------------------------------------------------------------------

TEST_CASE("постройка: корвет собирается и появляется в системе") {
    Yard yard;
    yard.buildShipyards(1, 2);
    yard.empire().alloys = fx::fromInt(1000);
    enqueueBuild(yard.queue(1), Hull::Corvette, 1);

    // Две верфи осваивают 1 сплав в секунду, корвет стоит 100.
    yard.run(99 * kSecond);
    CHECK(yard.fleetsOf(0) == 0);

    yard.run(3 * kSecond);
    CHECK(yard.fleetsOf(0) == 1);
    CHECK(yard.shipsIn(1) == 1u);
    // Заказ выполнен — очередь очищена.
    CHECK(yard.queue(1).hull == uint8_t(Hull::None));
    CHECK(yard.queue(1).remaining == 0u);
}

TEST_CASE("постройка: без верфи система строить не может") {
    Yard yard;
    yard.empire().alloys = fx::fromInt(5000);
    enqueueBuild(yard.queue(1), Hull::Corvette, 3);

    yard.run(600 * kSecond);
    CHECK(yard.fleetsOf(0) == 0);
    CHECK(yard.empire().alloys == fx::fromInt(5000));  // сплавы не тронуты
}

TEST_CASE("постройка: без сплавов стройка стоит") {
    Yard yard;
    yard.buildShipyards(1, 4);
    yard.empire().alloys = fx::zero();
    enqueueBuild(yard.queue(1), Hull::Corvette, 1);

    yard.run(300 * kSecond);
    CHECK(yard.fleetsOf(0) == 0);
    CHECK(yard.queue(1).remaining == 1u);  // заказ ждёт
}

TEST_CASE("постройка: верфи задают темп, а не объём") {
    auto secondsFor = [](int shipyards) {
        Yard yard;
        yard.buildShipyards(1, shipyards);
        yard.empire().alloys = fx::fromInt(10000);
        enqueueBuild(yard.queue(1), Hull::Corvette, 1);

        for (int64_t second = 1; second <= 1000; ++second) {
            yard.run(kSecond);
            if (yard.fleetsOf(0) > 0) return second;
        }
        return int64_t(-1);
    };

    const int64_t few = secondsFor(1);
    const int64_t many = secondsFor(4);
    CAPTURE(few);
    CAPTURE(many);
    CHECK(few > 0);
    CHECK(many > 0);
    // Вчетверо больше верфей — вчетверо быстрее сборка при том же запасе.
    CHECK(double(few) / double(many) > 3.5);
    CHECK(double(few) / double(many) < 4.5);
}

TEST_CASE("постройка: очередь из нескольких кораблей") {
    Yard yard;
    yard.buildShipyards(1, 10);
    yard.empire().alloys = fx::fromInt(10000);
    enqueueBuild(yard.queue(1), Hull::Corvette, 5);

    yard.run(300 * kSecond);
    CHECK(yard.fleetsOf(0) == 5);
    CHECK(yard.queue(1).remaining == 0u);
    // Пять корветов стоят 500 сплавов.
    CHECK(yard.empire().alloys.floorToInt() >= 9490);
    CHECK(yard.empire().alloys.floorToInt() <= 9510);
}

TEST_CASE("постройка: линкор дороже и дольше корвета") {
    auto secondsFor = [](Hull hull) {
        Yard yard;
        yard.buildShipyards(1, 12);
        yard.empire().alloys = fx::fromInt(100000);
        enqueueBuild(yard.queue(1), hull, 1);
        for (int64_t second = 1; second <= 2000; ++second) {
            yard.run(kSecond);
            if (yard.fleetsOf(0) > 0) return second;
        }
        return int64_t(-1);
    };

    const int64_t corvette = secondsFor(Hull::Corvette);
    const int64_t battleship = secondsFor(Hull::Battleship);
    CHECK(corvette > 0);
    CHECK(battleship > 0);
    // Отношение времён равно отношению стоимостей: 2400 к 100.
    const double ratio = double(battleship) / double(corvette);
    CHECK(ratio > 20.0);
    CHECK(ratio < 28.0);
}

TEST_CASE("постройка: смена типа обнуляет вложенное") {
    Yard yard;
    yard.buildShipyards(1, 2);
    yard.empire().alloys = fx::fromInt(10000);
    enqueueBuild(yard.queue(1), Hull::Battleship, 1);

    yard.run(60 * kSecond);
    CHECK(yard.queue(1).invested > fx::zero());

    // Передумали строить линкор. Недостроенный корпус не превращается
    // в корвет: за перестройку планов на ходу надо платить.
    enqueueBuild(yard.queue(1), Hull::Corvette, 1);
    CHECK(yard.queue(1).invested == fx::zero());
}

TEST_CASE("постройка: ничья система не строит") {
    Yard yard;
    yard.buildShipyards(3, 4);  // система 3 никому не принадлежит
    yard.empire().alloys = fx::fromInt(5000);
    enqueueBuild(yard.queue(3), Hull::Corvette, 2);

    yard.run(300 * kSecond);
    CHECK(yard.fleetsOf(0) == 0);
}

// ---------------------------------------------------------------------------
// Роспуск
// ---------------------------------------------------------------------------

TEST_CASE("роспуск: пустой флот исчезает") {
    Yard yard;
    yard.commands.spawnFleet(0, 1, Fleet{0, 0, 0, 0});
    yard.commands.spawnFleet(0, 1, Fleet{3, 0, 0, 0});
    TickContext context;
    systemApplyCommands(yard.world, context);
    CHECK(yard.fleetsOf(0) == 2);

    // Пустой флот занимал бы место, обходился каждый тик и, что хуже,
    // считался бы присутствием в системе.
    yard.run(1);
    CHECK(yard.fleetsOf(0) == 1);
    CHECK(yard.shipsIn(1) == 3u);
}

// ---------------------------------------------------------------------------
// Полный цикл и детерминизм
// ---------------------------------------------------------------------------

TEST_CASE("цикл: экономика кормит верфь, верфь строит флот") {
    Yard yard;
    // Шахты, заводы и верфи — вся цепочка от руды до корабля.
    const Entity mining = yard.world.create();
    yard.world.add<Planet>(mining, Planet{1, uint8_t(PlanetClass::AsteroidBelt),
                                          kMaxSlots, uint8_t(Specialization::Mining), 0});
    yard.world.add<Owner>(mining, Owner{0, 0});
    PlanetDevelopment mines{};
    for (int i = 0; i < 6; ++i) mines.buildings[i] = uint8_t(Building::Mine);
    yard.world.add<PlanetDevelopment>(mining, mines);

    const Entity industry = yard.world.create();
    yard.world.add<Planet>(industry, Planet{1, uint8_t(PlanetClass::Desert), kMaxSlots,
                                            uint8_t(Specialization::Industrial), 0});
    yard.world.add<Owner>(industry, Owner{0, 0});
    PlanetDevelopment works{};
    for (int i = 0; i < 4; ++i) works.buildings[i] = uint8_t(Building::Foundry);
    for (int i = 4; i < 8; ++i) works.buildings[i] = uint8_t(Building::Shipyard);
    yard.world.add<PlanetDevelopment>(industry, works);

    enqueueBuild(yard.queue(1), Hull::Corvette, 4);

    // Ни единого сплава на старте: всё, что построится, добыто и выплавлено.
    CHECK(yard.empire().alloys == fx::zero());

    for (int64_t i = 0; i < 900 * kSecond; ++i) {
        TickContext context;
        context.tick = uint64_t(i);
        systemEconomy(yard.world, context);
        systemProduction(yard.world, context);
        systemDisbandEmpty(yard.world, context);
        systemApplyCommands(yard.world, context);
    }

    CHECK(yard.fleetsOf(0) == 4);
    CHECK(yard.shipsIn(1) == 4u);
}

TEST_CASE("постройка: воспроизводится тик в тик") {
    Yard first, second;
    for (Yard* yard : {&first, &second}) {
        yard->buildShipyards(1, 6);
        yard->empire().alloys = fx::fromInt(20000);
        enqueueBuild(yard->queue(1), Hull::Destroyer, 8);
    }
    REQUIRE(first.world.hash() == second.world.hash());

    first.run(2000 * kSecond);
    second.run(2000 * kSecond);

    CHECK(first.world.hash() == second.world.hash());
    CHECK(first.fleetsOf(0) == 8);
}

// ---------------------------------------------------------------------------
// Слияние флотов
// ---------------------------------------------------------------------------

namespace {

FleetArmament withArmament(uint8_t kinetic, uint8_t energy) {
    FleetArmament armament = balancedArmament();
    armament.kinetic = kinetic;
    armament.energy = energy;
    return armament;
}

Entity stationed(Yard& yard, uint32_t system, uint32_t empire, const Fleet& composition,
                 const FleetArmament& armament) {
    const Entity e = yard.world.create();
    yard.world.add<Fleet>(e, composition);
    yard.world.add<FleetLocation>(e, FleetLocation{system, system, fx::zero()});
    yard.world.add<MoveOrder>(e, MoveOrder{kNoSystem, 0});
    yard.world.add<Owner>(e, Owner{empire, 0});
    yard.world.add<FleetArmament>(e, armament);
    return e;
}

void mergeTick(Yard& yard) {
    TickContext context;
    systemMergeFleets(yard.world, context);
    systemApplyCommands(yard.world, context);
}

}  // namespace

TEST_CASE("слияние: свои флоты в одной системе объединяются") {
    // Без слияния каждый построенный корабль остаётся отдельным отрядом:
    // прогон сезона за шесть часов набрал 98 отрядов по одному кораблю.
    Yard yard;
    yard.world.registerComponent<FleetArmament>("FleetArmament");
    const FleetArmament same = withArmament(50, 50);

    stationed(yard, 1, 0, Fleet{5, 0, 0, 0}, same);
    stationed(yard, 1, 0, Fleet{3, 1, 0, 0}, same);
    stationed(yard, 1, 0, Fleet{0, 0, 1, 0}, same);
    CHECK(yard.fleetsOf(0) == 3);

    mergeTick(yard);
    CHECK(yard.fleetsOf(0) == 1);
    // Корабли не потерялись и не удвоились.
    CHECK(yard.shipsIn(1) == 5u + 3u + 3u + 8u);
}

TEST_CASE("слияние: разное вооружение не смешивается") {
    Yard yard;
    yard.world.registerComponent<FleetArmament>("FleetArmament");

    stationed(yard, 1, 0, Fleet{5, 0, 0, 0}, withArmament(100, 0));
    stationed(yard, 1, 0, Fleet{5, 0, 0, 0}, withArmament(0, 100));

    mergeTick(yard);
    // Смешав их, мы потеряли бы выбор игрока, а вместе с ним контр-систему.
    CHECK(yard.fleetsOf(0) == 2);
}

TEST_CASE("слияние: чужие флоты не сливаются") {
    Yard yard;
    yard.world.registerComponent<FleetArmament>("FleetArmament");
    const FleetArmament same = withArmament(50, 50);

    stationed(yard, 1, 0, Fleet{5, 0, 0, 0}, same);
    stationed(yard, 1, 7, Fleet{5, 0, 0, 0}, same);

    mergeTick(yard);
    CHECK(yard.fleetsOf(0) == 1);
    CHECK(yard.fleetsOf(7) == 1);
}

TEST_CASE("слияние: флот с приказом не трогают") {
    Yard yard;
    yard.world.registerComponent<FleetArmament>("FleetArmament");
    const FleetArmament same = withArmament(50, 50);

    stationed(yard, 1, 0, Fleet{5, 0, 0, 0}, same);
    const Entity ordered = stationed(yard, 1, 0, Fleet{5, 0, 0, 0}, same);
    yard.world.get<MoveOrder>(ordered)->target = 9;

    mergeTick(yard);
    // Идущий к цели флот — отдельное намерение игрока, склеивать его нельзя.
    CHECK(yard.fleetsOf(0) == 2);
}

TEST_CASE("слияние: флоты в разных системах остаются раздельными") {
    Yard yard;
    yard.world.registerComponent<FleetArmament>("FleetArmament");
    const FleetArmament same = withArmament(50, 50);

    stationed(yard, 1, 0, Fleet{5, 0, 0, 0}, same);
    stationed(yard, 2, 0, Fleet{5, 0, 0, 0}, same);

    mergeTick(yard);
    CHECK(yard.fleetsOf(0) == 2);
}

TEST_CASE("производство: корабль наследует вооружение империи") {
    Yard yard;
    yard.world.registerComponent<FleetArmament>("FleetArmament");
    // Выбор билда, сделанный игроком, обязан доезжать до верфи: иначе
    // построенные корабли воюют не тем, чем империя собиралась воевать.
    yard.world.add<FleetArmament>(yard.empireEntity, withArmament(90, 10));

    yard.buildShipyards(1, 8);
    yard.empire().alloys = fx::fromInt(5000);
    enqueueBuild(yard.queue(1), Hull::Corvette, 1);
    yard.run(60 * kSecond);

    REQUIRE(yard.fleetsOf(0) == 1);
    bool checked = false;
    yard.world.each<Fleet, FleetArmament>([&](Entity, Fleet&, FleetArmament& armament) {
        CHECK(armament.kinetic == 90);
        CHECK(armament.energy == 10);
        checked = true;
    });
    CHECK(checked);
}

// ---------------------------------------------------------------------------
// Стройка на планете
//
// Главное свойство: здание не появляется по щелчку. Оно строится минуты,
// и всё это время видно, что именно строится и насколько готово.
// ---------------------------------------------------------------------------

TEST_CASE("стройка: здание не появляется мгновенно") {
    Yard yard;
    const Entity planet = yard.colony(1);
    yard.empire().minerals = fx::fromInt(1000);

    enqueueConstruction(*yard.world.get<PlanetConstruction>(planet), 0, Building::Mine);

    yard.runConstruction(1 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
          uint8_t(Building::None));
    CHECK(yard.world.get<PlanetConstruction>(planet)->slot == 0);

    // Шахта стоит 60 минералов при скорости 0.5 в секунду — две минуты.
    yard.runConstruction(100 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
          uint8_t(Building::None));

    yard.runConstruction(25 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
          uint8_t(Building::Mine));
    // Стройка закончилась — слот свободен под следующую.
    CHECK(yard.world.get<PlanetConstruction>(planet)->slot ==
          PlanetConstruction::kNoSlot);
}

TEST_CASE("стройка: время равно цене, делённой на темп") {
    // Цена и срок — одно число, а не два. Иначе они разошлись бы, и
    // появилось бы дешёвое здание, которое строится дольше дорогого.
    auto secondsToBuild = [](Building building) {
        Yard yard;
        const Entity planet = yard.colony(1);
        yard.empire().minerals = fx::fromInt(10000);
        enqueueConstruction(*yard.world.get<PlanetConstruction>(planet), 0, building);

        for (int64_t second = 1; second <= 2000; ++second) {
            yard.runConstruction(kSecond);
            if (yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
                uint8_t(building)) {
                return second;
            }
        }
        return int64_t(-1);
    };

    const int64_t mine = secondsToBuild(Building::Mine);
    const int64_t yardTime = secondsToBuild(Building::Shipyard);
    CAPTURE(mine);
    CAPTURE(yardTime);

    // Две минуты на шахту, около семи на верфь: масштаб, при котором
    // осаждённый не успевает достроить крепость под падающей обороной.
    CHECK(mine >= 118);
    CHECK(mine <= 122);
    CHECK(yardTime > mine * 3);
    CHECK(yardTime < 500);
}

TEST_CASE("стройка: без минералов ЖДЁТ, а не отменяется") {
    // Заказ не отменяется молча: игрок намерение выразил, и съедать его
    // за него — это ровно то поведение, которое человек читает как
    // «игра проглотила приказ».
    Yard yard;
    const Entity planet = yard.colony(1);
    yard.empire().minerals = fx::zero();
    enqueueConstruction(*yard.world.get<PlanetConstruction>(planet), 0, Building::Mine);

    yard.runConstruction(300 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
          uint8_t(Building::None));
    CHECK(yard.world.get<PlanetConstruction>(planet)->slot == 0);
    CHECK(yard.world.get<PlanetConstruction>(planet)->paid == 0);

    // Появились минералы — стройка началась.
    yard.empire().minerals = fx::fromInt(500);
    yard.runConstruction(130 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
          uint8_t(Building::Mine));
}

TEST_CASE("стройка: цена списывается целиком и сразу") {
    // Размазанная по времени оплата давала тупик: империя с десятью
    // начатыми стройками делила скудный доход на десять, ни одна
    // не доходила до конца, шахты не появлялись — и доход не рос никогда.
    // Прогон сезона показал империю, которая за два часа не построила
    // ни одного здания.
    Yard yard;
    const Entity planet = yard.colony(1);
    yard.empire().minerals = fx::fromInt(200);
    enqueueConstruction(*yard.world.get<PlanetConstruction>(planet), 0, Building::Mine);

    yard.runConstruction(1);
    CHECK(yard.empire().minerals == fx::fromInt(140));
    CHECK(yard.world.get<PlanetConstruction>(planet)->paid == 1);

    // Дальше цена больше не списывается: она уже уплачена.
    yard.runConstruction(60 * kSecond);
    CHECK(yard.empire().minerals == fx::fromInt(140));
}

TEST_CASE("стройка: скудный доход не делится между стройками") {
    // Десять начатых строек и минералов ровно на одну: одна обязана
    // достроиться, остальные — ждать. Делёж поровну не достраивает
    // ни одной, и это тупик, а не медленное развитие.
    Yard yard;
    yard.empire().minerals = fx::fromInt(60);

    std::vector<Entity> planets;
    for (int i = 0; i < 10; ++i) {
        planets.push_back(yard.colony(1));
        enqueueConstruction(*yard.world.get<PlanetConstruction>(planets.back()), 0,
                            Building::Mine);
    }

    yard.runConstruction(200 * kSecond);

    int built = 0;
    for (const Entity planet : planets) {
        if (yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
            uint8_t(Building::Mine)) {
            ++built;
        }
    }
    CHECK(built == 1);
}

TEST_CASE("стройка: тратит минералы империи") {
    Yard yard;
    const Entity planet = yard.colony(1);
    yard.empire().minerals = fx::fromInt(200);
    enqueueConstruction(*yard.world.get<PlanetConstruction>(planet), 0, Building::Mine);

    yard.runConstruction(130 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
          uint8_t(Building::Mine));
    // Ровно цена шахты, ни минералом больше.
    CHECK(yard.empire().minerals == fx::fromInt(140));
}

TEST_CASE("стройка: второй заказ встаёт в очередь, а не отменяет первый") {
    // Без очереди второй щелчок отменял бы первый — поведение, которое
    // человек читает как «игра съела мой приказ».
    Yard yard;
    const Entity planet = yard.colony(1);
    yard.empire().minerals = fx::fromInt(1000);
    PlanetConstruction& site = *yard.world.get<PlanetConstruction>(planet);

    CHECK(enqueueConstruction(site, 0, Building::Mine));
    CHECK(enqueueConstruction(site, 1, Building::Mine));
    CHECK(enqueueConstruction(site, 2, Building::Foundry));
    CHECK(site.slot == 0);
    CHECK(site.queued == 2);

    yard.runConstruction(130 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
          uint8_t(Building::Mine));
    // Очередь сдвинулась сама: игроку не надо возвращаться и тыкать снова.
    CHECK(site.slot == 1);
    CHECK(site.queued == 1);

    yard.runConstruction(130 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[1] ==
          uint8_t(Building::Mine));

    yard.runConstruction(190 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[2] ==
          uint8_t(Building::Foundry));
    CHECK(site.slot == PlanetConstruction::kNoSlot);
    CHECK(site.queued == 0);
}

TEST_CASE("стройка: отмена бросает начатое и подтягивает очередь") {
    Yard yard;
    const Entity planet = yard.colony(1);
    yard.empire().minerals = fx::fromInt(1000);
    PlanetConstruction& site = *yard.world.get<PlanetConstruction>(planet);

    enqueueConstruction(site, 0, Building::Shipyard);
    enqueueConstruction(site, 1, Building::Mine);
    yard.runConstruction(60 * kSecond);
    CHECK(site.elapsed > 0);
    CHECK(site.paid == 1);
    const fx afterShipyard = yard.empire().minerals;

    // Передумал — за перестройку планов на ходу надо платить: уплаченное
    // за верфь не возвращается.
    enqueueConstruction(site, 0, Building::None);
    CHECK(site.elapsed == 0);
    CHECK(site.paid == 0);
    CHECK(yard.empire().minerals == afterShipyard);
    CHECK(site.slot == 1);
    CHECK(site.building == uint8_t(Building::Mine));
    CHECK(site.queued == 0);
}

TEST_CASE("стройка: переполненная очередь отвечает отказом") {
    // Отказ обязан быть слышным: молчаливый человек читает как «не нажалось».
    Yard yard;
    const Entity planet = yard.colony(1);
    PlanetConstruction& site = *yard.world.get<PlanetConstruction>(planet);

    for (uint8_t i = 0; i <= PlanetConstruction::kQueueLimit; ++i) {
        CHECK(enqueueConstruction(site, i, Building::Mine));
    }
    CHECK_FALSE(enqueueConstruction(site, 11, Building::Mine));
}

TEST_CASE("стройка: процент готовности растёт от нуля до ста") {
    Yard yard;
    const Entity planet = yard.colony(1);
    yard.empire().minerals = fx::fromInt(1000);
    PlanetConstruction& site = *yard.world.get<PlanetConstruction>(planet);
    CHECK(constructionPercent(site) == 0);

    enqueueConstruction(site, 0, Building::Mine);
    CHECK(constructionPercent(site) == 0);

    yard.runConstruction(60 * kSecond);
    const uint32_t half = constructionPercent(site);
    CHECK(half >= 45);
    CHECK(half <= 55);

    yard.runConstruction(30 * kSecond);
    CHECK(constructionPercent(site) > half);
    CHECK(constructionPercent(site) <= 100);
}

TEST_CASE("стройка: ничья планета не строит") {
    Yard yard;
    const Entity planet = yard.colony(1, kNoEmpire);
    yard.empire().minerals = fx::fromInt(1000);
    enqueueConstruction(*yard.world.get<PlanetConstruction>(planet), 0, Building::Mine);

    yard.runConstruction(300 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(planet)->buildings[0] ==
          uint8_t(Building::None));
    // И казна не тронута.
    CHECK(yard.empire().minerals == fx::fromInt(1000));
}

TEST_CASE("стройка: слот за пределами планеты отвергается") {
    Yard yard;
    const Entity planet = yard.colony(1, /*who=*/0, /*slots=*/3);
    yard.empire().minerals = fx::fromInt(1000);
    enqueueConstruction(*yard.world.get<PlanetConstruction>(planet), 5, Building::Mine);

    yard.runConstruction(1 * kSecond);
    CHECK(yard.world.get<PlanetConstruction>(planet)->slot ==
          PlanetConstruction::kNoSlot);
    CHECK(yard.empire().minerals == fx::fromInt(1000));
}

TEST_CASE("стройка: две планеты строят параллельно и независимо") {
    // «Одна стройка на планету» — это не «одна стройка на империю».
    // Хочешь строить быстрее — строй на разных планетах.
    Yard yard;
    const Entity first = yard.colony(1);
    const Entity second = yard.colony(1);
    yard.empire().minerals = fx::fromInt(1000);

    enqueueConstruction(*yard.world.get<PlanetConstruction>(first), 0, Building::Mine);
    enqueueConstruction(*yard.world.get<PlanetConstruction>(second), 0, Building::Mine);

    yard.runConstruction(130 * kSecond);
    CHECK(yard.world.get<PlanetDevelopment>(first)->buildings[0] ==
          uint8_t(Building::Mine));
    CHECK(yard.world.get<PlanetDevelopment>(second)->buildings[0] ==
          uint8_t(Building::Mine));
    // Заплачено за обе.
    CHECK(yard.empire().minerals == fx::fromInt(1000 - 120));
}

TEST_CASE("стройка: воспроизводится тик в тик") {
    Yard first, second;
    for (Yard* yard : {&first, &second}) {
        yard->empire().minerals = fx::fromInt(400);
        const Entity a = yard->colony(1);
        const Entity b = yard->colony(1);
        enqueueConstruction(*yard->world.get<PlanetConstruction>(a), 0, Building::Foundry);
        enqueueConstruction(*yard->world.get<PlanetConstruction>(b), 2, Building::Fortress);
    }
    REQUIRE(first.world.hash() == second.world.hash());

    first.runConstruction(500 * kSecond);
    second.runConstruction(500 * kSecond);
    CHECK(first.world.hash() == second.world.hash());
}
