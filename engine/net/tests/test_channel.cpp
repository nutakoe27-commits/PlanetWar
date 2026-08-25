#include "doctest.h"

#include <cstring>
#include <string>
#include <vector>

#include "pw/core/rng.h"
#include "pw/net/channel.h"

using namespace pw;
using namespace pw::net;

namespace {

std::string text(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

bool sendText(ReliableChannel& channel, const std::string& value) {
    return channel.send(value.data(), value.size());
}

/// Перенести содержимое одного пакета из канала в канал.
uint32_t transfer(ReliableChannel& from, ReliableChannel& to, uint16_t sequence,
                  size_t capacity = 1200) {
    std::vector<uint8_t> packet(capacity);
    ByteWriter writer(packet.data(), packet.size());
    const uint32_t packed = from.pack(writer, sequence);

    ByteReader reader(packet.data(), writer.size());
    REQUIRE(to.unpack(reader));
    return packed;
}

std::vector<std::string> drain(ReliableChannel& channel) {
    std::vector<std::string> out;
    std::vector<uint8_t> buffer;
    while (channel.receive(buffer)) out.push_back(text(buffer));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Основной путь
// ---------------------------------------------------------------------------

TEST_CASE("канал: сообщение доходит и отдаётся один раз") {
    ReliableChannel sender, receiver;
    REQUIRE(sendText(sender, "флот 7 в систему 42"));
    CHECK(sender.queued() == 1);

    transfer(sender, receiver, 0);
    CHECK(drain(receiver) == std::vector<std::string>{"флот 7 в систему 42"});
    CHECK(drain(receiver).empty());
}

TEST_CASE("канал: порядок сохраняется") {
    ReliableChannel sender, receiver;
    for (int i = 0; i < 10; ++i) REQUIRE(sendText(sender, "команда " + std::to_string(i)));

    transfer(sender, receiver, 0);
    const auto got = drain(receiver);
    REQUIRE(got.size() == 10);
    for (int i = 0; i < 10; ++i) CHECK(got[size_t(i)] == "команда " + std::to_string(i));
}

TEST_CASE("канал: подтверждение освобождает очередь") {
    ReliableChannel sender, receiver;
    sendText(sender, "раз");
    sendText(sender, "два");
    transfer(sender, receiver, 5);
    CHECK(sender.queued() == 2);   // отправлено, но ещё не подтверждено

    sender.onPacketAcked(5);
    CHECK(sender.queued() == 0);
}

TEST_CASE("канал: отправленное не уезжает повторно без нужды") {
    ReliableChannel sender, receiver;
    sendText(sender, "раз");

    CHECK(transfer(sender, receiver, 0) == 1);
    // Второй пакет: сообщение уже в пути, слать его снова незачем.
    CHECK(transfer(sender, receiver, 1) == 0);
}

// ---------------------------------------------------------------------------
// Потери
// ---------------------------------------------------------------------------

TEST_CASE("канал: потерянный пакет пересылается") {
    ReliableChannel sender, receiver;
    sendText(sender, "построй крейсер");

    // Пакет 0 «уехал» и пропал: до получателя не доводим.
    std::vector<uint8_t> lost(256);
    ByteWriter writer(lost.data(), lost.size());
    CHECK(sender.pack(writer, 0) == 1);
    CHECK(transfer(sender, receiver, 1) == 0);   // пока считаем, что 0 в пути

    sender.onPacketLost(0);
    CHECK(transfer(sender, receiver, 2) == 1);   // теперь поехало заново
    CHECK(drain(receiver) == std::vector<std::string>{"построй крейсер"});
}

TEST_CASE("канал: дубль пакета не удваивает сообщение") {
    // UDP дублирует пакеты сам. Применить команду дважды — значит построить
    // два корабля вместо одного.
    ReliableChannel sender, receiver;
    sendText(sender, "построй линкор");

    std::vector<uint8_t> packet(256);
    ByteWriter writer(packet.data(), packet.size());
    sender.pack(writer, 0);

    for (int copy = 0; copy < 3; ++copy) {
        ByteReader reader(packet.data(), writer.size());
        CHECK(receiver.unpack(reader));
    }
    CHECK(drain(receiver) == std::vector<std::string>{"построй линкор"});
}

TEST_CASE("канал: пришедшее не по порядку придерживается до предшественника") {
    // Кладём каждое сообщение в свой пакет — для этого хватает тесного
    // бюджета — и доставляем их в обратном порядке.
    //
    // Этот тест поймал дефект получателя: он брал за точку отсчёта номер
    // ПЕРВОГО пришедшего сообщения и потому отдавал наверх второе, не
    // дождавшись первого. То есть порядок ломался ровно тогда, когда
    // терялся первый пакет, — в самом частом случае из возможных.
    ReliableChannel splitSender, splitReceiver;
    sendText(splitSender, "первая");
    sendText(splitSender, "вторая");

    std::vector<uint8_t> tiny(24);
    ByteWriter one(tiny.data(), tiny.size());
    REQUIRE(splitSender.pack(one, 0) == 1);
    std::vector<uint8_t> packetOne(tiny.begin(), tiny.begin() + long(one.size()));

    ByteWriter two(tiny.data(), tiny.size());
    REQUIRE(splitSender.pack(two, 1) == 1);
    std::vector<uint8_t> packetTwo(tiny.begin(), tiny.begin() + long(two.size()));

    // Доставляем ВТОРОЕ раньше первого.
    ByteReader readerTwo(packetTwo.data(), packetTwo.size());
    REQUIRE(splitReceiver.unpack(readerTwo));
    CHECK(drain(splitReceiver).empty());        // держим: первого ещё нет
    CHECK(splitReceiver.held() == 1);

    ByteReader readerOne(packetOne.data(), packetOne.size());
    REQUIRE(splitReceiver.unpack(readerOne));
    CHECK(drain(splitReceiver) == std::vector<std::string>{"первая", "вторая"});
}

// ---------------------------------------------------------------------------
// Границы
// ---------------------------------------------------------------------------

TEST_CASE("канал: пустое и слишком большое сообщение отвергаются") {
    ReliableChannel channel;
    CHECK_FALSE(channel.send("", 0));

    std::vector<uint8_t> huge(kMaxMessageSize + 1, 'x');
    CHECK_FALSE(channel.send(huge.data(), huge.size()));

    std::vector<uint8_t> limit(kMaxMessageSize, 'x');
    CHECK(channel.send(limit.data(), limit.size()));
}

TEST_CASE("канал: переполнение очереди честно возвращает отказ") {
    // Если собеседник молчит, канал обязан сказать об этом, а не расти,
    // пока не кончится память.
    ReliableChannel channel;
    for (uint32_t i = 0; i < kMaxQueuedMessages; ++i) {
        REQUIRE(sendText(channel, "команда " + std::to_string(i)));
    }
    CHECK(channel.queued() == kMaxQueuedMessages);
    CHECK_FALSE(sendText(channel, "лишняя"));
}

TEST_CASE("канал: тесный пакет увозит часть, остальное ждёт") {
    ReliableChannel sender, receiver;
    for (int i = 0; i < 50; ++i) sendText(sender, std::string(60, char('a' + i % 26)));

    uint32_t delivered = 0;
    for (uint16_t sequence = 0; sequence < 20 && sender.queued() > 0; ++sequence) {
        delivered += transfer(sender, receiver, sequence, /*capacity=*/200);
        sender.onPacketAcked(sequence);
    }
    CHECK(delivered == 50);
    CHECK(drain(receiver).size() == 50);
}

TEST_CASE("канал: битый пакет отвергается целиком") {
    ReliableChannel sender, receiver;
    sendText(sender, "команда");

    std::vector<uint8_t> packet(256);
    ByteWriter writer(packet.data(), packet.size());
    sender.pack(writer, 0);

    // Обрезаем пакет посередине.
    ByteReader truncated(packet.data(), writer.size() / 2);
    CHECK_FALSE(receiver.unpack(truncated));
    CHECK(drain(receiver).empty());
}

TEST_CASE("канал: заявленный размер больше пакета не проходит") {
    // Самая дешёвая атака: сказать «дальше идёт килобайт» и не прислать его.
    uint8_t packet[32];
    ByteWriter writer(packet, sizeof(packet));
    writer.varint(1);              // одно сообщение
    writer.varint(0);              // номер
    writer.varint(kMaxMessageSize);  // размер, которого нет
    writer.u8('x');

    ReliableChannel receiver;
    ByteReader reader(packet, writer.size());
    CHECK_FALSE(receiver.unpack(reader));
}

TEST_CASE("канал: мусор вместо пакета не роняет разбор") {
    Rng rng(0xDEADBEEF, /*stream=*/5);
    for (int attempt = 0; attempt < 3000; ++attempt) {
        uint8_t noise[128];
        const size_t size = size_t(rng.next() % sizeof(noise));
        for (size_t i = 0; i < size; ++i) noise[i] = uint8_t(rng.next());

        ReliableChannel receiver;
        ByteReader reader(noise, size);
        receiver.unpack(reader);   // требование одно: не упасть

        std::vector<uint8_t> buffer;
        while (receiver.receive(buffer)) { /* и не зациклиться */ }
    }
}

// ---------------------------------------------------------------------------
// Плохой канал целиком
// ---------------------------------------------------------------------------

TEST_CASE("канал: тысяча команд доходит по порядку через плохую сеть") {
    // Главная проверка всего слоя. Двадцать процентов потерь, перестановка,
    // дубли — и требование: получатель видит ровно те команды, ровно в том
    // порядке и ровно по одному разу.
    //
    // Именно это свойство отделяет «команда игрока» от «примерно дошло»:
    // без него флот уходит не туда, а корабль строится дважды.
    struct Packet {
        uint16_t sequence;
        std::vector<uint8_t> data;
        uint32_t deliverAt;
    };

    Rng rng(0xBAD4E7, /*stream=*/6);
    ReliableChannel sender, receiver;
    SentTracker sent;
    AckTracker acks;

    std::vector<Packet> wire;
    std::vector<std::string> got;
    uint32_t produced = 0;
    constexpr uint32_t kWanted = 1000;

    for (uint32_t step = 0; step < 40000 && got.size() < kWanted; ++step) {
        // Ставим команды в очередь, пока она принимает.
        while (produced < kWanted &&
               sendText(sender, "команда " + std::to_string(produced))) {
            ++produced;
        }

        // Собираем пакет.
        const uint16_t sequence = sent.next();
        std::vector<uint8_t> packet(400);
        ByteWriter writer(packet.data(), packet.size());
        sender.pack(writer, sequence);
        packet.resize(writer.size());

        const uint32_t roll = uint32_t(rng.next() % 100);
        if (roll >= 20) {                       // 20% теряется
            const uint32_t delay = uint32_t(rng.next() % 5);
            wire.push_back(Packet{sequence, packet, step + delay});
            if (roll >= 97) wire.push_back(Packet{sequence, packet, step + delay + 2});
        }

        // Доставляем то, чей срок пришёл.
        for (auto it = wire.begin(); it != wire.end();) {
            if (it->deliverAt > step) { ++it; continue; }
            if (acks.onReceived(it->sequence)) {
                ByteReader reader(it->data.data(), it->data.size());
                CHECK(receiver.unpack(reader));
            }
            it = wire.erase(it);
        }

        // Подтверждения обратно; они тоже теряются.
        if (rng.next() % 100 >= 15) {
            sent.onAck(acks.latest(), acks.bits(),
                       [&](uint16_t s) { sender.onPacketAcked(s); },
                       [&](uint16_t s) { sender.onPacketLost(s); });
        }

        std::vector<uint8_t> buffer;
        while (receiver.receive(buffer)) got.push_back(text(buffer));
    }

    REQUIRE(got.size() == kWanted);
    for (uint32_t i = 0; i < kWanted; ++i) {
        CHECK(got[i] == "команда " + std::to_string(i));
    }
}
