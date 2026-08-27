#include "doctest.h"

#include <algorithm>
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
            uint64_t seed = 0x5E5510, uint32_t speed = 1)
        : rng_(seed, /*stream=*/41), loss_(lossPercent), delay_(maxDelay) {
        ServerConfig config;
        config.galaxy.seed = 0xC0FFEE;
        config.galaxy.systemCount = systems;
        config.maxPlayers = 8;
        // Ускорение мира. Стройки и осады идут минутами, и гонять их
        // в реальном темпе значит держать в наборе тесты по полминуты
        // каждый. Сеть при этом остаётся настоящей: пакеты ходят с той же
        // частотой, ускоряется только симуляция.
        config.speed = speed;
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
    CHECK(fleet.composition[sim::Hull::Corvette] == 8);
    CHECK(fleet.composition[sim::Hull::Destroyer] == 2);
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

    // Ждём прибытия ЦИКЛОМ, а не «минуту игры».
    //
    // Раньше здесь стояла ровно минута, и её хватало: стартовый флот шёл
    // со скоростью эсминца. Теперь в нём есть колонизатор, скорость флота
    // задаёт самый медленный корабль, и минуты стало мало. Тест упал
    // на верном поведении — просто потому, что знал скорость наизусть.
    //
    // Цикл не знает ни скоростей, ни длин линий: он ждёт события,
    // а не отсчитывает время. Такой тест переживёт и следующую правку
    // баланса.
    for (int round = 0; round < 60; ++round) {
        session.run(10000);
        const FleetView& moving = client.view().fleets.at(fleet);
        if (moving.system == target && moving.nextSystem == target) break;
    }

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


/// Довести флот до системы и высадить колонию на её первую ничью планету.
///
/// Общий помощник, потому что этот путь проверяется четырьмя тестами
/// подряд, и каждый обязан идти ЧЕРЕЗ НАСТОЯЩИЙ ПРОТОКОЛ: приказ на
/// движение, ожидание прибытия, приказ на высадку. Проверка, срезающая
/// дорогу через мир сервера, не доказала бы ничего про игру.
///
/// Возвращает ложь, если колонизировать не удалось: у вызывающего должна
/// быть возможность сказать «не доехало», а не зависнуть.
bool colonizeFirstNeutral(Session& session, Client& client, uint32_t fleet,
                          uint32_t target) {
    if (!client.orderMove(fleet, target)) return false;

    // Ждём прибытия. Колонизатор медленный, поэтому срок щедрый.
    for (int round = 0; round < 120; ++round) {
        session.run(5000);
        const auto standing = client.fleetsAt(target);
        if (standing.empty()) continue;

        for (const auto& planet : client.planetsAt(target)) {
            if (planet.owner != 0xFF) continue;
            if (!client.orderColonize(standing.front(), planet.id)) return false;
            session.run(3000);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Расширение — то, ради чего всё это
// ---------------------------------------------------------------------------

TEST_CASE("сессия: игрок колонизирует ничью систему") {
    // Это и есть определение играбельности из дорожной карты: игрок
    // подключается, видит галактику, отправляет флот и получает планету.
    //
    // Путь изменился: раньше хватало привести любой флот и подождать,
    // теперь нужен колонизатор и приказ на высадку. Проверка стала длиннее
    // ровно настолько, насколько длиннее стала сама игра.
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
    REQUIRE(colonizeFirstNeutral(session, client, fleet, target));

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

TEST_CASE("сессия: шахта строится минуты, и только потом даёт минералы") {
    // Полный игровой цикл: клиент видит планету, отдаёт приказ, сервер
    // проверяет права, ВЕДЁТ СТРОЙКУ, экономика считает, снапшот привозит
    // и ход стройки, и готовое здание, и выросшие ресурсы.
    Session session(100, 0, 0, 0x5E5510, /*speed=*/8);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    const auto planets = client.planetsAt(client.capital());
    REQUIRE_FALSE(planets.empty());
    const uint32_t planet = planets.front().id;

    REQUIRE(client.orderBuildBuilding(planet, 0, sim::Building::Mine));
    session.run(1000);

    // Стройка видна игроку сразу — а здания ещё нет.
    {
        const auto during = client.planetsAt(client.capital());
        REQUIRE_FALSE(during.empty());
        CHECK(during.front().buildings[0] == uint8_t(sim::Building::None));
        CHECK(during.front().buildSlot == 0);
        CHECK(during.front().buildBuilding == uint8_t(sim::Building::Mine));
        // Занятый стройкой слот больше не считается свободным.
        CHECK(during.front().freeSlots() == during.front().slots - 1);
    }

    // Шахта стоит 60 минералов при темпе 0.5 в секунду — две минуты игры.
    session.run(20000);   // 160 секунд игры при ускорении восемь

    const auto after = client.planetsAt(client.capital());
    REQUIRE_FALSE(after.empty());
    CHECK(after.front().buildings[0] == uint8_t(sim::Building::Mine));
    CHECK_FALSE(after.front().building());

    // И она работает.
    const fx mineralsBefore = client.view().empire.minerals;
    session.run(5000);
    CHECK(client.view().empire.minerals > mineralsBefore);
}

TEST_CASE("сессия: цепочка шахта-литейная даёт сплавы") {
    // Сплавы — единственный ресурс для флота, и получить их можно только
    // переработкой минералов. Проверяем всю цепочку, а не отдельное здание.
    //
    // Три заказа подряд — это ещё и проверка очереди: без неё второй щелчок
    // отменял бы первый, и игрок получил бы одну литейную вместо цепочки.
    Session session(100, 0, 0, 0x5E5510, /*speed=*/16);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    const auto planets = client.planetsAt(client.capital());
    REQUIRE(planets.front().slots >= 3);
    const uint32_t planet = planets.front().id;

    REQUIRE(client.orderBuildBuilding(planet, 0, sim::Building::Mine));
    REQUIRE(client.orderBuildBuilding(planet, 1, sim::Building::Mine));
    REQUIRE(client.orderBuildBuilding(planet, 2, sim::Building::Foundry));
    session.run(1000);
    CHECK(client.planetsAt(client.capital()).front().buildQueued == 2);

    // 60 + 60 + 90 минералов при темпе 0.5 в секунду — семь минут игры.
    session.run(35000);
    const auto after = client.planetsAt(client.capital());
    CHECK(after.front().buildings[0] == uint8_t(sim::Building::Mine));
    CHECK(after.front().buildings[1] == uint8_t(sim::Building::Mine));
    CHECK(after.front().buildings[2] == uint8_t(sim::Building::Foundry));

    const fx alloysBefore = client.view().empire.alloys;
    session.run(10000);
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
    REQUIRE(colonizeFirstNeutral(session, client, fleet, target));

    // Оба приказа дошли, флот дошёл, колония стоит — несмотря на то, что
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

TEST_CASE("уведомления: основание колонии доезжает до игрока") {
    Session session(150);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    client.takeEvents();   // отбрасываем всё, что накопилось при входе

    const uint32_t home = client.capital();
    const uint32_t target = client.galaxy().neighbors(home)[0];
    const uint32_t fleet = client.fleetsAt(home).front();
    REQUIRE(colonizeFirstNeutral(session, client, fleet, target));
    REQUIRE(client.view().systems[target].owner == uint8_t(client.empire()));

    bool told = false;
    for (const ClientEvent& event : client.takeEvents()) {
        if (event.kind == NoticeKind::ColonyFounded && event.system == target) told = true;
    }
    CHECK(told);
}

TEST_CASE("уведомления: о своей столице при входе не сообщают") {
    // Иначе первым, что видит вошедший, становится «система захвачена»
    // о его собственном доме.
    //
    // Клиент подключается НЕ СРАЗУ, а к уже работающему серверу: именно
    // так и бывает в жизни, и именно этот случай ломался. Первая версия
    // теста подключала клиента в первую же миллисекунду, сервер не успевал
    // снять исходное состояние, и дефект не проявлялся.
    Session session(120);
    session.run(3000);   // сервер живёт сам по себе
    session.addClient("Михаил");
    session.run(3000);

    for (const ClientEvent& event : session.clients[0]->takeEvents()) {
        CHECK(event.kind != NoticeKind::SystemCaptured);
    }
}

TEST_CASE("уведомления: слияние флотов не считается потерей") {
    // Ложная тревога хуже пропущенной: игрок бросает дела и летит
    // спасать то, что цело.
    //
    // Первая версия считала ОТРЯДЫ и слала «флот уничтожен» при каждом
    // слиянии: два отряда становятся одним, количество падает, а не
    // потеряно ни одного корабля. Тест обязан это ловить, поэтому
    // проверяет, что слияние действительно произошло.
    Session session(120, 0, 0, 0x5E5510, /*speed=*/16);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    client.takeEvents();

    const auto countFleets = [&] {
        uint32_t total = 0;
        for (const auto& [id, fleet] : client.view().fleets) {
            if (fleet.empire == uint8_t(client.empire())) ++total;
        }
        return total;
    };
    const auto totalTonnage = [&] {
        uint32_t total = 0;
        for (const auto& [id, fleet] : client.view().fleets) {
            if (fleet.empire != uint8_t(client.empire())) continue;
            total += sim::fleetTonnage(fleet.composition);
        }
        return total;
    };

    // Сначала верфь: без неё система не имеет права строить флот,
    // и заказ просто не выполнится. Заодно шахты и литейная — иначе
    // стартовых сплавов не хватит на шесть корветов.
    const auto planets = client.planetsAt(client.capital());
    REQUIRE_FALSE(planets.empty());
    const uint32_t planet = planets.front().id;
    REQUIRE(planets.front().slots >= 4);

    REQUIRE(client.orderBuildBuilding(planet, 0, sim::Building::Shipyard));
    REQUIRE(client.orderBuildBuilding(planet, 1, sim::Building::Mine));
    REQUIRE(client.orderBuildBuilding(planet, 2, sim::Building::Mine));
    REQUIRE(client.orderBuildBuilding(planet, 3, sim::Building::Foundry));

    // Верфь строится семь минут игры, вся цепочка — почти двадцать.
    // Ждём, пока она действительно встанет: заказ корабля в систему без
    // верфи не выполняется вовсе.
    for (int round = 0; round < 40 && !client.planetsAt(client.capital()).empty(); ++round) {
        session.run(5000);
        const auto state = client.planetsAt(client.capital());
        if (state.front().buildings[3] == uint8_t(sim::Building::Foundry)) break;
    }
    REQUIRE(client.planetsAt(client.capital()).front().buildings[0] ==
            uint8_t(sim::Building::Shipyard));

    REQUIRE(client.orderBuildShip(client.capital(), sim::Hull::Corvette, 6));

    const uint32_t tonnageBefore = totalTonnage();
    for (int round = 0; round < 60; ++round) session.run(10000);

    // Слияние наблюдаем по РЕЗУЛЬТАТУ, а не по мгновенному состоянию:
    // построенный корабль сливается в следующем же тике, и поймать
    // промежуточный момент выборкой раз в десять секунд нельзя.
    //
    // Шесть новых кораблей без слияния дали бы шесть отрядов. Отряд
    // остался один — значит слияние произошло, причём многократно.
    CHECK(totalTonnage() > tonnageBefore);
    CHECK(countFleets() <= 2);

    for (const ClientEvent& event : client.takeEvents()) {
        CHECK(event.kind != NoticeKind::FleetDestroyed);
    }
}

TEST_CASE("уведомления: о бое узнают обе стороны, и по-разному") {
    // Одно сражение — две новости: победителю «выиграл», проигравшему
    // «проиграл». Если сервер шлёт одно и то же обоим, интерфейс врёт.
    Session session(40);
    session.addClient("первый");
    session.addClient("второй");
    session.run(2000);

    Client& first = *session.clients[0];
    Client& second = *session.clients[1];
    REQUIRE(first.ready());
    REQUIRE(second.ready());
    first.takeEvents();
    second.takeEvents();

    // Сводим флоты в одной системе. Гнать их через всю галактику
    // не нужно и вредно: тест проверяет уведомление, а не поиск пути,
    // и лишние двадцать минут игрового времени только делают его хрупким.
    // Ищем систему, соседнюю обеим столицам, иначе просто ближайшую
    // к обеим.
    uint32_t battlefield = 0xFFFFFFFFu;
    int32_t best = 1 << 20;
    for (uint32_t index = 0; index < first.galaxy().systemCount(); ++index) {
        const int32_t toFirst = first.galaxy().hopDistance(first.capital(), index);
        const int32_t toSecond = first.galaxy().hopDistance(second.capital(), index);
        if (toFirst < 0 || toSecond < 0) continue;
        const int32_t total = toFirst + toSecond;
        if (total >= best) continue;
        if (index == first.capital() || index == second.capital()) continue;
        best = total;
        battlefield = index;
    }
    REQUIRE(battlefield != 0xFFFFFFFFu);

    const uint32_t defender = first.fleetsAt(first.capital()).front();
    const uint32_t attacker = second.fleetsAt(second.capital()).front();
    REQUIRE(first.orderMove(defender, battlefield));
    REQUIRE(second.orderMove(attacker, battlefield));

    const auto isBattle = [](NoticeKind kind) {
        return kind == NoticeKind::BattleWon || kind == NoticeKind::BattleLost ||
               kind == NoticeKind::BattleDraw;
    };

    bool firstHeard = false, secondHeard = false;
    NoticeKind firstKind = NoticeKind::None, secondKind = NoticeKind::None;

    // Двести кругов, а не шестьдесят: в стартовом флоте теперь есть
    // колонизатор, скорость флота задаёт самый медленный корабль, и путь
    // до поля боя стал вдвое длиннее по времени. Цикл всё равно выходит
    // по СОБЫТИЮ, а не по счётчику, — предел лишь страхует от зависания.
    for (int round = 0; round < 200 && !(firstHeard && secondHeard); ++round) {
        session.run(20000);
        for (const ClientEvent& event : first.takeEvents()) {
            if (!isBattle(event.kind)) continue;
            firstHeard = true;
            firstKind = event.kind;
        }
        for (const ClientEvent& event : second.takeEvents()) {
            if (!isBattle(event.kind)) continue;
            secondHeard = true;
            secondKind = event.kind;
        }
    }

    REQUIRE(firstHeard);
    REQUIRE(secondHeard);

    // Исход зависит от бросков боя, и требовать от теста конкретного
    // победителя значило бы привязать его к сиду. Проверяется правило,
    // а не удача: у решённого боя две РАЗНЫЕ новости, у ничьей — одна
    // и та же ОБЕИМ сторонам. Молчания нет ни в одном из случаев.
    if (firstKind == NoticeKind::BattleDraw || secondKind == NoticeKind::BattleDraw) {
        CHECK(firstKind == NoticeKind::BattleDraw);
        CHECK(secondKind == NoticeKind::BattleDraw);
    } else {
        CHECK(firstKind != secondKind);
    }
}

TEST_CASE("приказ: заказ корабля без верфи отвергается вслух") {
    // Раньше заказ принимался и молча не выполнялся: игрок жал клавишу,
    // ничего не происходило, и понять почему было неоткуда. Молчаливый
    // отказ — худший вид отказа, потому что человек повторяет одно
    // и то же, считая, что промахнулся.
    Session session(80);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    client.takeEvents();

    // Свежая столица без единого здания.
    REQUIRE(client.orderBuildShip(client.capital(), sim::Hull::Corvette, 1));
    session.run(2000);

    bool told = false;
    for (const ClientEvent& event : client.takeEvents()) {
        if (event.kind == NoticeKind::OrderRejected) told = true;
    }
    CHECK(told);
}

TEST_CASE("приказ: с верфью заказ принимается") {
    Session session(80, 0, 0, 0x5E5510, /*speed=*/16);
    session.addClient("Михаил");
    session.run(2000);

    Client& client = *session.clients[0];
    const auto planets = client.planetsAt(client.capital());
    REQUIRE_FALSE(planets.empty());
    REQUIRE(client.orderBuildBuilding(planets.front().id, 0, sim::Building::Shipyard));

    // Верфь в 200 минералов при темпе 0.5 в секунду — почти семь минут игры.
    // Раньше она появлялась по щелчку, и тест этого не замечал.
    session.run(30000);
    REQUIRE(client.planetsAt(client.capital()).front().buildings[0] ==
            uint8_t(sim::Building::Shipyard));
    client.takeEvents();

    REQUIRE(client.orderBuildShip(client.capital(), sim::Hull::Corvette, 1));
    session.run(3000);

    for (const ClientEvent& event : client.takeEvents()) {
        CHECK(event.kind != NoticeKind::OrderRejected);
    }
}

TEST_CASE("уведомления: об осаде своей планеты игрок узнаёт сразу") {
    // Заголовок control.h обещает: владелец успевает получить уведомление,
    // союзники видят осаду и приходят. Без этой новости обещание пустое —
    // узнать об осаде можно было бы, только глядя в нужную часть карты
    // в нужную минуту.
    Session session(60, 0, 0, 0x5E5510, /*speed=*/16);
    session.addClient("защитник");
    session.addClient("нападающий");
    session.run(3000);

    Client& defender = *session.clients[0];
    Client& attacker = *session.clients[1];
    REQUIRE(defender.ready());
    REQUIRE(attacker.ready());
    defender.takeEvents();

    // Защитник уводит флот. Присутствие владельца снимает осаду
    // немедленно — это отдельное правило, и проверяется оно отдельно.
    const uint32_t guard = defender.fleetsAt(defender.capital()).front();
    REQUIRE(defender.orderMove(guard, defender.galaxy().neighbors(defender.capital())[0]));

    // Гоним флот нападающего прямо в столицу защитника.
    const uint32_t fleet = attacker.fleetsAt(attacker.capital()).front();
    REQUIRE(attacker.orderMove(fleet, defender.capital()));

    bool sieged = false;
    for (int round = 0; round < 60 && !sieged; ++round) {
        session.run(10000);
        for (const ClientEvent& event : defender.takeEvents()) {
            if (event.kind == NoticeKind::PlanetSieged) sieged = true;
        }
    }
    CHECK(sieged);
}

TEST_CASE("уведомления: о взятой планете узнаёт тот, кто её взял") {
    Session session(80, 0, 0, 0x5E5510, /*speed=*/8);
    session.addClient("Михаил");
    session.run(3000);

    Client& client = *session.clients[0];
    REQUIRE(client.ready());
    client.takeEvents();

    const uint32_t home = client.capital();
    const uint32_t target = client.galaxy().neighbors(home)[0];
    const uint32_t fleet = client.fleetsAt(home).front();
    REQUIRE(colonizeFirstNeutral(session, client, fleet, target));

    bool founded = false;
    for (const ClientEvent& event : client.takeEvents()) {
        if (event.kind == NoticeKind::ColonyFounded && event.system == target) {
            founded = true;
        }
    }
    CHECK(founded);
}
