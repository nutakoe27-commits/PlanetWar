#include "doctest.h"

#include <string>

#include "pw/core/rng.h"
#include "pw/game/protocol.h"

using namespace pw;
using namespace pw::game;

namespace {

template <typename Write, typename Read>
void roundtrip(Write write, Read read) {
    uint8_t buffer[1024];
    ByteWriter writer(buffer, sizeof(buffer));
    write(writer);
    REQUIRE_FALSE(writer.overflowed());

    ByteReader reader(buffer, writer.size());
    MessageType type = MessageType::Join;
    REQUIRE(readMessageType(reader, type));
    read(reader, type);
    CHECK(reader.complete());
}

}  // namespace

TEST_CASE("протокол: Join туда и обратно") {
    roundtrip([](ByteWriter& w) { writeJoin(w, JoinMessage{"Михаил"}); },
              [](ByteReader& r, MessageType type) {
                  CHECK(type == MessageType::Join);
                  JoinMessage message;
                  REQUIRE(readJoin(r, message));
                  CHECK(message.name == "Михаил");
              });
}

TEST_CASE("протокол: Welcome переносит параметры галактики бит в бит") {
    // Самое важное сообщение протокола: клиент строит галактику той же
    // функцией, что и сервер, и обязан получить ровно ту же карту.
    // Разойдись хоть один параметр — игроки увидят разные миры, и это
    // проявится не сразу, а когда флот «промахнётся» мимо системы,
    // которой у соседа нет.
    sim::GalaxyParams params;
    params.seed = 0x123456789ABCDEFull;
    params.systemCount = 377;
    params.radius = fx::fromInt(1500);
    params.arms = 5;
    params.minSpacing = fx::fromFraction(571, 20);
    params.maxLaneLength = fx::fromInt(163);
    params.lanesPerSystem = 4;
    params.armFractionPercent = 68;
    params.armSpread = fx::fromFraction(3, 25);
    params.radialJitter = fx::fromFraction(2, 17);
    params.twist = fx::fromFraction(7, 12);
    params.shortcutRangePercent = 210;
    params.shortcutHopThreshold = 9;
    params.shortcutRounds = 2;

    WelcomeMessage sent;
    sent.params = params;
    sent.empire = 3;
    sent.capitalSystem = 128;
    sent.tick = 987654321;

    roundtrip([&](ByteWriter& w) { writeWelcome(w, sent); },
              [&](ByteReader& r, MessageType type) {
                  CHECK(type == MessageType::Welcome);
                  WelcomeMessage got;
                  REQUIRE(readWelcome(r, got));
                  CHECK(got.params.seed == params.seed);
                  CHECK(got.params.systemCount == params.systemCount);
                  CHECK(got.params.radius.raw() == params.radius.raw());
                  CHECK(got.params.arms == params.arms);
                  CHECK(got.params.minSpacing.raw() == params.minSpacing.raw());
                  CHECK(got.params.maxLaneLength.raw() == params.maxLaneLength.raw());
                  CHECK(got.params.lanesPerSystem == params.lanesPerSystem);
                  CHECK(got.params.armFractionPercent == params.armFractionPercent);
                  CHECK(got.params.armSpread.raw() == params.armSpread.raw());
                  CHECK(got.params.radialJitter.raw() == params.radialJitter.raw());
                  CHECK(got.params.twist.raw() == params.twist.raw());
                  CHECK(got.params.shortcutRangePercent == params.shortcutRangePercent);
                  CHECK(got.params.shortcutHopThreshold == params.shortcutHopThreshold);
                  CHECK(got.params.shortcutRounds == params.shortcutRounds);
                  CHECK(got.empire == 3);
                  CHECK(got.capitalSystem == 128);
                  CHECK(got.tick == 987654321);
              });
}

TEST_CASE("протокол: приказы туда и обратно") {
    roundtrip([](ByteWriter& w) { writeMoveFleet(w, MoveFleetMessage{42, 137}); },
              [](ByteReader& r, MessageType type) {
                  CHECK(type == MessageType::MoveFleet);
                  MoveFleetMessage message;
                  REQUIRE(readMoveFleet(r, message));
                  CHECK(message.fleet == 42);
                  CHECK(message.target == 137);
              });

    roundtrip([](ByteWriter& w) {
                  writeBuildShip(w, BuildShipMessage{7, uint8_t(sim::Hull::Cruiser), 3});
              },
              [](ByteReader& r, MessageType type) {
                  CHECK(type == MessageType::BuildShip);
                  BuildShipMessage message;
                  REQUIRE(readBuildShip(r, message));
                  CHECK(message.system == 7);
                  CHECK(message.hull == uint8_t(sim::Hull::Cruiser));
                  CHECK(message.count == 3);
              });

    roundtrip([](ByteWriter& w) {
                  writeBuildBuilding(w, BuildBuildingMessage{99, 5, uint8_t(sim::Building::Foundry)});
              },
              [](ByteReader& r, MessageType type) {
                  CHECK(type == MessageType::BuildBuilding);
                  BuildBuildingMessage message;
                  REQUIRE(readBuildBuilding(r, message));
                  CHECK(message.planet == 99);
                  CHECK(message.slot == 5);
                  CHECK(message.building == uint8_t(sim::Building::Foundry));
              });

    roundtrip([](ByteWriter& w) {
                  writeNotice(w, NoticeMessage{NoticeKind::SystemCaptured, 314});
              },
              [](ByteReader& r, MessageType type) {
                  CHECK(type == MessageType::Notice);
                  NoticeMessage message;
                  REQUIRE(readNotice(r, message));
                  CHECK(message.kind == NoticeKind::SystemCaptured);
                  CHECK(message.system == 314);
              });
}

// ---------------------------------------------------------------------------
// Враньё в пакете
//
// Всё это приходит от клиента, то есть от кого угодно. Проверка обязана
// стоять В РАЗБОРЕ, а не у вызывающего: любой, кто её забудет, получит
// выход за границу таблицы.
// ---------------------------------------------------------------------------

TEST_CASE("протокол: несуществующий корпус отвергается при разборе") {
    uint8_t buffer[64];
    ByteWriter writer(buffer, sizeof(buffer));
    writer.varint(1);
    writer.u8(200);   // корпуса с таким номером нет
    writer.u8(1);

    ByteReader reader(buffer, writer.size());
    BuildShipMessage message;
    CHECK_FALSE(readBuildShip(reader, message));
}

TEST_CASE("протокол: нулевой корпус и нулевое количество отвергаются") {
    uint8_t buffer[64];
    for (int variant = 0; variant < 2; ++variant) {
        ByteWriter writer(buffer, sizeof(buffer));
        writer.varint(1);
        writer.u8(variant == 0 ? 0 : uint8_t(sim::Hull::Corvette));
        writer.u8(variant == 0 ? 1 : 0);

        ByteReader reader(buffer, writer.size());
        BuildShipMessage message;
        CHECK_FALSE(readBuildShip(reader, message));
    }
}

TEST_CASE("протокол: слот и здание вне диапазона отвергаются") {
    uint8_t buffer[64];
    ByteWriter slot(buffer, sizeof(buffer));
    slot.varint(1);
    slot.u8(sim::kMaxSlots);   // слотов ровно kMaxSlots, нумерация с нуля
    slot.u8(uint8_t(sim::Building::Mine));
    {
        ByteReader reader(buffer, slot.size());
        BuildBuildingMessage message;
        CHECK_FALSE(readBuildBuilding(reader, message));
    }

    ByteWriter building(buffer, sizeof(buffer));
    building.varint(1);
    building.u8(0);
    building.u8(uint8_t(sim::Building::Count));
    {
        ByteReader reader(buffer, building.size());
        BuildBuildingMessage message;
        CHECK_FALSE(readBuildBuilding(reader, message));
    }
}

TEST_CASE("протокол: безумные параметры галактики отвергаются") {
    // Клиент, доверившийся числу из пакета, повесил бы себя сам: генератор
    // выделит память и закрутит циклы ровно на столько, сколько сказано.
    struct Case { const char* what; sim::GalaxyParams params; };
    std::vector<Case> cases;
    {
        sim::GalaxyParams p; p.systemCount = 0;            cases.push_back({"ноль систем", p});
    }
    {
        sim::GalaxyParams p; p.systemCount = 2000000000u;  cases.push_back({"два миллиарда", p});
    }
    {
        sim::GalaxyParams p; p.arms = 0;                   cases.push_back({"ноль рукавов", p});
    }
    {
        sim::GalaxyParams p; p.lanesPerSystem = 1000;      cases.push_back({"тысяча линий", p});
    }
    {
        sim::GalaxyParams p; p.radius = fx::zero();        cases.push_back({"нулевой радиус", p});
    }
    {
        sim::GalaxyParams p; p.minSpacing = fx::fromInt(-5); cases.push_back({"минус шаг", p});
    }
    {
        sim::GalaxyParams p; p.shortcutRounds = 5000;      cases.push_back({"пять тысяч проходов", p});
    }

    for (const Case& item : cases) {
        CAPTURE(item.what);
        uint8_t buffer[512];
        ByteWriter writer(buffer, sizeof(buffer));
        WelcomeMessage message;
        message.params = item.params;
        message.capitalSystem = 0;
        writeWelcome(writer, message);

        ByteReader reader(buffer, writer.size());
        MessageType type = MessageType::Join;
        REQUIRE(readMessageType(reader, type));
        WelcomeMessage got;
        CHECK_FALSE(readWelcome(reader, got));
    }
}

TEST_CASE("протокол: столица вне галактики отвергается") {
    uint8_t buffer[512];
    ByteWriter writer(buffer, sizeof(buffer));
    WelcomeMessage message;
    message.params.systemCount = 100;
    message.capitalSystem = 100;   // нумерация с нуля, значит это уже вне
    writeWelcome(writer, message);

    ByteReader reader(buffer, writer.size());
    MessageType type = MessageType::Join;
    REQUIRE(readMessageType(reader, type));
    WelcomeMessage got;
    CHECK_FALSE(readWelcome(reader, got));
}

TEST_CASE("протокол: неизвестный тип сообщения отвергается") {
    for (int raw = 0; raw < 256; ++raw) {
        uint8_t buffer[4] = {uint8_t(raw)};
        ByteReader reader(buffer, 1);
        MessageType type = MessageType::Join;
        const bool known = readMessageType(reader, type);
        const bool expected = raw == 1 || raw == 2 || raw == 10 || raw == 11 ||
                              raw == 12 || raw == 20;
        CHECK(known == expected);
    }
}

TEST_CASE("протокол: мусор вместо сообщения не роняет разбор") {
    Rng rng(0x9A0705, /*stream=*/21);
    for (int attempt = 0; attempt < 5000; ++attempt) {
        uint8_t noise[128];
        const size_t size = size_t(rng.next() % sizeof(noise));
        for (size_t i = 0; i < size; ++i) noise[i] = uint8_t(rng.next());

        ByteReader reader(noise, size);
        MessageType type = MessageType::Join;
        if (!readMessageType(reader, type)) continue;

        switch (type) {
            case MessageType::Join: { JoinMessage m; readJoin(reader, m); break; }
            case MessageType::Welcome: { WelcomeMessage m; readWelcome(reader, m); break; }
            case MessageType::MoveFleet: { MoveFleetMessage m; readMoveFleet(reader, m); break; }
            case MessageType::BuildShip: { BuildShipMessage m; readBuildShip(reader, m); break; }
            case MessageType::BuildBuilding: {
                BuildBuildingMessage m; readBuildBuilding(reader, m); break;
            }
            case MessageType::Notice: { NoticeMessage m; readNotice(reader, m); break; }
        }
    }
}
