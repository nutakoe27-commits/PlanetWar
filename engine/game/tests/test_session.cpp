#include "doctest.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pw/core/rng.h"
#include "pw/game/client.h"
#include "pw/game/server.h"

using namespace pw;
using namespace pw::game;

namespace {

/// Сессия: сервер, клиенты и провод между ними — целиком в памяти.
///
/// Настоящие сокеты здесь не нужны и вредны. Они не умеют терять пакеты
/// по команде, а именно потери и надо проверять; и они заставляют ждать
/// настоящее время, а здесь полчаса игры прокручиваются за секунду.
class Session {
public:
    Session(uint32_t systems, uint32_t lossPercent = 0, uint32_t maxDelay = 0,
            uint64_t seed = 0x5E5510)
        : rng_(seed, /*stream=*/41), loss_(lossPercent), delay_(maxDelay) {
        ServerConfig config;
        config.galaxy.seed = 0xC0FFEE;
        config.galaxy.systemCount = systems;
        config.maxPlayers = 8;
        server.start(config);
    }

    /// Добавить клиента. Возвращает его номер.
    size_t addClient(const std::string& name) {
        clients.push_back(std::make_unique<Client>());
        addresses.push_back(net::Address::loopback(uint16_t(20000 + clients.size())));
        clients.back()->connect(serverAddress, name, now);
        return clients.size() - 1;
    }

    /// Прокрутить заданное число миллисекунд.
    void run(int64_t milliseconds) {
        const int64_t until = now + milliseconds;
        while (now < until) step();
    }

    Server server;
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<net::Address> addresses;
    net::Address serverAddress = net::Address::loopback(9000);
    int64_t now = 0;

private:
    struct Packet {
        bool toServer;
        size_t client;
        std::vector<uint8_t> data;
        int64_t at;
    };

    void put(bool toServer, size_t client, const uint8_t* data, size_t size) {
        if (rng_.next() % 100 < loss_) return;
        Packet packet;
        packet.toServer = toServer;
        packet.client = client;
        packet.data.assign(data, data + size);
        packet.at = now + int64_t(delay_ == 0 ? 0 : rng_.next() % delay_);
        wire_.push_back(std::move(packet));
    }

    void step() {
        ++now;

        // Доставка.
        for (auto it = wire_.begin(); it != wire_.end();) {
            if (it->at > now) { ++it; continue; }
            if (it->toServer) {
                server.receive(addresses[it->client], it->data.data(), it->data.size(), now);
            } else {
                clients[it->client]->receive(it->data.data(), it->data.size(), now);
            }
            it = wire_.erase(it);
        }

        // Сервер.
        std::vector<OutgoingPacket> outgoing;
        server.update(now, outgoing);
        for (const OutgoingPacket& packet : outgoing) {
            for (size_t i = 0; i < addresses.size(); ++i) {
                if (addresses[i] != packet.to) continue;
                put(/*toServer=*/false, i, packet.data.data(), packet.data.size());
                break;
            }
        }

        // Клиенты.
        for (size_t i = 0; i < clients.size(); ++i) {
            uint8_t buffer[net::kMaxPacketSize];
            const size_t size = clients[i]->update(now, buffer, sizeof(buffer));
            if (size > 0) put(/*toServer=*/true, i, buffer, size);
        }
    }

    Rng rng_;
    uint32_t loss_;
    uint32_t delay_;
    std::vector<Packet> wire_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Подключение
// ---------------------------------------------------------------------------

TEST_CASE("сессия: игрок подключается и получает свою галактику") {
    Session session(80);
    session.addClient("Михаил");
    session.run(500);

    Client& client = *session.clients[0];
    REQUIRE(client.state() == net::ConnectionState::Connected);
    REQUIRE(client.ready());

    // Галактика построена у клиента САМОСТОЯТЕЛЬНО, из сида. По сети
    // приехало три десятка байт вместо всей геометрии.
    CHECK(client.galaxy().systemCount() == session.server.galaxy().systemCount());
    CHECK(client.galaxy().hash() == session.server.galaxy().hash());
    CHECK(client.capital() < client.galaxy().systemCount());
}

TEST_CASE("сессия: четверо играют одновременно") {
    Session session(200);
    for (int i = 0; i < 4; ++i) session.addClient("игрок " + std::to_string(i + 1));
    session.run(1000);

    CHECK(session.server.playerCount() == 4);
    for (auto& client : session.clients) {
        CHECK(client->state() == net::ConnectionState::Connected);
        CHECK(client->ready());
    }

    // Столицы разнесены: соседний старт означал бы встречу на второй
    // минуте, а у проигравшего не было бы ни одного осмысленного хода.
    for (size_t a = 0; a < session.clients.size(); ++a) {
        for (size_t b = a + 1; b < session.clients.size(); ++b) {
            CHECK(session.clients[a]->capital() != session.clients[b]->capital());
        }
    }
}

TEST_CASE("сессия: игрок видит свою столицу своей") {
    Session session(80);
    session.addClient("Михаил");
    session.run(1500);

    Client& client = *session.clients[0];
    REQUIRE(client.ready());
    const auto& view = client.view();
    REQUIRE(client.capital() < view.systems.size());
    CHECK(view.systems[client.capital()].owner == uint8_t(client.empire()));
}

TEST_CASE("сессия: игрок видит свой стартовый флот") {
    Session session(80);
    session.addClient("Михаил");
    session.run(1500);

    Client& client = *session.clients[0];
    const auto own = client.fleetsAt(client.capital());
    REQUIRE(own.size() == 1);
    const FleetView& fleet = client.view().fleets.at(own.front());
    CHECK(fleet.composition.corvettes == 8);
    CHECK(fleet.composition.destroyers == 2);
}

// ---------------------------------------------------------------------------
// Приказы
// ---------------------------------------------------------------------------

TEST_CASE("сессия: флот идёт туда, куда приказано") {
    Session session(120);
    session.addClient("Михаил");
    session.run(1500);

    Client& client = *session.clients[0];
    const uint32_t fleet = client.fleetsAt(client.capital()).front();

    // Ищем соседнюю систему: туда флот дойдёт за разумное время.
    const uint32_t home = client.capital();
    REQUIRE(client.galaxy().neighborCount(home) > 0);
    const uint32_t target = client.galaxy().neighbors(home)[0];

    REQUIRE(client.orderMove(fleet, target));
    session.run(60000);   // минута игры

    const FleetView& seen = client.view().fleets.at(fleet);
    CHECK(seen.system == target);
    CHECK(seen.nextSystem == target);
}

TEST_CASE("сессия: чужим флотом покомандовать нельзя") {
    // Самый дешёвый чит из возможных: прислать номер чужой сущности.
    // Сервер обязан проверять принадлежность, потому что клиент — это
    // код на чужой машине, и доверять ему нельзя ни в чём.
    Session session(200);
    session.addClient("свой");
    session.addClient("чужой");
    session.run(1500);

    Client& mine = *session.clients[0];
    Client& other = *session.clients[1];
    const uint32_t victim = other.fleetsAt(other.capital()).front();
    const uint32_t before = other.view().fleets.at(victim).system;

    REQUIRE(mine.orderMove(victim, mine.capital()));
    session.run(30000);

    CHECK(other.view().fleets.at(victim).system == before);
    CHECK(session.server.rejectedOrders() > 0);
}

TEST_CASE("сессия: приказ в несуществующую систему отвергается с уведомлением") {
    Session session(80);
    session.addClient("Михаил");
    session.run(1500);

    Client& client = *session.clients[0];
    const uint32_t fleet = client.fleetsAt(client.capital()).front();
    REQUIRE(client.orderMove(fleet, 999999));
    session.run(1000);

    const auto events = client.takeEvents();
    REQUIRE_FALSE(events.empty());
    CHECK(events.front().kind == NoticeKind::OrderRejected);
}

TEST_CASE("сессия: чужую систему застроить нельзя") {
    Session session(200);
    session.addClient("свой");
    session.addClient("чужой");
    session.run(1500);

    const uint64_t before = session.server.rejectedOrders();
    REQUIRE(session.clients[0]->orderBuildShip(session.clients[1]->capital(),
                                               sim::Hull::Battleship));
    session.run(1000);
    CHECK(session.server.rejectedOrders() > before);
}

// ---------------------------------------------------------------------------
// Захват — то, ради чего всё это
// ---------------------------------------------------------------------------

TEST_CASE("сессия: игрок захватывает ничью систему") {
    // Это и есть определение играбельности из дорожной карты: игрок
    // подключается, видит галактику, отправляет флот и получает планету.
    Session session(150);
    session.addClient("Михаил");
    session.run(1500);

    Client& client = *session.clients[0];
    REQUIRE(client.ready());

    const uint32_t home = client.capital();
    REQUIRE(client.galaxy().neighborCount(home) > 0);
    const uint32_t target = client.galaxy().neighbors(home)[0];
    REQUIRE(client.view().systems[target].owner != uint8_t(client.empire()));

    const uint32_t fleet = client.fleetsAt(home).front();
    REQUIRE(client.orderMove(fleet, target));

    // Осада ничьей системы длится kClaimSeconds; даём с запасом.
    const int64_t needed = (sim::kClaimSeconds + 120) * 1000;
    session.run(needed);

    CHECK(client.view().systems[target].owner == uint8_t(client.empire()));
}

TEST_CASE("сессия: игрок видит планеты своей столицы") {
    Session session(100);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    const auto planets = client.planetsAt(client.capital());
    REQUIRE_FALSE(planets.empty());

    // Геометрия планет пришла НЕ по сети: клиент вывел её из сида той же
    // функцией, что и сервер. По проводу приехала только застройка.
    for (const auto& planet : planets) {
        CHECK(planet.system == client.capital());
        CHECK(planet.slots > 0);
        CHECK(planet.freeSlots() == planet.slots);   // ещё ничего не построено
    }
}

TEST_CASE("сессия: игрок строит шахту, и минералы начинают расти") {
    // Полный игровой цикл: клиент видит планету, отдаёт приказ, сервер
    // проверяет права, строит, экономика считает, снапшот привозит
    // и застройку, и выросшие ресурсы.
    Session session(100);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    const auto planets = client.planetsAt(client.capital());
    REQUIRE_FALSE(planets.empty());
    const uint32_t planet = planets.front().id;

    const fx mineralsBefore = client.view().empire.minerals;
    REQUIRE(client.orderBuildBuilding(planet, 0, sim::Building::Mine));
    session.run(60000);   // минута игры

    // Постройка видна игроку.
    const auto after = client.planetsAt(client.capital());
    REQUIRE_FALSE(after.empty());
    CHECK(after.front().buildings[0] == uint8_t(sim::Building::Mine));
    // И она работает.
    CHECK(client.view().empire.minerals > mineralsBefore);
}

TEST_CASE("сессия: цепочка шахта-литейная даёт сплавы") {
    // Сплавы — единственный ресурс для флота, и получить их можно только
    // переработкой минералов. Проверяем всю цепочку, а не отдельное здание.
    Session session(100);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    const auto planets = client.planetsAt(client.capital());
    REQUIRE(planets.front().slots >= 3);
    const uint32_t planet = planets.front().id;

    REQUIRE(client.orderBuildBuilding(planet, 0, sim::Building::Mine));
    REQUIRE(client.orderBuildBuilding(planet, 1, sim::Building::Mine));
    REQUIRE(client.orderBuildBuilding(planet, 2, sim::Building::Foundry));
    session.run(2000);

    const fx alloysBefore = client.view().empire.alloys;
    session.run(120000);   // две минуты игры
    CHECK(client.view().empire.alloys > alloysBefore);
}

TEST_CASE("сессия: чужую планету застроить нельзя") {
    Session session(200);
    session.addClient("свой");
    session.addClient("чужой");
    session.run(2000);

    const auto victim = session.clients[1]->planetsAt(session.clients[1]->capital());
    REQUIRE_FALSE(victim.empty());

    const uint64_t before = session.server.rejectedOrders();
    REQUIRE(session.clients[0]->orderBuildBuilding(victim.front().id, 0,
                                                   sim::Building::Fortress));
    session.run(2000);

    CHECK(session.server.rejectedOrders() > before);
    const auto after = session.clients[1]->planetsAt(session.clients[1]->capital());
    CHECK(after.front().buildings[0] == uint8_t(sim::Building::None));
}

// ---------------------------------------------------------------------------
// Плохая сеть
// ---------------------------------------------------------------------------

TEST_CASE("сессия: игра идёт через канал с четвертью потерь") {
    Session session(150, /*loss=*/25, /*delay=*/60);
    session.addClient("Михаил");
    session.run(5000);

    Client& client = *session.clients[0];
    REQUIRE(client.state() == net::ConnectionState::Connected);
    REQUIRE(client.ready());

    const uint32_t home = client.capital();
    const uint32_t target = client.galaxy().neighbors(home)[0];
    const uint32_t fleet = client.fleetsAt(home).front();
    REQUIRE(client.orderMove(fleet, target));

    session.run((sim::kClaimSeconds + 180) * 1000);

    // Приказ дошёл, флот дошёл, система захвачена — несмотря на то, что
    // каждый четвёртый пакет не доехал.
    CHECK(client.view().systems[target].owner == uint8_t(client.empire()));
    CHECK(client.lossPercent() > 5);
}

TEST_CASE("сессия: пропавший клиент отваливается по таймауту") {
    Session session(80);
    session.addClient("Михаил");
    session.run(1000);
    REQUIRE(session.server.playerCount() == 1);

    // Клиент замолчал: крутим только сервер.
    for (int64_t step = 0; step < net::kTimeoutMilliseconds + 500; ++step) {
        std::vector<OutgoingPacket> outgoing;
        session.server.update(session.now + step, outgoing);
    }
    CHECK(session.server.playerCount() == 0);
}

TEST_CASE("сессия: сервер отказывает, когда мест нет") {
    Session session(120);
    ServerConfig config;
    config.galaxy.systemCount = 120;
    config.maxPlayers = 2;
    session.server.start(config);

    for (int i = 0; i < 3; ++i) session.addClient("игрок " + std::to_string(i));
    session.run(2000);

    CHECK(session.server.playerCount() == 2);
    CHECK(session.clients[2]->state() == net::ConnectionState::Failed);
    CHECK(session.clients[2]->rejectReason() == net::RejectReason::ServerFull);
}

// ---------------------------------------------------------------------------
// Уведомления
//
// Игрок обязан узнавать о событии сам, а не замечать изменение на карте:
// в MMO он часто смотрит в другую её часть, а то и вовсе не смотрит.
// ---------------------------------------------------------------------------

TEST_CASE("уведомления: захват системы доезжает до игрока") {
    Session session(150);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    client.takeEvents();   // отбрасываем всё, что накопилось при входе

    const uint32_t home = client.capital();
    const uint32_t target = client.galaxy().neighbors(home)[0];
    const uint32_t fleet = client.fleetsAt(home).front();
    REQUIRE(client.orderMove(fleet, target));

    session.run((sim::kClaimSeconds + 180) * 1000);
    REQUIRE(client.view().systems[target].owner == uint8_t(client.empire()));

    bool told = false;
    for (const ClientEvent& event : client.takeEvents()) {
        if (event.kind == NoticeKind::SystemCaptured && event.system == target) told = true;
    }
    CHECK(told);
}

TEST_CASE("уведомления: о своей столице при входе не сообщают") {
    // Иначе игрок при подключении получал бы «система захвачена»
    // о том, что и так его.
    Session session(120);
    session.addClient("Михаил");
    session.run(3000);

    for (const ClientEvent& event : session.clients[0]->takeEvents()) {
        CHECK(event.kind != NoticeKind::SystemCaptured);
    }
}

TEST_CASE("уведомления: слияние флотов не считается потерей") {
    // Флот исчезает и от слияния, и от гибели. Путать их нельзя:
    // ложное «флот уничтожен» страшнее пропущенного.
    Session session(120);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    client.takeEvents();

    // Заказываем корабли: построенные отряды сольются со стартовым.
    REQUIRE(client.orderBuildShip(client.capital(), sim::Hull::Corvette, 3));
    session.run(120000);

    for (const ClientEvent& event : client.takeEvents()) {
        CHECK(event.kind != NoticeKind::FleetDestroyed);
    }
}
