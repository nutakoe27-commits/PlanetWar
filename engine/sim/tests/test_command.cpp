// Управление отрядом: маршрут, стойка, приписка, разделение, слияние.
//
// ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ. Каждое правило из docs/08-FLEET-COMMAND.md,
// и почти каждое из них — про то, чтобы приказ ПЕРЕЖИЛ НОЧЬ. Игрок отдаёт
// его один раз и уходит; всё, что происходит дальше, происходит без него,
// и увидеть ошибку он сможет только по итогу — когда отряда уже нет.
//
// Поэтому проверки здесь не про «функция вернула правильное число»,
// а про поведение: дошёл ли отряд по всем точкам, вернулся ли домой,
// ушёл ли от превосходящего, не слился ли обратно после разделения.
#include "doctest.h"

#include "game_time.h"

#include <algorithm>
#include <string>
#include <vector>

#include "pw/sim/battle_system.h"
#include "pw/sim/combat.h"
#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

using namespace pw;
using namespace pw::sim;
using namespace pw::test;

namespace {

/// Мир с галактикой, флотами и присутствием. Ровно то, что нужно
/// движению, стойке и слиянию, и ничего сверх.
struct Squadron {
    World world;
    Galaxy galaxy;
    Presence presence;
    Commands commands;

    explicit Squadron(uint64_t seed = 0xF1EE7, uint32_t systems = 60) {
        registerGalaxyComponents(world);
        registerFleetComponents(world);
        registerControlComponents(world);
        // Вооружение живёт в компонентах боя. Без него слияние резервов
        // не найдёт ключа и просто ничего не сольёт — то есть проверка
        // слияния тихо перестанет что-либо проверять.
        registerBattleComponents(world);

        GalaxyParams params;
        params.seed = seed;
        params.systemCount = systems;
        galaxy.generate(world, params);
        initialiseControl(world, galaxy);

        world.setResource(&galaxy);
        world.setResource(&presence);
        world.setResource(&commands);
    }

    /// Отряд в системе. По умолчанию — как выделенный игроком: стоит
    /// и ни с кем не сливается.
    Entity fleet(uint32_t system, uint32_t empire, const Fleet& composition,
                 Stance stance = Stance::Hold) {
        const Entity e = world.create();
        world.add<Fleet>(e, composition);
        world.add<FleetLocation>(e, standingAt(system));
        world.add<FleetOrders>(e, idleOrders(system, kNoOrbit, /*tag=*/0, stance));
        world.add<Owner>(e, Owner{empire, 0});
        world.add<FleetArmament>(e, balancedArmament());
        return e;
    }

    FleetOrders& orders(Entity e) { return *world.get<FleetOrders>(e); }
    FleetLocation& where(Entity e) { return *world.get<FleetLocation>(e); }

    /// Прогнать столько ИГРОВЫХ СЕКУНД полного цикла флота.
    void run(int64_t seconds) {
        const int64_t ticks = testTicks(seconds);
        for (int64_t i = 0; i < ticks; ++i) {
            const TickContext context = testTick(i);
            systemFleetMovement(world, context);
            systemFleetStation(world, context);
            systemPresence(world, context);
            systemFleetStance(world, context);
            systemMergeFleets(world, context);
            systemApplyCommands(world, context);
        }
    }

    /// Соседняя система, отличная от заданной.
    uint32_t neighbourOf(uint32_t system, uint32_t skip = kNoSystem) const {
        for (uint32_t k = 0; k < galaxy.neighborCount(system); ++k) {
            const uint32_t candidate = galaxy.neighbors(system)[k];
            if (candidate != skip) return candidate;
        }
        return kNoSystem;
    }

    /// Прогонять мелкими шагами, пока условие не выполнится.
    ///
    /// ШАГ МЕЛКИЙ НАМЕРЕННО. Линия занимает у корвета минут восемь,
    /// и почасовая выборка проскакивает всё: отряд успевает сходить
    /// туда и вернуться между двумя замерами. Первая версия этих проверок
    /// падала именно так — на верном поведении, потому что смотрела
    /// слишком редко.
    template <typename Fn>
    bool runUntil(Fn&& done, int64_t step, int64_t limit) {
        for (int64_t spent = 0; spent < limit; spent += step) {
            if (done()) return true;
            run(step);
        }
        return done();
    }

    uint32_t fleetsOf(uint32_t empire) {
        uint32_t total = 0;
        world.each<Fleet, Owner>([&](Entity, Fleet& fleet, Owner& owner) {
            if (owner.empire == empire && !fleetEmpty(fleet)) ++total;
        });
        return total;
    }
};

/// Система, у которой есть хотя бы два соседа. Нужна маршрутам: путь
/// из двух точек в тупике проверяет не маршрут, а тупик.
uint32_t forkSystem(const Squadron& squad) {
    for (uint32_t index = 0; index < squad.galaxy.systemCount(); ++index) {
        if (squad.galaxy.neighborCount(index) >= 2) return index;
    }
    return kNoSystem;
}

}  // namespace

// ---------------------------------------------------------------------------
// Маршрут
// ---------------------------------------------------------------------------

TEST_CASE("маршрут: пустой означает «приказа нет»") {
    FleetOrders orders = idleOrders(3);
    CHECK_FALSE(orders.routed());
    CHECK(orders.target() == kNoSystem);
    CHECK(orders.anchor == 3u);
    CHECK(orders.stance == uint8_t(Stance::Reserve));
}

TEST_CASE("маршрут: точки набираются и не переполняются") {
    FleetOrders orders = idleOrders(0);
    for (uint8_t i = 0; i < FleetOrders::kMaxRoute; ++i) {
        CAPTURE(int(i));
        CHECK(appendRoute(orders, uint32_t(i + 1)));
    }
    CHECK(orders.count == FleetOrders::kMaxRoute);

    // Девятая не влезает, и об этом говорят вслух: молчаливый отказ игрок
    // читает как «не нажалось» и жмёт ещё раз.
    CHECK_FALSE(appendRoute(orders, 99));
    CHECK(orders.count == FleetOrders::kMaxRoute);
}

TEST_CASE("маршрут: два щелчка по одной системе — одна точка") {
    // Дрожащая рука не должна превращаться в две точки маршрута: отряд
    // пришёл бы в систему и пошёл в неё же ещё раз.
    FleetOrders orders = idleOrders(0);
    CHECK(appendRoute(orders, 7));
    CHECK(appendRoute(orders, 7));
    CHECK(orders.count == 1);
}

TEST_CASE("маршрут: замена стирает прежний целиком") {
    FleetOrders orders = idleOrders(0);
    appendRoute(orders, 4);
    appendRoute(orders, 5);
    orders.step = 1;

    setRoute(orders, 9);
    CHECK(orders.count == 1);
    CHECK(orders.step == 0);
    CHECK(orders.target() == 9u);
}

TEST_CASE("маршрут: отряд обходит все точки по порядку") {
    // ГЛАВНАЯ ПРОВЕРКА МАРШРУТА. Именно она отличает «список целей»
    // от «одной цели, которую переписали»: отряд обязан побывать
    // в каждой точке и в том порядке, в котором их назвал игрок.
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);
    const uint32_t first = squad.galaxy.neighbors(home)[0];
    const uint32_t second = squad.neighbourOf(first, home);
    REQUIRE(second != kNoSystem);

    const Entity raid = squad.fleet(home, 1, makeFleet({{Hull::Corvette, 4}}));
    FleetOrders& orders = squad.orders(raid);
    setRoute(orders, first);
    REQUIRE(appendRoute(orders, second));

    bool sawFirst = false;
    squad.runUntil(
        [&] {
            if (squad.where(raid).system == first) sawFirst = true;
            return squad.where(raid).system == second;
        },
        1 * kMinute, 12 * kHour);

    CHECK(sawFirst);                                  // не срезал первую точку
    CHECK(squad.where(raid).system == second);        // дошёл до последней
    CHECK_FALSE(squad.orders(raid).routed());         // и маршрут кончился
}

TEST_CASE("маршрут: точка, где отряд уже стоит, засчитывается без полёта") {
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);

    const Entity fleet = squad.fleet(home, 1, makeFleet({{Hull::Corvette, 2}}));
    setRoute(squad.orders(fleet), home);

    squad.run(1 * kMinute);
    CHECK(squad.where(fleet).system == home);
    CHECK_FALSE(squad.orders(fleet).routed());
}

TEST_CASE("маршрут: недостижимая цель снимает маршрут, а не висит молча") {
    Squadron squad;
    const Entity fleet = squad.fleet(0, 1, makeFleet({{Hull::Corvette, 2}}));
    setRoute(squad.orders(fleet), 999999);

    squad.run(1 * kMinute);
    CHECK_FALSE(squad.orders(fleet).routed());
    CHECK(squad.where(fleet).system == 0u);
}

TEST_CASE("патруль: маршрут не кончается") {
    // «Делай так, пока меня нет» — ровно то, ради чего стойка существует.
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);
    const uint32_t away = squad.galaxy.neighbors(home)[0];

    const Entity patrol = squad.fleet(home, 1, makeFleet({{Hull::Corvette, 3}}),
                                      Stance::Patrol);
    FleetOrders& orders = squad.orders(patrol);
    setRoute(orders, away);
    REQUIRE(appendRoute(orders, home));

    uint32_t visits = 0;
    uint32_t previous = squad.where(patrol).system;
    for (int step = 0; step < 24 * 30; ++step) {
        squad.run(2 * kMinute);
        const uint32_t now = squad.where(patrol).system;
        if (now != previous) {
            ++visits;
            previous = now;
        }
    }

    // Ходил туда-обратно, а не сходил один раз и встал.
    CAPTURE(visits);
    CHECK(visits >= 3);
    CHECK(squad.orders(patrol).routed());   // маршрут жив и через сутки
}

TEST_CASE("патруль: из одной точки не выходит") {
    // Ходить по кругу из системы в неё же значит стоять, но с видом
    // занятого. Такой «патруль» вводил бы игрока в заблуждение о том,
    // что отряд чем-то занят.
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);

    const Entity patrol = squad.fleet(home, 1, makeFleet({{Hull::Corvette, 3}}),
                                      Stance::Patrol);
    setRoute(squad.orders(patrol), home);

    squad.run(10 * kMinute);
    CHECK_FALSE(squad.orders(patrol).routed());
}

// ---------------------------------------------------------------------------
// Стойки
// ---------------------------------------------------------------------------

TEST_CASE("охрана: отряд возвращается к приписке сам") {
    // Без этого правила гарнизон, отогнавший рейдера на соседнюю систему,
    // там и оставался — то есть оборона тыла требовала присутствия игрока,
    // а значит не существовала.
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);
    const uint32_t away = squad.galaxy.neighbors(home)[0];

    const Entity guard = squad.fleet(home, 1, makeFleet({{Hull::Corvette, 5}}),
                                     Stance::Guard);
    squad.orders(guard).anchor = home;
    setRoute(squad.orders(guard), away);

    // Ушёл по приказу.
    CHECK(squad.runUntil([&] { return squad.where(guard).system == away; },
                         1 * kMinute, 6 * kHour));

    // И вернулся сам, без единого нового приказа.
    CHECK(squad.runUntil(
        [&] {
            return squad.where(guard).system == home && !squad.orders(guard).routed();
        },
        1 * kMinute, 6 * kHour));
}

TEST_CASE("стоять: отряд остаётся там, где встал") {
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);
    const uint32_t away = squad.galaxy.neighbors(home)[0];

    const Entity fleet = squad.fleet(home, 1, makeFleet({{Hull::Corvette, 5}}),
                                     Stance::Hold);
    squad.orders(fleet).anchor = home;
    setRoute(squad.orders(fleet), away);

    REQUIRE(squad.runUntil([&] { return squad.where(fleet).system == away; },
                           1 * kMinute, 6 * kHour));

    // Сутки спустя — всё там же. «Стоять» означает стоять.
    squad.run(24 * kHour);
    CHECK(squad.where(fleet).system == away);
}

TEST_CASE("уклонение: отряд уходит от превосходящего") {
    // Единственная механика во всей игре, которая работает, пока игрок
    // спит. Разведчик, конвой, эскорт колониста живут ровно до первой
    // встречи с линейным флотом.
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);
    const uint32_t away = squad.galaxy.neighbors(home)[0];

    const Entity scout = squad.fleet(away, 1, makeFleet({{Hull::Corvette, 4}}));
    squad.orders(scout).anchor = home;
    squad.orders(scout).evade = 1;

    // Втрое превосходящий противник в той же системе.
    squad.fleet(away, 2, makeFleet({{Hull::Battleship, 3}}));

    CHECK(squad.runUntil([&] { return squad.where(scout).system == home; },
                         1 * kMinute, 8 * kHour));
}

TEST_CASE("уклонение: от слабого не уходят") {
    // Иначе «уклоняться» означало бы «никогда не воевать», то есть кнопку
    // «выключить флот».
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);
    const uint32_t away = squad.galaxy.neighbors(home)[0];

    const Entity line = squad.fleet(away, 1, makeFleet({{Hull::Battleship, 4}}));
    squad.orders(line).anchor = home;
    squad.orders(line).evade = 1;

    squad.fleet(away, 2, makeFleet({{Hull::Corvette, 2}}));

    squad.run(6 * kHour);
    CHECK(squad.where(line).system == away);
}

TEST_CASE("уклонение: дома не уклоняются") {
    // Уклонение — это уход ДОМОЙ. Дома уходить некуда, и отряд принимает
    // бой: иначе гарнизон убегал бы от той самой осады, ради отражения
    // которой он там стоит.
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);

    const Entity guard = squad.fleet(home, 1, makeFleet({{Hull::Corvette, 4}}));
    squad.orders(guard).anchor = home;
    squad.orders(guard).evade = 1;

    squad.fleet(home, 2, makeFleet({{Hull::Battleship, 5}}));

    squad.run(6 * kHour);
    CHECK(squad.where(guard).system == home);
}

// ---------------------------------------------------------------------------
// Резерв и слияние
// ---------------------------------------------------------------------------

TEST_CASE("резерв: нетронутые отряды сливаются") {
    // Правило нужно и игроку, и серверу: без слияния прогон сезона
    // набирал 98 отрядов по одному кораблю.
    Squadron squad;
    squad.fleet(4, 1, makeFleet({{Hull::Corvette, 3}}), Stance::Reserve);
    squad.fleet(4, 1, makeFleet({{Hull::Corvette, 2}}), Stance::Reserve);
    REQUIRE(squad.fleetsOf(1) == 2);

    squad.run(1 * kSecond);
    CHECK(squad.fleetsOf(1) == 1);
}

TEST_CASE("резерв: тронутые отряды НЕ сливаются") {
    // САМОЕ ВАЖНОЕ ПРАВИЛО ВО ВСЁМ ФАЙЛЕ. Пока сливался любой стоящий
    // отряд без приказа, два своих отряда нельзя было держать раздельно
    // дома: выделил рейдовую группу — на следующем тике она снова в общей
    // куче. Одно это делало управление флотом в духе RTS невозможным.
    Squadron squad;
    squad.fleet(4, 1, makeFleet({{Hull::Corvette, 3}}), Stance::Hold);
    squad.fleet(4, 1, makeFleet({{Hull::Corvette, 2}}), Stance::Hold);

    squad.run(1 * kHour);
    CHECK(squad.fleetsOf(1) == 2);
}

TEST_CASE("резерв: с тронутым отрядом резерв тоже не сливается") {
    Squadron squad;
    squad.fleet(4, 1, makeFleet({{Hull::Corvette, 3}}), Stance::Reserve);
    squad.fleet(4, 1, makeFleet({{Hull::Corvette, 2}}), Stance::Hold);

    squad.run(1 * kHour);
    CHECK(squad.fleetsOf(1) == 2);
}

TEST_CASE("резерв: первый приказ выводит из резерва") {
    FleetOrders orders = idleOrders(1);
    REQUIRE(orders.stance == uint8_t(Stance::Reserve));

    commandGiven(orders);
    CHECK(orders.stance == uint8_t(Stance::Hold));

    // Второй приказ стойку уже не трогает: игрок мог поставить охрану,
    // и следующий приказ на движение не имеет права её отменить.
    orders.stance = uint8_t(Stance::Guard);
    commandGiven(orders);
    CHECK(orders.stance == uint8_t(Stance::Guard));
}

// ---------------------------------------------------------------------------
// Разделение
// ---------------------------------------------------------------------------

TEST_CASE("разделение: составом за один приказ") {
    Fleet fleet = makeFleet({{Hull::Corvette, 10}, {Hull::Destroyer, 4},
                             {Hull::Tender, 2}, {Hull::Colonizer, 1}});
    const Fleet take = makeFleet({{Hull::Corvette, 6}, {Hull::Destroyer, 2},
                                  {Hull::Tender, 1}});

    CHECK(splitCheck(fleet, standingAt(3), take) == SplitRefusal::Ok);
    const Fleet taken = applySplit(fleet, take);

    CHECK(taken[Hull::Corvette] == 6u);
    CHECK(taken[Hull::Destroyer] == 2u);
    CHECK(taken[Hull::Tender] == 1u);
    CHECK(taken[Hull::Colonizer] == 0u);

    CHECK(fleet[Hull::Corvette] == 4u);
    CHECK(fleet[Hull::Destroyer] == 2u);
    CHECK(fleet[Hull::Tender] == 1u);
    CHECK(fleet[Hull::Colonizer] == 1u);
}

TEST_CASE("разделение: чего нет — того не выделить") {
    const Fleet fleet = makeFleet({{Hull::Corvette, 3}});
    CHECK(splitCheck(fleet, standingAt(0), makeFleet({{Hull::Corvette, 4}})) ==
          SplitRefusal::NotEnough);
    CHECK(splitCheck(fleet, standingAt(0), makeFleet({{Hull::Titan, 1}})) ==
          SplitRefusal::NotEnough);
    CHECK(splitCheck(fleet, standingAt(0), Fleet{}) == SplitRefusal::NotEnough);
}

TEST_CASE("разделение: весь флот выделить нельзя") {
    // Исходный отряд опустеет и будет распущен, а новый займёт его место:
    // игрок получит тот же флот с новым номером и решит, что игра его
    // обманула.
    const Fleet fleet = makeFleet({{Hull::Corvette, 3}, {Hull::Tender, 1}});
    CHECK(splitCheck(fleet, standingAt(0), fleet) == SplitRefusal::WholeFleet);
}

TEST_CASE("разделение: на ходу не перестраиваются") {
    const Fleet fleet = makeFleet({{Hull::Corvette, 5}});
    CHECK(splitCheck(fleet, inTransit(1, 2, fx::fromFraction(1, 2)),
                     makeFleet({{Hull::Corvette, 2}})) == SplitRefusal::InTransit);
}

TEST_CASE("разделение: половина округляется вниз") {
    // «Половина» не должна уносить последний корабль редкого класса:
    // один титан из одного при делении обязан остаться на месте.
    const Fleet fleet = makeFleet({{Hull::Corvette, 7}, {Hull::Titan, 1}});
    const Fleet half = fleetHalf(fleet);
    CHECK(half[Hull::Corvette] == 3u);
    CHECK(half[Hull::Titan] == 0u);
}

TEST_CASE("разделение: заготовка «только этот класс»") {
    const Fleet fleet = makeFleet({{Hull::Corvette, 7}, {Hull::Colonizer, 2}});
    const Fleet only = fleetOnly(fleet, Hull::Colonizer);
    CHECK(only[Hull::Colonizer] == 2u);
    CHECK(only[Hull::Corvette] == 0u);
    CHECK(fleetShipCount(only) == 2u);
}

TEST_CASE("разделение: выделенный отряд не сливается обратно") {
    // Без этого разделение выглядело бы как «кнопка ничего не делает»:
    // отряд выходит и на следующем же тике возвращается.
    Squadron squad;
    const Entity main = squad.fleet(6, 1, makeFleet({{Hull::Corvette, 10}}),
                                    Stance::Reserve);

    Fleet& composition = *squad.world.get<Fleet>(main);
    const Fleet taken = applySplit(composition, makeFleet({{Hull::Corvette, 4}}));
    commandGiven(squad.orders(main));

    FleetBirth birth;
    birth.stance = Stance::Hold;
    squad.commands.spawnFleet(1, 6, taken, nullptr, birth);

    squad.run(1 * kHour);
    CHECK(squad.fleetsOf(1) == 2);
}

// ---------------------------------------------------------------------------
// Слияние
// ---------------------------------------------------------------------------

TEST_CASE("слияние: свои стоящие в одной системе") {
    CHECK(mergeCheck(1, standingAt(4), 1, standingAt(4), false) == MergeRefusal::Ok);
    CHECK(mergeCheck(1, standingAt(4), 2, standingAt(4), false) == MergeRefusal::NotYours);
    CHECK(mergeCheck(1, standingAt(4), 1, standingAt(5), false) == MergeRefusal::Apart);
    CHECK(mergeCheck(1, standingAt(4), 1, inTransit(4, 5, fx::zero()), false) ==
          MergeRefusal::InTransit);
    CHECK(mergeCheck(1, standingAt(4), 1, standingAt(4), true) == MergeRefusal::SameFleet);
}

TEST_CASE("слияние: состав складывается, источник пустеет") {
    Fleet into = makeFleet({{Hull::Corvette, 3}, {Hull::Destroyer, 1}});
    Fleet from = makeFleet({{Hull::Corvette, 2}, {Hull::Tender, 4}});

    applyMerge(into, from);

    CHECK(into[Hull::Corvette] == 5u);
    CHECK(into[Hull::Destroyer] == 1u);
    CHECK(into[Hull::Tender] == 4u);
    CHECK(fleetEmpty(from));
}

// ---------------------------------------------------------------------------
// Приписка
// ---------------------------------------------------------------------------

TEST_CASE("приписка: отряд встаёт на орбиту своей планеты") {
    // «Привязать флот к планете» игрок читает как обещание. Отряд,
    // вставший у соседнего мира, потому что успел раньше, это обещание
    // нарушает.
    Squadron squad;
    uint32_t system = kNoSystem;
    for (uint32_t index = 0; index < squad.galaxy.systemCount(); ++index) {
        if (squad.galaxy.planetCount(index) >= 3) { system = index; break; }
    }
    REQUIRE(system != kNoSystem);

    const Entity guard = squad.fleet(system, 1, makeFleet({{Hull::Corvette, 4}}));
    squad.orders(guard).anchor = system;
    squad.orders(guard).anchorOrbit = 2;

    squad.run(1 * kSecond);
    CHECK(squad.where(guard).orbit == 2u);

    // И перестаёт держаться за неё, как только приписку сняли.
    squad.orders(guard).anchorOrbit = 0;
    squad.run(1 * kSecond);
    CHECK(squad.where(guard).orbit == 0u);
}

TEST_CASE("приписка: в пути орбиты нет") {
    Squadron squad;
    const uint32_t home = forkSystem(squad);
    REQUIRE(home != kNoSystem);
    const uint32_t away = squad.galaxy.neighbors(home)[0];

    const Entity fleet = squad.fleet(home, 1, makeFleet({{Hull::Corvette, 2}}));
    squad.orders(fleet).anchor = home;
    squad.orders(fleet).anchorOrbit = 0;
    setRoute(squad.orders(fleet), away);

    squad.run(2 * kMinute);
    REQUIRE(squad.where(fleet).system != squad.where(fleet).nextSystem);
    CHECK(squad.where(fleet).orbit == kNoOrbit);
}

// ---------------------------------------------------------------------------
// Номера отрядов
// ---------------------------------------------------------------------------

TEST_CASE("номер: выдаётся наименьший свободный и освобождается со смертью") {
    // Сквозной счётчик за сезон дорос бы до четырёхзначных номеров,
    // и «отряд 1743» перестал бы что-либо называть.
    Squadron squad;
    squad.commands.spawnFleet(1, 5, makeFleet({{Hull::Corvette, 1}}));
    squad.commands.spawnFleet(1, 5, makeFleet({{Hull::Destroyer, 1}}));
    squad.commands.apply(squad.world);

    std::vector<uint16_t> tags;
    Entity second = kNoEntity;
    squad.world.each<FleetOrders, Owner>([&](Entity e, FleetOrders& orders, Owner& owner) {
        if (owner.empire != 1) return;
        tags.push_back(orders.tag);
        if (orders.tag == 2) second = e;
    });
    std::sort(tags.begin(), tags.end());
    REQUIRE(tags.size() == 2);
    CHECK(tags[0] == 1);
    CHECK(tags[1] == 2);

    // Второй погиб — его номер снова свободен, и следующий получает
    // именно двойку, а не тройку.
    REQUIRE(second.valid());
    squad.world.destroy(second);
    squad.commands.spawnFleet(1, 5, makeFleet({{Hull::Tender, 1}}));
    squad.commands.apply(squad.world);

    bool reused = false;
    squad.world.each<Fleet, FleetOrders, Owner>(
        [&](Entity, Fleet& fleet, FleetOrders& orders, Owner& owner) {
            if (owner.empire != 1) return;
            if (orders.tag == 2 && fleet[Hull::Tender] == 1) reused = true;
        });
    CHECK(reused);
}

TEST_CASE("номер: у разных империй свои") {
    Squadron squad;
    squad.commands.spawnFleet(1, 5, makeFleet({{Hull::Corvette, 1}}));
    squad.commands.spawnFleet(2, 7, makeFleet({{Hull::Corvette, 1}}));
    squad.commands.apply(squad.world);

    uint32_t firsts = 0;
    squad.world.each<FleetOrders, Owner>([&](Entity, FleetOrders& orders, Owner&) {
        if (orders.tag == 1) ++firsts;
    });
    // По одному первому отряду у каждой империи: номер — это имя внутри
    // своего флота, а не сквозной номер по всему серверу.
    CHECK(firsts == 2);
}

// ---------------------------------------------------------------------------
// Стойки как понятия
// ---------------------------------------------------------------------------

TEST_CASE("стойки: у каждой есть имя и объяснение") {
    // Стойка — это решение игрока о поведении отряда на часы вперёд.
    // Решение без объяснения принимают наугад.
    for (uint8_t raw = 0; raw < uint8_t(Stance::Count); ++raw) {
        const Stance stance = Stance(raw);
        CAPTURE(int(raw));
        CHECK(std::string(stanceName(stance)) != "?");
        CHECK(std::string(stanceHint(stance)).size() > 20);
    }
}

TEST_CASE("отказы: у каждого своя причина словами") {
    // «Нельзя» без причины заставляет гадать. Проверяется, что причины
    // разные: одинаковый текст на два разных отказа — это то же «нельзя».
    std::vector<std::string> split, merge;
    for (uint8_t raw = 1; raw < uint8_t(SplitRefusal::Count); ++raw) {
        split.push_back(splitRefusalText(SplitRefusal(raw)));
    }
    for (uint8_t raw = 1; raw < uint8_t(MergeRefusal::Count); ++raw) {
        merge.push_back(mergeRefusalText(MergeRefusal(raw)));
    }
    auto unique = [](std::vector<std::string> texts) {
        std::sort(texts.begin(), texts.end());
        return std::unique(texts.begin(), texts.end()) == texts.end();
    };
    CHECK(split.size() >= 4);
    CHECK(merge.size() >= 4);
    CHECK(unique(split));
    CHECK(unique(merge));
}
