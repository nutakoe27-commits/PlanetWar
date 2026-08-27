#include "doctest.h"

#include "pw/sim/battle_system.h"
#include "pw/sim/control.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

using namespace pw;
using namespace pw::sim;

namespace {

struct War {
    World world;
    Galaxy galaxy;

    explicit War(uint64_t seed = 0xBA771E, uint32_t systems = 40) {
        registerGalaxyComponents(world);
        registerFleetComponents(world);
        registerControlComponents(world);
        registerBattleComponents(world);

        GalaxyParams params;
        params.seed = seed;
        params.systemCount = systems;
        galaxy.generate(world, params);
        initialiseControl(world, galaxy);
        initialiseBattles(world, galaxy);
        world.setResource(&galaxy);
    }

    Entity station(uint32_t system, uint32_t empire, const Fleet& composition,
                   const FleetArmament& armament) {
        const Entity e = world.create();
        world.add<Fleet>(e, composition);
        world.add<FleetLocation>(e, standingAt(system));
        world.add<MoveOrder>(e, MoveOrder{kNoSystem, 0});
        world.add<Owner>(e, Owner{empire, 0});
        world.add<FleetArmament>(e, armament);
        return e;
    }

    void own(uint32_t system, uint32_t empire) {
        world.get<Owner>(galaxy.systemEntity(system))->empire = empire;
    }

    void run(int64_t ticks, uint64_t startTick = 0) {
        for (int64_t i = 0; i < ticks; ++i) {
            TickContext context;
            context.tick = startTick + uint64_t(i);
            systemBattles(world, context);
        }
    }

    Fleet fleetOf(Entity e) { return *world.get<Fleet>(e); }
    uint32_t tonnageOf(Entity e) { return fleetTonnage(*world.get<Fleet>(e)); }
};

FleetArmament armed(uint8_t k, uint8_t e, uint8_t m, uint8_t sh, uint8_t ar, uint8_t pd) {
    FleetArmament a{};
    a.kinetic = k; a.energy = e; a.missile = m;
    a.shields = sh; a.armour = ar; a.pointDefense = pd;
    a.doctrine = uint8_t(Doctrine::Line);
    return a;
}

constexpr int64_t kSecond = kTicksPerSecond;
constexpr int64_t kMinute = 60 * kSecond;

}  // namespace

TEST_CASE("сведение: один флот в системе не воюет сам с собой") {
    War war;
    const Entity lone = war.station(5, 1, makeFleet({{Hull::Corvette, 20}}), armed(50, 50, 0, 50, 50, 0));

    war.run(5 * kMinute);
    CHECK(war.tonnageOf(lone) == 20u);
}

TEST_CASE("сведение: союзники в одной системе не дерутся") {
    War war;
    const Entity a = war.station(5, 1, makeFleet({{Hull::Corvette, 20}}), armed(50, 50, 0, 50, 50, 0));
    const Entity b = war.station(5, 1, makeFleet({{Hull::Corvette, 15}}), armed(50, 50, 0, 50, 50, 0));

    war.run(5 * kMinute);
    CHECK(war.tonnageOf(a) == 20u);
    CHECK(war.tonnageOf(b) == 15u);
}

TEST_CASE("сведение: враги в одной системе несут потери") {
    War war;
    const Entity blue = war.station(5, 1, makeFleet({{Hull::Corvette, 40}}), armed(100, 0, 0, 50, 50, 0));
    const Entity red = war.station(5, 2, makeFleet({{Hull::Corvette, 40}}), armed(100, 0, 0, 50, 50, 0));

    war.run(1);
    CHECK(war.tonnageOf(blue) < 40u);
    CHECK(war.tonnageOf(red) < 40u);
}

TEST_CASE("сведение: летящий флот в сражении не участвует") {
    War war;
    const Entity defender = war.station(5, 1, makeFleet({{Hull::Corvette, 40}}), armed(50, 50, 0, 50, 50, 0));
    const Entity passing = war.station(5, 2, makeFleet({{Hull::Corvette, 40}}), armed(50, 50, 0, 50, 50, 0));
    // Второй уже вышел из системы: он между узлами.
    war.world.get<FleetLocation>(passing)->nextSystem = war.galaxy.neighbors(5)[0];

    war.run(5 * kMinute);
    CHECK(war.tonnageOf(defender) == 40u);
    CHECK(war.tonnageOf(passing) == 40u);
}

TEST_CASE("сведение: сражение идёт раз в минуту, а не каждый тик") {
    War war;
    const Entity blue = war.station(5, 1, makeFleet({{Hull::Destroyer, 30}}), armed(50, 50, 0, 50, 50, 50));
    war.station(5, 2, makeFleet({{Hull::Destroyer, 30}}), armed(50, 50, 0, 50, 50, 50));

    war.run(1);
    const uint32_t afterFirst = war.tonnageOf(blue);
    CHECK(afterFirst < 30u * 20u);

    // Полминуты спустя — ничего нового: откат ещё не истёк. Иначе бой шёл бы
    // десять раз в секунду, и подкрепление никогда не успевало бы вмешаться.
    war.run(30 * kSecond, 1);
    CHECK(war.tonnageOf(blue) == afterFirst);

    war.run(35 * kSecond, 1 + 30 * kSecond);
    CHECK(war.tonnageOf(blue) < afterFirst);
}

TEST_CASE("сведение: потери разносятся по флотам стороны") {
    War war;
    const Entity big = war.station(5, 1, makeFleet({{Hull::Corvette, 60}}), armed(50, 50, 0, 50, 50, 0));
    const Entity small = war.station(5, 1, makeFleet({{Hull::Corvette, 20}}), armed(50, 50, 0, 50, 50, 0));
    war.station(5, 2, makeFleet({{Hull::Corvette, 80}}), armed(50, 50, 0, 50, 50, 0));

    war.run(1);
    const uint32_t lostBig = 60u - war.tonnageOf(big);
    const uint32_t lostSmall = 20u - war.tonnageOf(small);

    CHECK(lostBig > 0u);
    CHECK(lostSmall > 0u);
    // Крупный флот несёт примерно втрое больше потерь — по своему вкладу.
    const double ratio = double(lostBig) / double(lostSmall);
    CHECK(ratio > 2.2);
    CHECK(ratio < 4.0);
}

TEST_CASE("сведение: разбитый отходит в соседнюю систему") {
    War war;
    war.own(5, 1);
    // Империя 1 защищается щитами от кинетики, империя 2 идёт с кинетикой
    // и бронёй — заведомо проигрышный выбор против щитов.
    war.station(5, 1, makeFleet({{Hull::Destroyer, 40}}), armed(0, 100, 0, 100, 0, 60));
    const Entity doomed = war.station(5, 2, makeFleet({{Hull::Destroyer, 25}}), armed(100, 0, 0, 0, 100, 0));

    war.run(1);
    const MoveOrder* order = war.world.get<MoveOrder>(doomed);
    REQUIRE(order != nullptr);

    if (!fleetEmpty(war.fleetOf(doomed))) {
        // Уцелевшие получают приказ уходить, а не стоять под добивание.
        CHECK(order->target != kNoSystem);
        bool adjacent = false;
        for (uint32_t k = 0; k < war.galaxy.neighborCount(5); ++k) {
            if (war.galaxy.neighbors(5)[k] == order->target) adjacent = true;
        }
        CHECK(adjacent);
    }
}

TEST_CASE("сведение: победитель остаётся в системе") {
    War war;
    war.own(5, 1);
    const Entity winner = war.station(5, 1, makeFleet({{Hull::Destroyer, 40}}), armed(0, 100, 0, 100, 0, 60));
    war.station(5, 2, makeFleet({{Hull::Destroyer, 20}}), armed(100, 0, 0, 0, 100, 0));

    war.run(1);
    // Система за победителем: приказа уходить он не получает.
    CHECK(war.world.get<MoveOrder>(winner)->target == kNoSystem);
}

TEST_CASE("сведение: владелец системы сражается всегда") {
    War war;
    war.own(5, 3);
    // Слабый хозяин и два сильных чужака. Хозяин обязан быть одной
    // из сторон: это его дом, отсидеться не выйдет.
    const Entity host = war.station(5, 3, makeFleet({{Hull::Corvette, 10}}), armed(50, 50, 0, 50, 50, 0));
    war.station(5, 1, makeFleet({{Hull::Corvette, 60}}), armed(50, 50, 0, 50, 50, 0));
    war.station(5, 2, makeFleet({{Hull::Corvette, 50}}), armed(50, 50, 0, 50, 50, 0));

    war.run(1);
    CHECK(war.tonnageOf(host) < 10u);
}

TEST_CASE("сведение: воспроизводится тик в тик") {
    War first(0x5EED, 40), second(0x5EED, 40);

    for (War* war : {&first, &second}) {
        war->own(5, 1);
        war->station(5, 1, makeFleet({{Hull::Corvette, 30}, {Hull::Tender, 5}, {Hull::Colonizer, 2}, {Hull::Destroyer, 1}}), armed(60, 20, 20, 70, 30, 20));
        war->station(5, 1, makeFleet({{Hull::Corvette, 10}, {Hull::Tender, 2}}), armed(40, 40, 20, 50, 50, 40));
        war->station(5, 2, makeFleet({{Hull::Corvette, 25}, {Hull::Tender, 8}, {Hull::Colonizer, 3}, {Hull::Destroyer, 1}}), armed(20, 50, 30, 30, 70, 50));
        war->station(9, 2, makeFleet({{Hull::Corvette, 15}}), armed(0, 0, 100, 50, 50, 0));
        war->own(9, 1);
        war->station(9, 1, makeFleet({{Hull::Corvette, 18}}), armed(50, 0, 50, 60, 40, 60));
    }
    REQUIRE(first.world.hash() == second.world.hash());

    first.run(10 * kMinute);
    second.run(10 * kMinute);

    CHECK(first.world.hash() == second.world.hash());
}

TEST_CASE("сведение: без карты в ресурсах ничего не ломается") {
    World world;
    registerGalaxyComponents(world);
    registerFleetComponents(world);
    registerControlComponents(world);
    registerBattleComponents(world);

    TickContext context;
    systemBattles(world, context);  // не должно упасть
    CHECK(true);
}
