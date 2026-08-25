#include "doctest.h"

#include "pw/sim/control.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

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

    void setOwner(uint32_t system, uint32_t empire, fx readiness) {
        world.get<Owner>(systemAt(system))->empire = empire;
        world.get<SystemDefense>(systemAt(system))->readiness = readiness;
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
            systemPresence(world, context);
            systemSiege(world, context);
        }
    }

    uint32_t ownerOf(uint32_t system) { return world.get<Owner>(systemAt(system))->empire; }
    fx readinessOf(uint32_t system) {
        return world.get<SystemDefense>(systemAt(system))->readiness;
    }
};

constexpr int64_t kSecond = kTicksPerSecond;
constexpr int64_t kMinute = 60 * kSecond;

}  // namespace

// ---------------------------------------------------------------------------
// Занятие ничьих систем
// ---------------------------------------------------------------------------

TEST_CASE("владение: галактика начинается ничьей") {
    Realm realm;
    for (uint32_t i = 0; i < realm.galaxy.systemCount(); ++i) {
        CHECK(realm.ownerOf(i) == kNoEmpire);
        CHECK(realm.readinessOf(i) == fx::zero());
    }
}

TEST_CASE("владение: флот занимает ничью систему за пять минут") {
    Realm realm;
    realm.garrison(3, /*empire=*/1, Fleet{2, 0, 0, 0});

    realm.run(4 * kMinute + 50 * kSecond);
    CHECK(realm.ownerOf(3) == kNoEmpire);  // ещё рано

    realm.run(20 * kSecond);
    CHECK(realm.ownerOf(3) == 1u);
    // Занятая ничья система сразу боеспособна: воюют не за пустоту.
    CHECK(realm.readinessOf(3) == kReadinessMax);
}

TEST_CASE("владение: два претендента не занимают ничего") {
    Realm realm;
    realm.garrison(5, /*empire=*/1, Fleet{5, 0, 0, 0});
    realm.garrison(5, /*empire=*/2, Fleet{5, 0, 0, 0});

    realm.run(20 * kMinute);
    // Пока не разобрались между собой, система остаётся ничьей.
    CHECK(realm.ownerOf(5) == kNoEmpire);
}

TEST_CASE("владение: ушедший флот не дозанимает систему") {
    Realm realm;
    const Entity fleet = realm.garrison(7, /*empire=*/1, Fleet{3, 0, 0, 0});

    realm.run(3 * kMinute);
    // Флот отправился дальше — счётчик занятия обязан обнулиться.
    realm.world.get<FleetLocation>(fleet)->nextSystem =
        realm.galaxy.neighbors(7)[0];

    realm.run(4 * kMinute);
    CHECK(realm.ownerOf(7) == kNoEmpire);
}

// ---------------------------------------------------------------------------
// Осада
// ---------------------------------------------------------------------------

TEST_CASE("осада: чужая система падает за десятки минут, а не мгновенно") {
    Realm realm;
    realm.setOwner(9, /*empire=*/1, kReadinessMax);
    realm.garrison(9, /*empire=*/2, Fleet{20, 0, 0, 0});  // тоннаж 20

    // Через минуту система ещё держится. Это и есть главное свойство:
    // мгновенных потерь в игре нет.
    realm.run(1 * kMinute);
    CHECK(realm.ownerOf(9) == 1u);
    CHECK(realm.readinessOf(9) < kReadinessMax);

    realm.run(29 * kMinute);
    CHECK(realm.ownerOf(9) == 1u);  // и через полчаса тоже

    realm.run(45 * kMinute);
    CHECK(realm.ownerOf(9) == 2u);
}

TEST_CASE("осада: укладывается в обещанные дизайном 30–90 минут") {
    auto minutesToFall = [](uint32_t tonnage) {
        Realm realm;
        realm.setOwner(11, /*empire=*/1, kReadinessMax);
        // Тоннаж набираем корветами: один корвет — одна единица.
        realm.garrison(11, /*empire=*/2, Fleet{tonnage, 0, 0, 0});

        for (int64_t minute = 1; minute <= 240; ++minute) {
            realm.run(kMinute);
            if (realm.ownerOf(11) == 2u) return minute;
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

    // Втрое больший флот НЕ берёт систему втрое быстрее: иначе выигрывал бы
    // просто тот, кто собрал всё в один кулак, и делить силы стало бы незачем.
    CHECK(double(small) / double(large) < 2.5);
}

TEST_CASE("осада: защитники срывают её немедленно") {
    Realm realm;
    realm.setOwner(13, /*empire=*/1, kReadinessMax);
    realm.garrison(13, /*empire=*/2, Fleet{50, 0, 0, 0});

    realm.run(10 * kMinute);
    const fx damaged = realm.readinessOf(13);
    CHECK(damaged < kReadinessMax);

    // Деблокирующий удар: пришли свои — осада снята.
    realm.garrison(13, /*empire=*/1, Fleet{1, 0, 0, 0});
    realm.run(1 * kSecond);
    CHECK(realm.world.get<SiegeState>(realm.systemAt(13))->besieger == kNoEmpire);

    // И оборона начинает восстанавливаться.
    realm.run(10 * kMinute);
    CHECK(realm.readinessOf(13) > damaged);
    CHECK(realm.ownerOf(13) == 1u);
}

TEST_CASE("осада: свежий захват слаб") {
    Realm realm;
    realm.setOwner(15, /*empire=*/1, kReadinessMax);
    realm.garrison(15, /*empire=*/2, Fleet{0, 0, 0, 30});  // тяжёлый флот

    for (int64_t minute = 0; minute < 200 && realm.ownerOf(15) != 2u; ++minute) {
        realm.run(kMinute);
    }
    REQUIRE(realm.ownerOf(15) == 2u);

    // Взятая система достаётся четвертью готовности: отбить её обратно
    // реально, и фронт может ходить, а не застывать после первого удара.
    CHECK(realm.readinessOf(15) <= kReadinessMax * kCaptureReadinessShare + fx::one());
    CHECK(realm.readinessOf(15) > fx::zero());
}

TEST_CASE("осада: оборона восстанавливается в мирное время") {
    Realm realm;
    realm.setOwner(17, /*empire=*/1, fx::zero());

    realm.run(50 * kMinute);
    const fx half = realm.readinessOf(17);
    CHECK(half > fx::zero());
    CHECK(half < kReadinessMax);

    realm.run(60 * kMinute);
    // Полное восстановление около ста минут: система, пережившая осаду,
    // ещё долго остаётся уязвимой.
    CHECK(realm.readinessOf(17) == kReadinessMax);
}

TEST_CASE("осада: флот в пути не участвует ни в чём") {
    Realm realm;
    realm.setOwner(19, /*empire=*/1, kReadinessMax);

    const Entity raider = realm.garrison(19, /*empire=*/2, Fleet{50, 0, 0, 0});
    // Флот вышел из системы: он между узлами и осаждать не может.
    realm.world.get<FleetLocation>(raider)->nextSystem = realm.galaxy.neighbors(19)[0];

    realm.run(30 * kMinute);
    CHECK(realm.readinessOf(19) == kReadinessMax);
    CHECK(realm.ownerOf(19) == 1u);
}

TEST_CASE("осада: смена осаждающего обнуляет счётчик") {
    Realm realm;
    realm.setOwner(21, /*empire=*/1, kReadinessMax);
    const Entity first = realm.garrison(21, /*empire=*/2, Fleet{40, 0, 0, 0});

    realm.run(5 * kMinute);
    CHECK(realm.world.get<SiegeState>(realm.systemAt(21))->besieger == 2u);
    const uint32_t ticksBefore = realm.world.get<SiegeState>(realm.systemAt(21))->ticks;
    CHECK(ticksBefore > 0);

    // Первый ушёл, пришёл третий — счёт осады начинается заново, но
    // повреждения обороны остаются: они уже нанесены.
    realm.world.get<FleetLocation>(first)->nextSystem = realm.galaxy.neighbors(21)[0];
    realm.garrison(21, /*empire=*/3, Fleet{40, 0, 0, 0});
    realm.run(1 * kSecond);

    const SiegeState* siege = realm.world.get<SiegeState>(realm.systemAt(21));
    CHECK(siege->besieger == 3u);
    CHECK(siege->ticks < ticksBefore);
    CHECK(realm.readinessOf(21) < kReadinessMax);
}

TEST_CASE("владение: воспроизводится тик в тик") {
    Realm first(0xE55, 60), second(0xE55, 60);

    for (Realm* realm : {&first, &second}) {
        realm->setOwner(4, 1, kReadinessMax);
        realm->setOwner(8, 2, kReadinessMax);
        realm->garrison(4, 2, Fleet{15, 3, 1, 0});
        realm->garrison(8, 1, Fleet{9, 2, 0, 1});
        realm->garrison(12, 3, Fleet{5, 0, 0, 0});
    }
    REQUIRE(first.world.hash() == second.world.hash());

    first.run(80 * kMinute);
    second.run(80 * kMinute);
    CHECK(first.world.hash() == second.world.hash());

    // И что-то действительно произошло, а не просто ничего не менялось.
    CHECK(first.ownerOf(12) == 3u);
}
