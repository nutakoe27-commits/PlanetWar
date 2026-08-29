#include "doctest.h"

#include <vector>

#include "pw/core/rng.h"
#include "pw/game/snapshot.h"

using namespace pw;
using namespace pw::game;

namespace {

/// Сервер и клиент с проводом между ними, но без транспорта: снапшот
/// проверяется отдельно от соединения, иначе непонятно, чей это дефект.
struct Link {
    SnapshotWriter server;
    SnapshotReader client;
    uint32_t systemCount;

    /// Потери и порядок задаются вызывающим — это и есть предмет проверки.
    std::vector<std::vector<uint8_t>> wire;

    explicit Link(uint32_t systems) : systemCount(systems) {
        server.reset(systems);
        client.reset(systems);
    }

    /// Собрать снапшот в пакет заданного размера.
    std::vector<uint8_t> build(const WorldView& current, size_t capacity = 1100) {
        std::vector<uint8_t> packet(capacity);
        ByteWriter writer(packet.data(), packet.size());
        server.write(writer, current);
        REQUIRE_FALSE(writer.overflowed());
        packet.resize(writer.size());
        return packet;
    }

    /// Доставить пакет и подтвердить его.
    void deliver(const std::vector<uint8_t>& packet, bool acknowledge = true) {
        ByteReader reader(packet.data(), packet.size());
        REQUIRE(client.apply(reader));
        if (acknowledge) server.acknowledge(client.lastSnapshotId());
    }

    /// Гонять снапшоты, пока клиент не догонит сервер.
    void settle(const WorldView& current, int rounds = 64, size_t capacity = 1100) {
        for (int i = 0; i < rounds; ++i) {
            deliver(build(current, capacity));
            if (same(current)) return;
        }
    }

    bool same(const WorldView& current) const {
        const WorldView& view = client.view();
        if (view.systems.size() != current.systems.size()) return false;
        for (size_t i = 0; i < view.systems.size(); ++i) {
            if (view.systems[i] != current.systems[i]) return false;
        }
        if (view.fleets.size() != current.fleets.size()) return false;
        for (const auto& [id, fleet] : current.fleets) {
            const auto found = view.fleets.find(id);
            if (found == view.fleets.end() || found->second != fleet) return false;
        }
        return view.empire.alloys.raw() == current.empire.alloys.raw();
    }
};

WorldView makeWorld(uint32_t systems, uint32_t fleets) {
    WorldView world;
    world.resize(systems);
    for (uint32_t i = 0; i < systems; ++i) {
        world.systems[i].owner = uint8_t(i % 5 == 0 ? 0 : 0xFF);
        world.systems[i].readiness = uint8_t(i % 101);
    }
    for (uint32_t i = 0; i < fleets; ++i) {
        FleetView fleet;
        fleet.id = i + 1;
        fleet.empire = uint8_t(i % 4);
        fleet.system = i % systems;
        fleet.nextSystem = fleet.system;
        fleet.composition = sim::makeFleet({{sim::Hull::Corvette, i + 1}, {sim::Hull::Destroyer, i % 3}, {sim::Hull::Cruiser, i % 2}});
        world.fleets[fleet.id] = fleet;
    }
    world.empire.alloys = fx::fromInt(1000);
    return world;
}

}  // namespace

// ---------------------------------------------------------------------------
// Основной путь
// ---------------------------------------------------------------------------

TEST_CASE("снапшот: клиент догоняет сервер") {
    Link link(50);
    const WorldView world = makeWorld(50, 10);
    link.settle(world);
    CHECK(link.same(world));
    CHECK(link.client.view().empire.alloys.raw() == fx::fromInt(1000).raw());
}

TEST_CASE("снапшот: в спокойной игре разница почти пуста") {
    // Ради этого дельты и нужны: бюджет 20 КБ/с на игрока. Полное
    // состояние двухсот систем не влезает даже в один пакет.
    Link link(200);
    WorldView world = makeWorld(200, 100);
    link.settle(world);
    REQUIRE(link.same(world));

    const std::vector<uint8_t> quiet = link.build(world);
    CHECK(quiet.size() < 32);   // ничего не менялось — почти пустой пакет
}

TEST_CASE("снапшот: полное состояние доезжает по частям") {
    // Двести систем и сто пятьдесят флотов не влезают в датаграмму.
    // Резать состояние пополам нельзя — клиент увидел бы половину мира
    // из прошлого кадра. Поэтому недосланное уезжает следующим снапшотом.
    Link link(200);
    const WorldView world = makeWorld(200, 150);

    int packets = 0;
    for (; packets < 64; ++packets) {
        const std::vector<uint8_t> packet = link.build(world);
        CHECK(packet.size() <= 1100);
        link.deliver(packet);
        if (link.same(world)) break;
    }
    CHECK(link.same(world));
    CHECK(packets > 0);   // за один пакет и не должно было влезть
}

TEST_CASE("снапшот: хвост карты тоже обновляется") {
    // Обход систем идёт по кругу от прошлой остановки. Без этого при
    // нехватке места мы бы каждый раз паковали одни и те же первые
    // системы, а дальний край карты не обновлялся бы никогда.
    Link link(400);
    WorldView world = makeWorld(400, 0);
    link.settle(world, /*rounds=*/128, /*capacity=*/300);
    REQUIRE(link.same(world));

    // Меняем самую последнюю систему и требуем, чтобы она доехала.
    world.systems[399].owner = 3;
    for (int i = 0; i < 32 && !link.same(world); ++i) link.deliver(link.build(world, 300));
    CHECK(link.client.view().systems[399].owner == 3);
}

// ---------------------------------------------------------------------------
// Потери
// ---------------------------------------------------------------------------

TEST_CASE("снапшот: потерянный не пересылается, но и не теряется навсегда") {
    // Снапшот едет ненадёжной частью пакета. База не сдвинулась — значит
    // следующий снапшот повторит недошедшее и добавит новое. Потеря стоит
    // одного кадра задержки, а не рассинхрона.
    Link link(30);
    WorldView world = makeWorld(30, 5);
    link.settle(world);
    REQUIRE(link.same(world));

    world.systems[7].owner = 2;

    // Строим снапшот и НЕ доставляем его.
    link.build(world);
    CHECK(link.client.view().systems[7].owner != 2);

    // Следующий доезжает и приносит потерянное изменение.
    link.deliver(link.build(world));
    CHECK(link.client.view().systems[7].owner == 2);
}

TEST_CASE("снапшот: половина пакетов теряется — состояние всё равно сходится") {
    Link link(120);
    WorldView world = makeWorld(120, 60);

    Rng rng(0x5A0F, /*stream=*/31);
    for (int round = 0; round < 400; ++round) {
        const std::vector<uint8_t> packet = link.build(world, 400);
        if (rng.next() % 100 < 50) link.deliver(packet);
        if (link.same(world)) break;
    }
    CHECK(link.same(world));
}

TEST_CASE("снапшот: устаревший применять нельзя") {
    // Переупорядоченный снапшот вернул бы мир в прошлое: флоты прыгнули бы
    // назад, а захваченная система снова стала бы чужой.
    Link link(20);
    WorldView world = makeWorld(20, 3);
    link.settle(world);

    world.systems[1].owner = 4;
    const std::vector<uint8_t> older = link.build(world);
    world.systems[1].owner = 5;
    const std::vector<uint8_t> newer = link.build(world);

    link.deliver(newer);
    CHECK(link.client.view().systems[1].owner == 5);

    link.deliver(older, /*acknowledge=*/false);
    CHECK(link.client.view().systems[1].owner == 5);   // не откатились
}

// ---------------------------------------------------------------------------
// Флоты
// ---------------------------------------------------------------------------

TEST_CASE("снапшот: исчезнувший флот исчезает и у клиента") {
    // Иначе клиент рисует призраков: погибший флот остаётся на карте,
    // и игрок отдаёт приказы тому, чего нет.
    Link link(20);
    WorldView world = makeWorld(20, 8);
    link.settle(world);
    REQUIRE(link.client.view().fleets.count(3) == 1);

    world.fleets.erase(3);
    link.settle(world);
    CHECK(link.client.view().fleets.count(3) == 0);
    CHECK(link.same(world));
}

TEST_CASE("снапшот: движение флота доезжает целиком") {
    Link link(20);
    WorldView world = makeWorld(20, 4);
    link.settle(world);

    for (int step = 1; step <= 10; ++step) {
        FleetView& fleet = world.fleets[1];
        fleet.system = 5;
        fleet.nextSystem = 6;
        fleet.progress = fx::fromFraction(step, 10);
        link.settle(world);

        const FleetView& seen = link.client.view().fleets.at(1);
        CHECK(seen.nextSystem == 6);
        CHECK(seen.progress.raw() == fx::fromFraction(step, 10).raw());
    }
}

TEST_CASE("снапшот: приказы отряда доезжают целиком") {
    // МАРШРУТ ОБЯЗАН ДОЕХАТЬ. Игрок задал план из четырёх точек и ушёл;
    // если снапшот везёт только текущую цель, вернувшись он увидит отряд,
    // идущий непонятно куда, и переотдаст приказ — то есть план, ради
    // которого всё и делалось, не переживёт даже одного захода.
    Link link(20);
    WorldView world = makeWorld(20, 4);

    FleetView& fleet = world.fleets[1];
    fleet.tag = 3;
    fleet.stance = uint8_t(sim::Stance::Patrol);
    fleet.evade = 1;
    fleet.anchor = 7;
    fleet.anchorOrbit = 2;
    fleet.routeStep = 1;
    fleet.routeCount = 4;
    fleet.route[0] = 5;
    fleet.route[1] = 9;
    fleet.route[2] = 12;
    fleet.route[3] = 7;
    link.settle(world);

    const FleetView& seen = link.client.view().fleets.at(1);
    CHECK(seen.tag == 3);
    CHECK(seen.stance == uint8_t(sim::Stance::Patrol));
    CHECK(seen.evade == 1);
    CHECK(seen.anchor == 7u);
    CHECK(seen.anchorOrbit == 2u);
    CHECK(seen.routeStep == 1);
    CHECK(seen.routeCount == 4);
    CHECK(seen.route[1] == 9u);
    CHECK(seen.routeTarget() == 9u);
}

TEST_CASE("снапшот: смена маршрута считается изменением") {
    // Без сравнения приказов снапшот считал бы отряд неизменившимся
    // после смены плана, и игрок видел бы старый маршрут до первого боя.
    // Тот же класс ошибки, из-за которого четыре корпуса из восьми
    // однажды не ездили по сети вовсе.
    Link link(20);
    WorldView world = makeWorld(20, 4);
    world.fleets[1].routeCount = 1;
    world.fleets[1].route[0] = 5;
    link.settle(world);
    REQUIRE(link.client.view().fleets.at(1).routeTarget() == 5u);

    world.fleets[1].route[0] = 11;
    link.settle(world);
    CHECK(link.client.view().fleets.at(1).routeTarget() == 11u);

    // И стойка тоже: она решает, что отряд делает ночью.
    world.fleets[1].stance = uint8_t(sim::Stance::Guard);
    link.settle(world);
    CHECK(link.client.view().fleets.at(1).stance == uint8_t(sim::Stance::Guard));
}

TEST_CASE("снапшот: чужой маршрут в пакет не попадает") {
    // Видеть, куда идёт сосед, и встречать его заранее означало бы
    // выиграть войну, ни разу не выйдя в космос. Сервер таких полей
    // не заполняет вовсе — проверка сторожит сам формат: пустые приказы
    // обязаны и читаться пустыми.
    Link link(20);
    WorldView world = makeWorld(20, 4);
    FleetView& stranger = world.fleets[1];
    stranger.empire = 3;
    stranger.tag = 0;
    stranger.anchor = sim::kNoSystem;
    stranger.routeCount = 0;
    link.settle(world);

    const FleetView& seen = link.client.view().fleets.at(1);
    CHECK(seen.routeCount == 0);
    CHECK(seen.routeTarget() == sim::kNoSystem);
    CHECK(seen.anchor == sim::kNoSystem);
}

// ---------------------------------------------------------------------------
// Битые пакеты
// ---------------------------------------------------------------------------

TEST_CASE("снапшот: битый пакет не оставляет мир наполовину обновлённым") {
    // Половина полей из этого кадра, половина из прошлого — худшее,
    // что может случиться с состоянием. Поэтому применяем только после
    // того, как разобрали всё.
    Link link(40);
    WorldView world = makeWorld(40, 10);
    link.settle(world);
    const WorldView before = link.client.view();

    world.systems[3].owner = 9;
    world.fleets[1].system = 17;
    std::vector<uint8_t> packet = link.build(world);
    packet.resize(packet.size() / 2);   // обрезали в пути

    ByteReader reader(packet.data(), packet.size());
    link.client.apply(reader);

    CHECK(link.client.view().systems[3].owner == before.systems[3].owner);
    CHECK(link.client.view().fleets.at(1).system == before.fleets.at(1).system);
}

TEST_CASE("снапшот: мусор не роняет клиента") {
    Rng rng(0xBADF00D, /*stream=*/32);
    for (int attempt = 0; attempt < 5000; ++attempt) {
        SnapshotReader client;
        client.reset(64);

        uint8_t noise[256];
        const size_t size = size_t(rng.next() % sizeof(noise));
        for (size_t i = 0; i < size; ++i) noise[i] = uint8_t(rng.next());

        ByteReader reader(noise, size);
        client.apply(reader);
        CHECK(client.view().systems.size() == 64);
    }
}

TEST_CASE("снапшот: заявленное количество больше пакета не выделяет память") {
    uint8_t buffer[64];
    ByteWriter writer(buffer, sizeof(buffer));
    writer.varint(1);            // номер снапшота
    writer.varint(0);            // тик
    writer.boolean(false);       // ресурсы не менялись
    writer.varint(0xFFFFFFFF);   // «дальше четыре миллиарда исчезнувших флотов»

    SnapshotReader client;
    client.reset(16);
    ByteReader reader(buffer, writer.size());
    CHECK_FALSE(client.apply(reader));
}
