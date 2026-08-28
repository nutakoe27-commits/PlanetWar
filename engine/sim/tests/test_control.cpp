#include "doctest.h"

#include "game_time.h"

#include "pw/sim/colony.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/season.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/production.h"

using namespace pw;
using namespace pw::sim;
using namespace pw::test;

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
        world.add<FleetLocation>(e, standingAt(system));
        world.add<MoveOrder>(e, MoveOrder{kNoSystem, 0});
        world.add<Owner>(e, Owner{empire, 0});
        return e;
    }

    /// Прогнать столько ИГРОВЫХ СЕКУНД. Не тиков: осада измеряется часами,
    /// и тест, отсчитывающий её тиками, шёл бы полмиллиона итераций
    /// на один CHECK. Подробности — в game_time.h.
    void run(int64_t seconds) {
        const int64_t ticks = testTicks(seconds);
        for (int64_t i = 0; i < ticks; ++i) {
            const TickContext context = testTick(i);
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
// Ничьи планеты
//
// ПРАВИЛО ИЗМЕНИЛОСЬ, и это главное изменение геймплея за фазу.
//
// Раньше любой флот, постояв над пустой планетой три минуты, делал её своей.
// Выглядело безобидно — и обесценивало колонизацию целиком: зачем строить
// дорогой медленный корабль, если то же самое даёт бесплатная стоянка
// любым корветом.
//
// Теперь ничью планету берут ТОЛЬКО высадкой колониста. Флот над ней может
// стоять сколько угодно: он её охраняет, но не присваивает. Проверки ниже
// сторожат именно это — потому что откатить правило обратно проще всего
// случайно, «починив» тест.
// ---------------------------------------------------------------------------

TEST_CASE("владение: флот НЕ занимает ничью планету, сколько бы ни стоял") {
    Realm realm;
    realm.garrison(3, /*empire=*/1, makeFleet({{Hull::Corvette, 2}}));

    realm.run(1 * kHour);
    CHECK(realm.planetOwner(3, 0) == kNoEmpire);

    // И через сутки тоже. Дело не в сроке: механики занятия стоянкой
    // больше нет вовсе.
    realm.run(24 * kHour);
    CHECK(realm.planetOwner(3, 0) == kNoEmpire);
}

TEST_CASE("владение: даже огромный флот не занимает пустую планету") {
    // Ни тоннаж, ни осадная мощь тут ни при чём. Пустую планету не берут
    // силой — на неё высаживаются.
    Realm realm;
    realm.garrison(4, /*empire=*/1,
                   makeFleet({{Hull::Titan, 4}, {Hull::Monitor, 10}}));

    realm.run(24 * kHour);
    CHECK(realm.planetOwner(4, 0) == kNoEmpire);
}

TEST_CASE("владение: над ничьей планетой не начинается осада") {
    // Осаждать пустоту нечего: обороны там нет, и запись «идёт осада»
    // в журнале означала бы событие, которого не происходит.
    Realm realm;
    realm.garrison(5, /*empire=*/1, makeFleet({{Hull::Corvette, 5}}));
    realm.run(1 * kHour);
    const SiegeState* siege = realm.siegeOf(5, 0);
    REQUIRE(siege != nullptr);
    CHECK(siege->ticks == 0u);
    CHECK(siege->besieger == kNoEmpire);
}

// ---------------------------------------------------------------------------
// Осада
// ---------------------------------------------------------------------------

TEST_CASE("осада: чужая планета падает за часы, а не мгновенно") {
    Realm realm;
    realm.setOwner(9, /*empire=*/1, kReadinessMax);
    realm.garrison(9, /*empire=*/2, makeFleet({{Hull::Corvette, 20}}));  // тоннаж 20

    // Через час планета ещё держится, хотя оборона уже просела. Это
    // и есть главное свойство: мгновенных потерь в игре нет.
    realm.run(1 * kHour);
    CHECK(realm.planetOwner(9, 0) == 1u);
    CHECK(realm.readinessOf(9, 0) < kReadinessMax);

    // И через восемь часов тоже — то есть проспать потерю планеты нельзя.
    realm.run(7 * kHour);
    CHECK(realm.planetOwner(9, 0) == 1u);

    realm.run(8 * kHour);
    CHECK(realm.planetOwner(9, 0) == 2u);
}

TEST_CASE("осада: длиннее ночного сна и короче суток") {
    // ГЛАВНАЯ ПРОВЕРКА ТЕМПА ВСЕЙ ИГРЫ, и она про часы, а не про минуты.
    //
    // Нижняя граница — обещание игроку: ни одну планету нельзя потерять,
    // пока хозяин спит. Значит даже собранный по уму штурм обязан идти
    // дольше любого сна. Верхняя — обещание нападающему: осада, которая
    // не заканчивается за сутки, перестаёт быть операцией и становится
    // образом жизни, а флот всё это время стоит и ничего не делает.
    auto hoursToFall = [](uint32_t tonnage) {
        Realm realm;
        realm.setOwner(11, /*empire=*/1, kReadinessMax);
        // Тоннаж набираем корветами: один корвет — одна единица.
        realm.garrison(11, /*empire=*/2, makeFleet({{Hull::Corvette, tonnage}}));

        for (int64_t hour = 1; hour <= 48; ++hour) {
            realm.run(kHour);
            if (realm.planetOwner(11, 0) == 2u) return hour;
        }
        return int64_t(-1);
    };

    const int64_t small = hoursToFall(10);
    const int64_t large = hoursToFall(400);
    CAPTURE(small);
    CAPTURE(large);

    CHECK(small > 8);     // скромным флотом — дольше ночи
    CHECK(small <= 24);   // но всё-таки за сутки
    CHECK(large > 4);     // и даже полным кулаком — дольше рабочего дня
    CHECK(large < small);

    // Втрое больший флот НЕ берёт планету втрое быстрее: иначе выигрывал бы
    // просто тот, кто собрал всё в один кулак, и делить силы стало бы незачем.
    CHECK(double(small) / double(large) < 2.5);
}

TEST_CASE("осада: защитники срывают её немедленно") {
    Realm realm;
    realm.setOwner(13, /*empire=*/1, kReadinessMax);
    realm.garrison(13, /*empire=*/2, makeFleet({{Hull::Corvette, 50}}));

    realm.run(2 * kHour);
    const fx damaged = realm.readinessOf(13, 0);
    CHECK(damaged < kReadinessMax);

    // Деблокирующий удар: пришли свои — осада снята.
    realm.garrison(13, /*empire=*/1, makeFleet({{Hull::Corvette, 1}}));
    realm.run(1 * kSecond);
    CHECK(realm.siegeOf(13, 0)->besieger == kNoEmpire);

    // И оборона начинает восстанавливаться.
    realm.run(2 * kHour);
    CHECK(realm.readinessOf(13, 0) > damaged);
    CHECK(realm.planetOwner(13, 0) == 1u);
}

TEST_CASE("осада: свежий захват слаб") {
    Realm realm;
    realm.setOwner(15, /*empire=*/1, kReadinessMax);
    realm.garrison(15, /*empire=*/2, makeFleet({{Hull::Battleship, 30}}));  // тяжёлый флот

    // Шаг мелкий НАМЕРЕННО: сразу после падения планета начинает отрастать,
    // и на часовом шаге к моменту замера она успела бы подняться выше
    // четверти. Проверять надо сам момент захвата, а не «через час после».
    for (int64_t step = 0; step < 48 * 12 && realm.planetOwner(15, 0) != 2u; ++step) {
        realm.run(5 * kMinute);
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

    realm.run(10 * kHour);
    const fx half = realm.readinessOf(17, 0);
    CHECK(half > fx::zero());
    CHECK(half < kReadinessMax);

    realm.run(12 * kHour);
    // Полное восстановление около двадцати одного часа: планета, пережившая
    // осаду, остаётся уязвимой почти сутки, и вернуться за ней назавтра —
    // законный ход.
    CHECK(realm.readinessOf(17, 0) == kReadinessMax);
}

TEST_CASE("осада: флот в пути не участвует ни в чём") {
    Realm realm;
    realm.setOwner(19, /*empire=*/1, kReadinessMax);

    const Entity raider = realm.garrison(19, /*empire=*/2, makeFleet({{Hull::Corvette, 50}}));
    // Флот вышел из системы: он между узлами и осаждать не может.
    realm.world.get<FleetLocation>(raider)->nextSystem = realm.galaxy.neighbors(19)[0];

    realm.run(24 * kHour);
    CHECK(realm.readinessOf(19, 0) == kReadinessMax);
    CHECK(realm.planetOwner(19, 0) == 1u);
}

TEST_CASE("осада: смена осаждающего обнуляет счётчик") {
    Realm realm;
    realm.setOwner(21, /*empire=*/1, kReadinessMax);
    const Entity first = realm.garrison(21, /*empire=*/2, makeFleet({{Hull::Corvette, 40}}));

    realm.run(1 * kHour);
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

    realm.run(2 * kHour);
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

    for (int64_t hour = 0; hour < 72 && realm.planetOwner(system, 1) != 2u; ++hour) {
        realm.run(kHour);
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

    first.run(20 * kHour);
    second.run(20 * kHour);
    CHECK(first.world.hash() == second.world.hash());

    // И что-то действительно произошло, а не просто ничего не менялось.
    //
    // Раньше признаком «произошло» служило занятие ничьей планеты флотом
    // империи 3. Занятия стоянкой больше нет, и признаком стала осада:
    // империя 2 стоит над планетой империи 1 в системе 4 и за двадцать
    // часов обязана её взять. Свойство то же — мир не стоял на месте, —
    // но опирается оно на механику, которая есть.
    CHECK(first.planetOwner(4, 0) == 2u);
    CHECK(second.planetOwner(4, 0) == 2u);
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

    realm.run(seconds);
    return kReadinessMax - realm.readinessOf(system);
}

}  // namespace

TEST_CASE("щит: растягивает осаду, но не отменяет её") {
    // Крепость поднимает ПОТОЛОК обороны, щит растягивает ВРЕМЯ. Разница
    // в том, что потолок сбивают большим флотом, а время — не сбить ничем,
    // кроме осадных кораблей. На этом и держится роль монитора.
    const Fleet attacker = makeFleet({{Hull::Battleship, 5}});

    const fx bare = siegeDamage({}, attacker, 2 * kHour);
    const fx oneShield = siegeDamage({Building::ShieldGenerator}, attacker, 2 * kHour);
    const fx twoShields = siegeDamage({Building::ShieldGenerator, Building::ShieldGenerator},
                                      attacker, 2 * kHour);

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
    const fx byLine = siegeDamage(shields, makeFleet({{Hull::Battleship, 5}}), 2 * kHour);
    const fx bySiege = siegeDamage(shields, makeFleet({{Hull::Monitor, 10}}), 2 * kHour);

    CHECK(bySiege > byLine);
}

TEST_CASE("гарнизон: оборона возвращается быстрее") {
    // Планета, пережившая осаду, без гарнизона остаётся уязвимой почти
    // сутки. Гарнизон — ответ на вопрос «как удержать фронт», отдельный
    // от вопроса «как выдержать удар», на который отвечает крепость.
    auto regained = [](std::initializer_list<Building> buildings) {
        Realm realm(0xD1FE, 60);
        registerEconomyComponents(realm.world);
        const uint32_t system = realm.systemWith(1);
        REQUIRE(system != UINT32_MAX);
        realm.setOwner(system, /*empire=*/1, fx::zero());
        realm.develop(system, 0, buildings);
        realm.run(5 * kHour);
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

TEST_CASE("сезон: стадия — чистая функция от игрового времени") {
    // Не «сервер решил, что пора»: и сервер, и клиент, и реплей считают
    // стадию из одного числа одинаково, и рассинхрону взяться неоткуда.
    //
    // Число это — ИГРОВЫЕ СЕКУНДЫ, а не тики. Тик при сжатии времени стоит
    // не десятую долю секунды, а сколько угодно; секунда означает одно и то
    // же всегда, и весь дизайн сезона записан именно в ней.
    SeasonConfig config;
    config.expansionSeconds = 100;
    config.conflictSeconds = 200;
    config.crisisSeconds = 50;
    config.finalSeconds = 25;

    CHECK(stageAt(config, 0) == SeasonStage::Expansion);
    CHECK(stageAt(config, 99) == SeasonStage::Expansion);
    CHECK(stageAt(config, 100) == SeasonStage::Conflict);
    CHECK(stageAt(config, 299) == SeasonStage::Conflict);
    CHECK(stageAt(config, 300) == SeasonStage::Crisis);
    CHECK(stageAt(config, 350) == SeasonStage::Final);
    // За краем сезона стадия не «сбрасывается» — Финал так и остаётся.
    CHECK(stageAt(config, 100000) == SeasonStage::Final);

    // Обратный отсчёт согласован со стадией.
    CHECK(secondsLeftInStage(config, 0) == 100);
    CHECK(secondsLeftInStage(config, 60) == 40);
    CHECK(secondsLeftInStage(config, 100) == 200);

    // Масштаб растягивает всё разом, не меняя порядка.
    config.scale = 10;
    CHECK(stageAt(config, 999) == SeasonStage::Expansion);
    CHECK(stageAt(config, 1000) == SeasonStage::Conflict);
    CHECK(config.totalSeconds() == 3750);
}

TEST_CASE("сезон: по умолчанию длится одиннадцать недель") {
    // УМОЛЧАНИЕ — ЭТО ТОЖЕ ДИЗАЙН. Здесь стояли два часа, потому что
    // столько удобно прогонять в CI, и под эти два часа незаметно
    // подстроились все остальные числа игры: осада в час, перелёт
    // в минуту, шахта за две. Сервер на ускорении ×20 доигрывал сезон
    // за десять минут, и это был не баг сервера, а баг умолчания.
    //
    // Диапазон 8–12 недель обещан в docs/01-GAME-DESIGN.md. Проверка
    // сторожит именно обещание, а не конкретное число: одиннадцать недель
    // можно поменять на девять, два часа — нельзя.
    const SeasonConfig config;
    const int64_t week = 7 * SeasonConfig::kDay;
    CHECK(config.totalSeconds() >= 8 * week);
    CHECK(config.totalSeconds() <= 12 * week);

    // Конфликт — самая длинная стадия: это собственно игра.
    CHECK(config.conflictSeconds > config.expansionSeconds);
    CHECK(config.conflictSeconds > config.crisisSeconds);
    CHECK(config.conflictSeconds > config.finalSeconds);
    // Кризис короче Конфликта: общий враг перестаёт быть событием,
    // если живёт дольше войны.
    CHECK(config.crisisSeconds < config.conflictSeconds);
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
    realm.run(2 * kHour);
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

    realm.run(2 * kHour);
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

    realm.run(2 * kHour);
    CHECK(realm.readinessOf(system) == kReadinessMax);
}

TEST_CASE("сезон: убежище Расширения не мешает колонизировать") {
    // Убежище запрещает ОСАДУ чужих планет возле чужих столиц. Высадку
    // на ничью планету оно запрещать не должно ни на какой стадии:
    // расширение — это то, ради чего стадия Расширения и существует,
    // и запретить его значило бы отменить саму стадию.
    //
    // Проверяется правилом, а не прогоном: колонизация не зависит
    // от стадии вовсе, и это ровно то, что здесь утверждается.
    const Fleet colonist = makeFleet({{Hull::Colonizer, 1}});
    CHECK(colonizeCheck(1, colonist, standingAt(7), kNoEmpire, 7) == ColonyRefusal::Ok);
}
