#include "doctest.h"

#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/season.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/production.h"

using namespace pw;
using namespace pw::sim;

namespace {

struct Realm {
    World world;
    Galaxy galaxy;
    Presence presence;

    explicit Realm(uint64_t seed = 0xC0DE, uint32_t systems = 60) {
        registerGalaxyComponents(world);
        registerFleetComponents(world);
        registerControlComponents(world);

        GalaxyParams params;
        params.seed = seed;
        params.systemCount = systems;
        galaxy.generate(world, params);
        initialiseControl(world, galaxy);

        world.setResource(&galaxy);
        world.setResource(&presence);
    }

    Entity systemAt(uint32_t index) { return galaxy.systemEntity(index); }
    Entity planetAt(uint32_t system, uint32_t orbit) {
        return galaxy.planetEntity(system, orbit);
    }
    uint32_t planetCount(uint32_t system) { return galaxy.planetCount(system); }

    /// Первая система ровно с таким числом планет.
    ///
    /// Именно поиском, а не константой: число планет выводится из сида,
    /// и захардкоженный номер системы развалился бы от любой правки
    /// генератора — причём молча, превратив тест про одну планету в тест
    /// про четыре.
    uint32_t systemWith(uint32_t planets) {
        for (uint32_t i = 0; i < galaxy.systemCount(); ++i) {
            if (galaxy.planetCount(i) == planets) return i;
        }
        return UINT32_MAX;
    }
    uint32_t systemWithAtLeast(uint32_t planets) {
        for (uint32_t i = 0; i < galaxy.systemCount(); ++i) {
            if (galaxy.planetCount(i) >= planets) return i;
        }
        return UINT32_MAX;
    }

    void setPlanetOwner(uint32_t system, uint32_t orbit, uint32_t empire, fx readiness) {
        const Entity planet = planetAt(system, orbit);
        REQUIRE(planet.valid());
        world.get<Owner>(planet)->empire = empire;
        world.get<PlanetDefense>(planet)->readiness = readiness;
    }

    /// Отдать одной империи всю систему целиком.
    void setOwner(uint32_t system, uint32_t empire, fx readiness) {
        for (uint32_t orbit = 0; orbit < planetCount(system); ++orbit) {
            setPlanetOwner(system, orbit, empire, readiness);
        }
        world.get<Owner>(systemAt(system))->empire = empire;
    }

    Entity garrison(uint32_t system, uint32_t empire, const Fleet& composition) {
        const Entity e = world.create();
        world.add<Fleet>(e, composition);
        world.add<FleetLocation>(e, FleetLocation{system, system, fx::zero()});
        world.add<MoveOrder>(e, MoveOrder{kNoSystem, 0});
        world.add<Owner>(e, Owner{empire, 0});
        return e;
    }

    void run(int64_t ticks) {
        for (int64_t i = 0; i < ticks; ++i) {
            TickContext context;
            context.tick = uint64_t(i);
            systemControlRollup(world, context);
            systemPresence(world, context);
            systemSiege(world, context);
        }
    }

    /// Поставить на планету застройку. Экономику тесты осады не гоняют,
    /// но щит, гарнизон и док читаются именно из неё.
    void develop(uint32_t system, uint32_t orbit,
                 std::initializer_list<Building> buildings) {
        const Entity planet = planetAt(system, orbit);
        REQUIRE(planet.valid());
        PlanetDevelopment development{};
        uint8_t slot = 0;
        for (Building building : buildings) {
            development.buildings[slot++] = uint8_t(building);
        }
        if (PlanetDevelopment* existing = world.get<PlanetDevelopment>(planet)) {
            *existing = development;
        } else {
            world.add<PlanetDevelopment>(planet, development);
        }
    }

    uint32_t ownerOf(uint32_t system) { return world.get<Owner>(systemAt(system))->empire; }
    uint32_t planetOwner(uint32_t system, uint32_t orbit) {
        return world.get<Owner>(planetAt(system, orbit))->empire;
    }
    fx readinessOf(uint32_t system, uint32_t orbit = 0) {
        return world.get<PlanetDefense>(planetAt(system, orbit))->readiness;
    }
    const SiegeState* siegeOf(uint32_t system, uint32_t orbit = 0) {
        return world.get<SiegeState>(planetAt(system, orbit));
    }
    uint32_t planetsOwnedBy(uint32_t system, uint32_t empire) {
        uint32_t total = 0;
        for (uint32_t orbit = 0; orbit < planetCount(system); ++orbit) {
            if (planetOwner(system, orbit) == empire) ++total;
        }
        return total;
    }
};

constexpr int64_t kSecond = kTicksPerSecond;
constexpr int64_t kMinute = 60 * kSecond;

}  // namespace

// ---------------------------------------------------------------------------
// Производное владение системой
// ---------------------------------------------------------------------------

TEST_CASE("владение: галактика начинается ничьей") {
    Realm realm;
    for (uint32_t i = 0; i < realm.galaxy.systemCount(); ++i) {
        CHECK(realm.ownerOf(i) == kNoEmpire);
        CHECK(realm.planetCount(i) > 0);  // владеть должно быть чем в КАЖДОЙ системе
        for (uint32_t orbit = 0; orbit < realm.planetCount(i); ++orbit) {
            CHECK(realm.planetOwner(i, orbit) == kNoEmpire);
            CHECK(realm.readinessOf(i, orbit) == fx::zero());
        }
    }
}

TEST_CASE("владение: у чёрной дыры есть станция") {
    // Без ownable-тела ценнейшая система карты выпала бы из игры целиком:
    // её нельзя было бы ни взять, ни оборонять, ни застроить.
    Realm realm(0xB1AC, 400);
    uint32_t holes = 0;
    for (uint32_t i = 0; i < realm.galaxy.systemCount(); ++i) {
        if (realm.galaxy.starClass(i) != uint8_t(StarClass::BlackHole)) continue;
        ++holes;
        REQUIRE(realm.planetCount(i) >= 1);
        const Planet* body = realm.world.get<Planet>(realm.planetAt(i, 0));
        REQUIRE(body != nullptr);
        CHECK(body->planetClass == uint8_t(PlanetClass::Station));
        CHECK(body->slots > 0);
    }
    CHECK(holes > 0);  // иначе тест ничего не проверил
}

TEST_CASE("владение: система достаётся тому, у кого больше планет") {
    Realm realm;
    const uint32_t system = realm.systemWithAtLeast(3);
    REQUIRE(system != UINT32_MAX);

    realm.setPlanetOwner(system, 0, /*empire=*/1, kReadinessMax);
    realm.setPlanetOwner(system, 1, /*empire=*/1, kReadinessMax);
    realm.setPlanetOwner(system, 2, /*empire=*/2, kReadinessMax);

    TickContext context;
    systemControlRollup(realm.world, context);
    CHECK(realm.ownerOf(system) == 1u);
}

TEST_CASE("владение: поровну — система спорная") {
    // Спорная система не тыл никому: она не даёт своему «владельцу»
    // ни права строить флот, ни статуса на карте.
    Realm realm;
    const uint32_t system = realm.systemWithAtLeast(2);
    REQUIRE(system != UINT32_MAX);

    realm.setPlanetOwner(system, 0, /*empire=*/1, kReadinessMax);
    realm.setPlanetOwner(system, 1, /*empire=*/2, kReadinessMax);
    for (uint32_t orbit = 2; orbit < realm.planetCount(system); ++orbit) {
        realm.setPlanetOwner(system, orbit, kNoEmpire, fx::zero());
    }

    TickContext context;
    systemControlRollup(realm.world, context);
    CHECK(realm.ownerOf(system) == kNoEmpire);
}

// ---------------------------------------------------------------------------
// Занятие ничьих планет
// ---------------------------------------------------------------------------

TEST_CASE("владение: флот занимает ничью планету за kClaimSeconds") {
    // Срок берётся из константы, а не пишется числом: он балансный
    // и уже менялся — с пяти минут на три, когда захват переехал
    // с систем на планеты.
    Realm realm;
    realm.garrison(3, /*empire=*/1, makeFleet({{Hull::Corvette, 2}}));

    realm.run(kClaimSeconds * kSecond - 10 * kSecond);
    CHECK(realm.planetOwner(3, 0) == kNoEmpire);  // ещё рано

    realm.run(20 * kSecond);
    CHECK(realm.planetOwner(3, 0) == 1u);
    // Занятая ничья планета сразу боеспособна: воюют не за пустоту.
    CHECK(realm.readinessOf(3, 0) == kReadinessMax);
}

TEST_CASE("владение: планеты занимаются по одной, от звезды наружу") {
    // Это и есть перенос захвата с систем на планеты: система с четырьмя
    // планетами больше не берётся одним щелчком за то же время, что
    // и система с одной.
    Realm realm;
    const uint32_t system = realm.systemWithAtLeast(3);
    REQUIRE(system != UINT32_MAX);
    realm.garrison(system, /*empire=*/1, makeFleet({{Hull::Corvette, 4}}));

    realm.run(kClaimSeconds * kSecond + 10 * kSecond);
    CHECK(realm.planetOwner(system, 0) == 1u);
    // Вторая ещё ничья: срок занятия даёт ОДНУ планету, а не систему.
    CHECK(realm.planetOwner(system, 1) == kNoEmpire);
    CHECK(realm.planetsOwnedBy(system, 1) == 1u);

    realm.run(kClaimSeconds * kSecond);
    CHECK(realm.planetOwner(system, 1) == 1u);
    CHECK(realm.planetsOwnedBy(system, 1) == 2u);

    // Вся система целиком — только когда занята каждая планета.
    realm.run(kClaimSeconds * kSecond * int64_t(realm.planetCount(system)));
    CHECK(realm.planetsOwnedBy(system, 1) == realm.planetCount(system));
    CHECK(realm.ownerOf(system) == 1u);
}

TEST_CASE("владение: два своих флота не мешают занять планету") {
    // Правило «занимает только единственный претендент» считает ИМПЕРИИ,
    // а не флоты. Пока считались флоты, империя, приведшая два отряда,
    // блокировала занятие сама себе, и экспансия вставала намертво.
    // Юнит-тесты этого не видели — поймал прогон сезона на ботах.
    Realm realm;
    realm.garrison(4, /*empire=*/1, makeFleet({{Hull::Corvette, 3}}));
    realm.garrison(4, /*empire=*/1, makeFleet({{Hull::Corvette, 5}}));
    realm.garrison(4, /*empire=*/1, makeFleet({{Hull::Corvette, 2}}));

    realm.run(kClaimSeconds * kSecond + 10 * kSecond);
    CHECK(realm.planetOwner(4, 0) == 1u);
}

TEST_CASE("владение: два претендента не занимают ничего") {
    Realm realm;
    realm.garrison(5, /*empire=*/1, makeFleet({{Hull::Corvette, 5}}));
    realm.garrison(5, /*empire=*/2, makeFleet({{Hull::Corvette, 5}}));

    realm.run(20 * kMinute);
    // Пока не разобрались между собой, планета остаётся ничьей.
    CHECK(realm.planetOwner(5, 0) == kNoEmpire);
    CHECK(realm.ownerOf(5) == kNoEmpire);
}

TEST_CASE("владение: ушедший флот не дозанимает планету") {
    Realm realm;
    const Entity fleet = realm.garrison(7, /*empire=*/1, makeFleet({{Hull::Corvette, 3}}));

    realm.run(kClaimSeconds * kSecond / 2);
    // Флот отправился дальше — счётчик занятия обязан обнулиться.
    realm.world.get<FleetLocation>(fleet)->nextSystem =
        realm.galaxy.neighbors(7)[0];

    realm.run(kClaimSeconds * kSecond);
    CHECK(realm.planetOwner(7, 0) == kNoEmpire);
}

// ---------------------------------------------------------------------------
// Осада
// ---------------------------------------------------------------------------

TEST_CASE("осада: чужая планета падает за десятки минут, а не мгновенно") {
    Realm realm;
    realm.setOwner(9, /*empire=*/1, kReadinessMax);
    realm.garrison(9, /*empire=*/2, makeFleet({{Hull::Corvette, 20}}));  // тоннаж 20

    // Через минуту планета ещё держится. Это и есть главное свойство:
    // мгновенных потерь в игре нет.
    realm.run(1 * kMinute);
    CHECK(realm.planetOwner(9, 0) == 1u);
    CHECK(realm.readinessOf(9, 0) < kReadinessMax);

    realm.run(29 * kMinute);
    CHECK(realm.planetOwner(9, 0) == 1u);  // и через полчаса тоже

    realm.run(45 * kMinute);
    CHECK(realm.planetOwner(9, 0) == 2u);
}

TEST_CASE("осада: укладывается в обещанные дизайном 30–90 минут") {
    auto minutesToFall = [](uint32_t tonnage) {
        Realm realm;
        realm.setOwner(11, /*empire=*/1, kReadinessMax);
        // Тоннаж набираем корветами: один корвет — одна единица.
        realm.garrison(11, /*empire=*/2, makeFleet({{Hull::Corvette, tonnage}}));

        for (int64_t minute = 1; minute <= 240; ++minute) {
            realm.run(kMinute);
            if (realm.planetOwner(11, 0) == 2u) return minute;
        }
        return int64_t(-1);
    };

    const int64_t small = minutesToFall(10);
    const int64_t large = minutesToFall(400);
    CAPTURE(small);
    CAPTURE(large);

    CHECK(small > 30);
    CHECK(small <= 90);
    CHECK(large >= 30);
    CHECK(large < small);

    // Втрое больший флот НЕ берёт планету втрое быстрее: иначе выигрывал бы
    // просто тот, кто собрал всё в один кулак, и делить силы стало бы незачем.
    CHECK(double(small) / double(large) < 2.5);
}

TEST_CASE("осада: защитники срывают её немедленно") {
    Realm realm;
    realm.setOwner(13, /*empire=*/1, kReadinessMax);
    realm.garrison(13, /*empire=*/2, makeFleet({{Hull::Corvette, 50}}));

    realm.run(10 * kMinute);
    const fx damaged = realm.readinessOf(13, 0);
    CHECK(damaged < kReadinessMax);

    // Деблокирующий удар: пришли свои — осада снята.
    realm.garrison(13, /*empire=*/1, makeFleet({{Hull::Corvette, 1}}));
    realm.run(1 * kSecond);
    CHECK(realm.siegeOf(13, 0)->besieger == kNoEmpire);

    // И оборона начинает восстанавливаться.
    realm.run(10 * kMinute);
    CHECK(realm.readinessOf(13, 0) > damaged);
    CHECK(realm.planetOwner(13, 0) == 1u);
}

TEST_CASE("осада: свежий захват слаб") {
    Realm realm;
    realm.setOwner(15, /*empire=*/1, kReadinessMax);
    realm.garrison(15, /*empire=*/2, makeFleet({{Hull::Battleship, 30}}));  // тяжёлый флот

    for (int64_t minute = 0; minute < 200 && realm.planetOwner(15, 0) != 2u; ++minute) {
        realm.run(kMinute);
    }
    REQUIRE(realm.planetOwner(15, 0) == 2u);

    // Взятая планета достаётся четвертью готовности: отбить её обратно
    // реально, и фронт может ходить, а не застывать после первого удара.
    CHECK(realm.readinessOf(15, 0) <= kReadinessMax * kCaptureReadinessShare + fx::one());
    CHECK(realm.readinessOf(15, 0) > fx::zero());
}

TEST_CASE("осада: оборона восстанавливается в мирное время") {
    Realm realm;
    realm.setOwner(17, /*empire=*/1, fx::zero());

    realm.run(50 * kMinute);
    const fx half = realm.readinessOf(17, 0);
    CHECK(half > fx::zero());
    CHECK(half < kReadinessMax);

    realm.run(60 * kMinute);
    // Полное восстановление около ста минут: планета, пережившая осаду,
    // ещё долго остаётся уязвимой.
    CHECK(realm.readinessOf(17, 0) == kReadinessMax);
}

TEST_CASE("осада: флот в пути не участвует ни в чём") {
    Realm realm;
    realm.setOwner(19, /*empire=*/1, kReadinessMax);

    const Entity raider = realm.garrison(19, /*empire=*/2, makeFleet({{Hull::Corvette, 50}}));
    // Флот вышел из системы: он между узлами и осаждать не может.
    realm.world.get<FleetLocation>(raider)->nextSystem = realm.galaxy.neighbors(19)[0];

    realm.run(30 * kMinute);
    CHECK(realm.readinessOf(19, 0) == kReadinessMax);
    CHECK(realm.planetOwner(19, 0) == 1u);
}

TEST_CASE("осада: смена осаждающего обнуляет счётчик") {
    Realm realm;
    realm.setOwner(21, /*empire=*/1, kReadinessMax);
    const Entity first = realm.garrison(21, /*empire=*/2, makeFleet({{Hull::Corvette, 40}}));

    realm.run(5 * kMinute);
    CHECK(realm.siegeOf(21, 0)->besieger == 2u);
    const uint32_t ticksBefore = realm.siegeOf(21, 0)->ticks;
    CHECK(ticksBefore > 0);

    // Первый ушёл, пришёл третий — счёт осады начинается заново, но
    // повреждения обороны остаются: они уже нанесены.
    realm.world.get<FleetLocation>(first)->nextSystem = realm.galaxy.neighbors(21)[0];
    realm.garrison(21, /*empire=*/3, makeFleet({{Hull::Corvette, 40}}));
    realm.run(1 * kSecond);

    const SiegeState* siege = realm.siegeOf(21, 0);
    CHECK(siege->besieger == 3u);
    CHECK(siege->ticks < ticksBefore);
    CHECK(realm.readinessOf(21, 0) < kReadinessMax);
}

TEST_CASE("осада: анклав в чужом тылу обороняется сам") {
    // Держать планету в чужой системе — законный ход. Флот соседа её
    // не обороняет: защитники считаются по владельцу ПЛАНЕТЫ, а не системы.
    Realm realm;
    const uint32_t system = realm.systemWithAtLeast(2);
    REQUIRE(system != UINT32_MAX);

    realm.setOwner(system, /*empire=*/1, kReadinessMax);
    realm.setPlanetOwner(system, 1, /*empire=*/2, kReadinessMax);

    // Империя 1 стоит в системе своим флотом: её собственная планета
    // защищена, а вот анклав империи 2 — нет.
    realm.garrison(system, /*empire=*/1, makeFleet({{Hull::Corvette, 60}}));

    realm.run(10 * kMinute);
    CHECK(realm.readinessOf(system, 0) == kReadinessMax);   // своя цела
    CHECK(realm.readinessOf(system, 1) < kReadinessMax);    // анклав под осадой
    CHECK(realm.siegeOf(system, 1)->besieger == 1u);
}

TEST_CASE("осада: занятая планета не мешает осаждать следующую") {
    // Флот идёт по орбитам: взял первую — принялся за вторую. Иначе
    // «одна планета за раз» превращалась бы в «одна планета навсегда».
    Realm realm;
    const uint32_t system = realm.systemWithAtLeast(2);
    REQUIRE(system != UINT32_MAX);

    realm.setOwner(system, /*empire=*/1, kReadinessMax);
    realm.garrison(system, /*empire=*/2, makeFleet({{Hull::Battleship, 40}}));

    for (int64_t minute = 0; minute < 400 && realm.planetOwner(system, 1) != 2u; ++minute) {
        realm.run(kMinute);
    }
    CHECK(realm.planetOwner(system, 0) == 2u);
    CHECK(realm.planetOwner(system, 1) == 2u);
}

TEST_CASE("владение: воспроизводится тик в тик") {
    Realm first(0xE55, 60), second(0xE55, 60);

    for (Realm* realm : {&first, &second}) {
        realm->setOwner(4, 1, kReadinessMax);
        realm->setOwner(8, 2, kReadinessMax);
        realm->garrison(4, 2, makeFleet({{Hull::Corvette, 15}, {Hull::Destroyer, 3}, {Hull::Cruiser, 1}}));
        realm->garrison(8, 1, makeFleet({{Hull::Corvette, 9}, {Hull::Destroyer, 2}, {Hull::Battleship, 1}}));
        realm->garrison(12, 3, makeFleet({{Hull::Corvette, 5}}));
    }
    REQUIRE(first.world.hash() == second.world.hash());

    first.run(80 * kMinute);
    second.run(80 * kMinute);
    CHECK(first.world.hash() == second.world.hash());

    // И что-то действительно произошло, а не просто ничего не менялось.
    CHECK(first.planetOwner(12, 0) == 3u);
}

// ---------------------------------------------------------------------------
// Инфраструктура обороны
// ---------------------------------------------------------------------------

namespace {

/// Сколько обороны сняли за заданное время при такой застройке планеты.
fx siegeDamage(std::initializer_list<Building> buildings, const Fleet& attacker,
               int64_t seconds) {
    Realm realm(0xD1FE, 60);
    registerEconomyComponents(realm.world);

    const uint32_t system = realm.systemWith(1);
    REQUIRE(system != UINT32_MAX);
    realm.setOwner(system, /*empire=*/1, kReadinessMax);
    realm.develop(system, 0, buildings);
    realm.garrison(system, /*empire=*/2, attacker);

    realm.run(seconds * kTicksPerSecond);
    return kReadinessMax - realm.readinessOf(system);
}

}  // namespace

TEST_CASE("щит: растягивает осаду, но не отменяет её") {
    // Крепость поднимает ПОТОЛОК обороны, щит растягивает ВРЕМЯ. Разница
    // в том, что потолок сбивают большим флотом, а время — не сбить ничем,
    // кроме осадных кораблей. На этом и держится роль монитора.
    const Fleet attacker = makeFleet({{Hull::Battleship, 5}});

    const fx bare = siegeDamage({}, attacker, 600);
    const fx oneShield = siegeDamage({Building::ShieldGenerator}, attacker, 600);
    const fx twoShields = siegeDamage({Building::ShieldGenerator, Building::ShieldGenerator},
                                      attacker, 600);

    CHECK(oneShield < bare);
    CHECK(twoShields < oneShield);
    // Но осада идёт: обвешанную щитами планету можно взять, просто дольше.
    CHECK(twoShields > fx::zero());
}

TEST_CASE("монитор: ломает щит там, где линкоры уже упёрлись") {
    // Именно ради этого осадный корабль и существует. Если бы щит одинаково
    // держал любой флот, строить мониторы было бы незачем.
    const auto shields = {Building::ShieldGenerator, Building::ShieldGenerator};

    // Одинаковый бюджет в сплавах: пять линкоров против десяти мониторов.
    const fx byLine = siegeDamage(shields, makeFleet({{Hull::Battleship, 5}}), 600);
    const fx bySiege = siegeDamage(shields, makeFleet({{Hull::Monitor, 10}}), 600);

    CHECK(bySiege > byLine);
}

TEST_CASE("гарнизон: оборона возвращается быстрее") {
    // Планета, пережившая осаду, без гарнизона остаётся уязвимой полтора
    // часа. Гарнизон — ответ на вопрос «как удержать фронт», отдельный
    // от вопроса «как выдержать удар», на который отвечает крепость.
    auto regained = [](std::initializer_list<Building> buildings) {
        Realm realm(0xD1FE, 60);
        registerEconomyComponents(realm.world);
        const uint32_t system = realm.systemWith(1);
        REQUIRE(system != UINT32_MAX);
        realm.setOwner(system, /*empire=*/1, fx::zero());
        realm.develop(system, 0, buildings);
        realm.run(300 * kTicksPerSecond);
        return realm.readinessOf(system);
    };

    const fx bare = regained({});
    const fx withGarrison = regained({Building::Garrison});
    CHECK(withGarrison > bare);
    // Втрое: гарнизон даёт удвоение сверх базовой единицы.
    CHECK(withGarrison.toDouble() > bare.toDouble() * 2.5);
}

// ---------------------------------------------------------------------------
// Стадии сезона
// ---------------------------------------------------------------------------

TEST_CASE("сезон: стадия — чистая функция от тика") {
    // Не «сервер решил, что пора»: и сервер, и клиент, и реплей считают
    // стадию из одного числа одинаково, и рассинхрону взяться неоткуда.
    SeasonConfig config;
    config.expansionSeconds = 100;
    config.conflictSeconds = 200;
    config.crisisSeconds = 50;
    config.finalSeconds = 25;

    CHECK(stageAt(config, 0) == SeasonStage::Expansion);
    CHECK(stageAt(config, 99 * kTicksPerSecond) == SeasonStage::Expansion);
    CHECK(stageAt(config, 100 * kTicksPerSecond) == SeasonStage::Conflict);
    CHECK(stageAt(config, 299 * kTicksPerSecond) == SeasonStage::Conflict);
    CHECK(stageAt(config, 300 * kTicksPerSecond) == SeasonStage::Crisis);
    CHECK(stageAt(config, 350 * kTicksPerSecond) == SeasonStage::Final);
    // За краем сезона стадия не «сбрасывается» — Финал так и остаётся.
    CHECK(stageAt(config, 100000 * kTicksPerSecond) == SeasonStage::Final);

    // Обратный отсчёт согласован со стадией.
    CHECK(secondsLeftInStage(config, 0) == 100);
    CHECK(secondsLeftInStage(config, 60 * kTicksPerSecond) == 40);
    CHECK(secondsLeftInStage(config, 100 * kTicksPerSecond) == 200);

    // Масштаб растягивает всё разом, не меняя порядка.
    config.scale = 10;
    CHECK(stageAt(config, 999 * kTicksPerSecond) == SeasonStage::Expansion);
    CHECK(stageAt(config, 1000 * kTicksPerSecond) == SeasonStage::Conflict);
    CHECK(config.totalSeconds() == 3750);
}

namespace {

/// Мир с сезоном на нужной стадии.
struct SeasonRealm : Realm {
    Season season;

    explicit SeasonRealm(SeasonStage stage) : Realm(0x5EA50, 60) {
        registerEconomyComponents(world);
        season.config.expansionSeconds = 1000;
        season.config.conflictSeconds = 1000;
        season.config.crisisSeconds = 1000;
        season.config.finalSeconds = 1000;
        season.stage = stage;
        world.setResource(&season);
    }

    /// Столица империи. Убежище считается от неё.
    void capital(uint32_t empire, uint32_t system) {
        const Entity e = world.create();
        world.add<Empire>(e, Empire{fx::zero(), fx::zero(), fx::zero(), fx::zero(),
                                    fx::zero(), empire, system});
    }
};

}  // namespace

TEST_CASE("сезон: на Расширении чужой дом неприкосновенен") {
    // Новичок обязан получить свою империю раньше, чем встретит соседа.
    // Иначе тысяча игроков превращается в тысячу человек, из которых
    // играет сотня, а остальные выбыли на первом часу.
    SeasonRealm realm(SeasonStage::Expansion);

    const uint32_t home = realm.systemWithAtLeast(2);
    REQUIRE(home != UINT32_MAX);
    realm.capital(/*empire=*/1, home);
    realm.setOwner(home, /*empire=*/1, kReadinessMax);
    realm.garrison(home, /*empire=*/2, makeFleet({{Hull::Battleship, 20}}));

    const fx before = realm.readinessOf(home);
    realm.run(600 * kTicksPerSecond);
    CHECK(realm.readinessOf(home) == before);
    CHECK(realm.planetOwner(home, 0) == 1u);
}

TEST_CASE("сезон: на Конфликте убежища больше нет") {
    SeasonRealm realm(SeasonStage::Conflict);

    const uint32_t home = realm.systemWithAtLeast(2);
    REQUIRE(home != UINT32_MAX);
    realm.capital(/*empire=*/1, home);
    realm.setOwner(home, /*empire=*/1, kReadinessMax);
    realm.garrison(home, /*empire=*/2, makeFleet({{Hull::Battleship, 20}}));

    realm.run(600 * kTicksPerSecond);
    CHECK(realm.readinessOf(home) < kReadinessMax);
}

TEST_CASE("сезон: на Финале карта заморожена") {
    // Захват в последнюю минуту не должен решать сезон: иначе вся стратегия
    // сводится к тому, чтобы не показываться до последнего часа.
    SeasonRealm realm(SeasonStage::Final);

    const uint32_t system = realm.systemWithAtLeast(2);
    REQUIRE(system != UINT32_MAX);
    realm.setOwner(system, /*empire=*/1, kReadinessMax);
    realm.garrison(system, /*empire=*/2, makeFleet({{Hull::Battleship, 20}}));

    realm.run(600 * kTicksPerSecond);
    CHECK(realm.readinessOf(system) == kReadinessMax);
}

TEST_CASE("сезон: на Расширении ничьи планеты занимают свободно") {
    // Иначе стадия расширения не давала бы расширяться — а она ровно
    // для этого и есть.
    SeasonRealm realm(SeasonStage::Expansion);

    const uint32_t system = realm.systemWith(1);
    REQUIRE(system != UINT32_MAX);
    realm.capital(/*empire=*/1, realm.systemWithAtLeast(3));
    realm.garrison(system, /*empire=*/1, makeFleet({{Hull::Corvette, 5}}));

    realm.run((kClaimSeconds + 5) * kTicksPerSecond);
    CHECK(realm.planetOwner(system, 0) == 1u);
}
