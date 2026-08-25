#include "doctest.h"

#include <map>
#include <string>
#include <vector>

#include "pw/core/rng.h"
#include "pw/net/connection.h"

using namespace pw;
using namespace pw::net;

namespace {

/// Провод между двумя соединениями.
///
/// Своя сеть в памяти, с управляемыми потерями, задержкой и дублями.
/// Настоящий сокет здесь не нужен и вреден: он не умеет терять пакеты по
/// команде, а именно потери и надо проверять. Время тоже своё — сутки
/// прокручиваются за миллисекунду.
class Wire {
public:
    Wire(uint64_t seed, uint32_t lossPercent, uint32_t maxDelay)
        : rng_(seed, /*stream=*/11), loss_(lossPercent), maxDelay_(maxDelay) {}

    /// Поставить в очередь пакет от `fromClient` стороны.
    void put(bool fromClient, const uint8_t* data, size_t size, int64_t now) {
        if (rng_.next() % 100 < loss_) return;

        Packet packet;
        packet.fromClient = fromClient;
        packet.data.assign(data, data + size);
        packet.at = now + int64_t(maxDelay_ == 0 ? 0 : rng_.next() % maxDelay_);
        queue_.push_back(packet);

        // Пара процентов пакетов приходит дважды: UDP так умеет сам.
        if (rng_.next() % 100 < 3) {
            packet.at += 5;
            queue_.push_back(packet);
        }
    }

    /// Отдать всё, чей срок пришёл.
    std::vector<std::pair<bool, std::vector<uint8_t>>> take(int64_t now) {
        std::vector<std::pair<bool, std::vector<uint8_t>>> out;
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (it->at > now) { ++it; continue; }
            out.emplace_back(it->fromClient, std::move(it->data));
            it = queue_.erase(it);
        }
        return out;
    }

private:
    struct Packet {
        bool fromClient;
        std::vector<uint8_t> data;
        int64_t at;
    };

    Rng rng_;
    uint32_t loss_;
    uint32_t maxDelay_;
    std::vector<Packet> queue_;
};

/// Пара «клиент — сервер», которую можно прокручивать по миллисекундам.
struct Pair {
    Connection client;
    Connection server;
    Wire wire;
    int64_t now = 0;
    bool serverAccepted = false;
    uint32_t assignedId;

    Pair(uint64_t seed = 1, uint32_t loss = 0, uint32_t delay = 0, uint32_t playerId = 7)
        : wire(seed, loss, delay), assignedId(playerId) {}

    void start() { client.connect(Address::loopback(9000), now); }

    /// Один шаг в миллисекунду.
    void step() {
        ++now;

        for (auto& [fromClient, data] : wire.take(now)) {
            Connection& target = fromClient ? server : client;

            // Сервер до соединения обязан сам разобрать запрос — соединения
            // ещё нет, а отвечать надо.
            if (fromClient && !serverAccepted) {
                uint16_t version = 0;
                if (Connection::parseRequest(data.data(), data.size(), version)) {
                    if (version == kProtocolVersion) {
                        server.accept(Address::loopback(9001), assignedId, now);
                        serverAccepted = true;
                    }
                    continue;
                }
            }

            ReceivedPacket packet;
            target.receive(data.data(), data.size(), now, packet);
            if (!packet.payload.empty()) {
                (fromClient ? serverSnapshots : clientSnapshots).push_back(packet.payload);
            }
        }

        pump(client, /*fromClient=*/true);
        if (serverAccepted) pump(server, /*fromClient=*/false);

        client.update(now);
        server.update(now);
    }

    void run(int64_t milliseconds) {
        const int64_t until = now + milliseconds;
        while (now < until) step();
    }

    /// Снапшот, который сервер шлёт клиенту в каждом пакете.
    std::vector<uint8_t> serverSnapshot;
    std::vector<std::vector<uint8_t>> clientSnapshots;
    std::vector<std::vector<uint8_t>> serverSnapshots;

private:
    void pump(Connection& connection, bool fromClient) {
        if (!connection.shouldSend(now)) return;
        uint8_t buffer[kMaxPacketSize];
        const void* snapshot = nullptr;
        size_t snapshotSize = 0;
        if (!fromClient && !serverSnapshot.empty()) {
            snapshot = serverSnapshot.data();
            snapshotSize = serverSnapshot.size();
        }
        const size_t size = connection.build(buffer, sizeof(buffer), now, snapshot, snapshotSize);
        if (size > 0) wire.put(fromClient, buffer, size, now);
    }
};

bool sendText(Connection& connection, const std::string& value) {
    return connection.sendReliable(value.data(), value.size());
}

std::vector<std::string> drain(Connection& connection) {
    std::vector<std::string> out;
    std::vector<uint8_t> buffer;
    while (connection.receiveReliable(buffer)) {
        out.emplace_back(buffer.begin(), buffer.end());
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Рукопожатие
// ---------------------------------------------------------------------------

TEST_CASE("соединение: рукопожатие проходит и выдаёт номер игрока") {
    Pair pair;
    pair.start();
    CHECK(pair.client.state() == ConnectionState::Connecting);

    pair.run(200);
    CHECK(pair.client.state() == ConnectionState::Connected);
    CHECK(pair.server.state() == ConnectionState::Connected);
    CHECK(pair.client.playerId() == 7);
}

TEST_CASE("соединение: потерянный Accept повторяется") {
    // Без повтора потеря одного пакета означала бы, что игрок висит на
    // «подключаюсь» до самого таймаута — пять секунд впустую.
    Pair pair(/*seed=*/42, /*loss=*/60, /*delay=*/3);
    pair.start();
    pair.run(2000);
    CHECK(pair.client.state() == ConnectionState::Connected);
}

TEST_CASE("соединение: молчащий сервер даёт отказ, а не вечное ожидание") {
    Connection client;
    client.connect(Address::loopback(9000), 0);
    for (int64_t now = 0; now <= kConnectTimeoutMilliseconds; ++now) client.update(now);
    CHECK(client.state() == ConnectionState::Failed);
    CHECK(client.reason() == DisconnectReason::Timeout);
}

TEST_CASE("соединение: сервер может отказать с причиной") {
    Connection client;
    client.connect(Address::loopback(9000), 0);

    uint8_t buffer[64];
    const size_t size = Connection::buildReject(buffer, sizeof(buffer), RejectReason::ServerFull);
    REQUIRE(size > 0);

    ReceivedPacket packet;
    CHECK(client.receive(buffer, size, 1, packet));
    CHECK(client.state() == ConnectionState::Failed);
    CHECK(client.reason() == DisconnectReason::Rejected);
    CHECK(client.rejectReason() == RejectReason::ServerFull);
}

// ---------------------------------------------------------------------------
// Разрыв
// ---------------------------------------------------------------------------

TEST_CASE("соединение: молчание собеседника рвёт связь") {
    Pair pair;
    pair.start();
    pair.run(200);
    REQUIRE(pair.client.state() == ConnectionState::Connected);

    // Сервер замолчал: крутим только время.
    for (int64_t step = 0; step < kTimeoutMilliseconds + 100; ++step) {
        pair.client.update(pair.now + step);
    }
    CHECK(pair.client.state() == ConnectionState::Failed);
    CHECK(pair.client.reason() == DisconnectReason::Timeout);
}

TEST_CASE("соединение: keepalive не даёт связи развалиться в тишине") {
    // Молчание неотличимо от обрыва, поэтому тишины в протоколе нет.
    Pair pair;
    pair.start();
    pair.run(30000);   // полминуты без единой команды
    CHECK(pair.client.state() == ConnectionState::Connected);
    CHECK(pair.server.state() == ConnectionState::Connected);
}

TEST_CASE("соединение: прощание разрывает сразу, без таймаута") {
    Pair pair;
    pair.start();
    pair.run(200);
    REQUIRE(pair.server.state() == ConnectionState::Connected);

    uint8_t buffer[64];
    const size_t size = pair.client.buildDisconnect(buffer, sizeof(buffer));
    REQUIRE(size > 0);

    ReceivedPacket packet;
    CHECK(pair.server.receive(buffer, size, pair.now, packet));
    CHECK(pair.server.state() == ConnectionState::Disconnected);
    CHECK(pair.server.reason() == DisconnectReason::Closed);
}

// ---------------------------------------------------------------------------
// Чужой трафик
// ---------------------------------------------------------------------------

TEST_CASE("соединение: чужой пакет отбрасывается молча") {
    // На любой открытый UDP-порт в интернете стучатся сканеры. Отвечать им
    // нельзя: сервер превратился бы в усилитель чужого трафика.
    Pair pair;
    pair.start();
    pair.run(200);

    uint8_t foreign[32] = {};
    foreign[0] = 0xDE; foreign[1] = 0xAD; foreign[2] = 0xBE; foreign[3] = 0xEF;

    ReceivedPacket packet;
    CHECK_FALSE(pair.server.receive(foreign, sizeof(foreign), pair.now, packet));
    CHECK(pair.server.state() == ConnectionState::Connected);
}

TEST_CASE("соединение: случайный мусор не роняет и не рвёт связь") {
    Pair pair;
    pair.start();
    pair.run(200);
    REQUIRE(pair.server.state() == ConnectionState::Connected);

    Rng rng(0x5CA44E2, /*stream=*/12);
    for (int attempt = 0; attempt < 5000; ++attempt) {
        uint8_t noise[128];
        const size_t size = size_t(rng.next() % sizeof(noise));
        for (size_t i = 0; i < size; ++i) noise[i] = uint8_t(rng.next());

        // Каждый пятый пакет — с правильной меткой протокола: так проверяется
        // не только фильтр по метке, но и разбор тела.
        if (size >= 5 && attempt % 5 == 0) {
            noise[0] = uint8_t((kProtocolId >> 0) & 0xFF);
            noise[1] = uint8_t((kProtocolId >> 8) & 0xFF);
            noise[2] = uint8_t((kProtocolId >> 16) & 0xFF);
            noise[3] = uint8_t((kProtocolId >> 24) & 0xFF);
            noise[4] = uint8_t(PacketType::Payload);
        }

        ReceivedPacket packet;
        pair.server.receive(noise, size, pair.now, packet);
    }
    CHECK(pair.server.state() == ConnectionState::Connected);
}

// ---------------------------------------------------------------------------
// Команды и снапшоты
// ---------------------------------------------------------------------------

TEST_CASE("соединение: команды доходят по порядку") {
    Pair pair;
    pair.start();
    pair.run(200);

    for (int i = 0; i < 20; ++i) REQUIRE(sendText(pair.client, "приказ " + std::to_string(i)));
    pair.run(500);

    const auto got = drain(pair.server);
    REQUIRE(got.size() == 20);
    for (int i = 0; i < 20; ++i) CHECK(got[size_t(i)] == "приказ " + std::to_string(i));
}

TEST_CASE("соединение: команды доходят через плохой канал") {
    Pair pair(/*seed=*/7, /*loss=*/25, /*delay=*/40);
    pair.start();
    pair.run(3000);
    REQUIRE(pair.client.state() == ConnectionState::Connected);

    std::vector<std::string> sentAll;
    for (int i = 0; i < 100; ++i) {
        const std::string command = "приказ " + std::to_string(i);
        if (sendText(pair.client, command)) sentAll.push_back(command);
    }

    std::vector<std::string> got;
    for (int round = 0; round < 200; ++round) {
        pair.run(100);
        for (const std::string& value : drain(pair.server)) got.push_back(value);
        if (got.size() == sentAll.size()) break;
    }

    REQUIRE(got.size() == sentAll.size());
    CHECK(got == sentAll);
    CHECK(pair.client.state() == ConnectionState::Connected);
}

TEST_CASE("соединение: снапшот доезжает и не пересылается") {
    // Снапшот ненадёжен намеренно: свежий полностью заменяет прошлый,
    // и пересылать потерянный — значит занимать канал устаревшими данными.
    Pair pair;
    pair.start();
    pair.run(200);

    const std::string state = "галактика: 200 систем";
    pair.serverSnapshot.assign(state.begin(), state.end());
    pair.run(500);

    REQUIRE_FALSE(pair.clientSnapshots.empty());
    const auto& first = pair.clientSnapshots.front();
    CHECK(std::string(first.begin(), first.end()) == state);
    // Их много: снапшот едет в каждом пакете, а не один раз.
    CHECK(pair.clientSnapshots.size() > 3);
}

TEST_CASE("соединение: снапшот и команды едут в одном пакете") {
    Pair pair;
    pair.start();
    pair.run(200);

    const std::string state(400, 'S');
    pair.serverSnapshot.assign(state.begin(), state.end());
    REQUIRE(sendText(pair.server, "мир изменился"));
    pair.run(300);

    CHECK_FALSE(pair.clientSnapshots.empty());
    CHECK(drain(pair.client) == std::vector<std::string>{"мир изменился"});
}

// ---------------------------------------------------------------------------
// Наблюдение
// ---------------------------------------------------------------------------

TEST_CASE("соединение: задержка измеряется") {
    Pair pair(/*seed=*/3, /*loss=*/0, /*delay=*/30);
    pair.start();
    pair.run(3000);
    CHECK(pair.client.roundTrip() > 0);
    // Провод задерживает до 30 мс в каждую сторону, значит туда-обратно
    // разумная оценка не превышает сотни.
    CHECK(pair.client.roundTrip() < 100);
}

TEST_CASE("соединение: потери видны") {
    Pair pair(/*seed=*/9, /*loss=*/30, /*delay=*/20);
    pair.start();
    pair.run(5000);
    REQUIRE(pair.client.state() == ConnectionState::Connected);
    CHECK(pair.client.lossPercent() > 5);
    CHECK(pair.client.lossPercent() < 70);
}
