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

    void buildShipyards(uint32_t system, int count) {
        const Entity planet = world.create();
        world.add<Planet>(planet, Planet{system, uint8_t(PlanetClass::Desert),
                                         kMaxSlots, uint8_t(Specialization::Shipyard), 0});
        PlanetDevelopment development{};
        for (int i = 0; i < count && i < kMaxSlots; ++i) {
            development.buildings[i] = uint8_t(Building::Shipyard);
        }
        world.add<PlanetDevelopment>(planet, development);
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
    PlanetDevelopment mines{};
    for (int i = 0; i < 6; ++i) mines.buildings[i] = uint8_t(Building::Mine);
    yard.world.add<PlanetDevelopment>(mining, mines);

    const Entity industry = yard.world.create();
    yard.world.add<Planet>(industry, Planet{1, uint8_t(PlanetClass::Desert), kMaxSlots,
                                            uint8_t(Specialization::Industrial), 0});
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
