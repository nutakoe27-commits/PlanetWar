#include "doctest.h"

#include <cstring>
#include <vector>

#include "pw/core/rng.h"
#include "pw/net/bitstream.h"

using namespace pw;
using namespace pw::net;

namespace {

/// Записать и прочитать обратно — самая частая проверка в этом файле.
template <typename Write, typename Read>
void roundtrip(Write write, Read read) {
    uint8_t buffer[512];
    ByteWriter writer(buffer, sizeof(buffer));
    write(writer);
    REQUIRE_FALSE(writer.overflowed());

    ByteReader reader(buffer, writer.size());
    read(reader);
    CHECK(reader.complete());
}

}  // namespace

// ---------------------------------------------------------------------------
// Числа туда и обратно
// ---------------------------------------------------------------------------

TEST_CASE("поток: целые фиксированной длины") {
    const uint64_t values[] = {0, 1, 127, 128, 255, 256, 65535, 65536,
                               0xFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
    for (uint64_t value : values) {
        roundtrip([&](ByteWriter& w) { w.u64(value); },
                  [&](ByteReader& r) { CHECK(r.u64() == value); });
    }
    roundtrip([](ByteWriter& w) { w.u8(0xAB); w.u16(0xBEEF); w.u32(0xDEADBEEF); },
              [](ByteReader& r) {
                  CHECK(r.u8() == 0xAB);
                  CHECK(r.u16() == 0xBEEF);
                  CHECK(r.u32() == 0xDEADBEEF);
              });
}

TEST_CASE("поток: varint переживает любое беззнаковое") {
    Rng rng(0x5EED, /*stream=*/1);
    for (int i = 0; i < 20000; ++i) {
        // Разные порядки величины, а не только маленькие числа.
        const uint64_t value = rng.next() >> (rng.next() % 64);
        roundtrip([&](ByteWriter& w) { w.varint(value); },
                  [&](ByteReader& r) { CHECK(r.varint() == value); });
    }
}

TEST_CASE("поток: svarint переживает любое знаковое") {
    const int64_t edges[] = {0, -1, 1, -2, 2, 127, -128,
                             INT64_MIN, INT64_MAX, INT64_MIN + 1};
    for (int64_t value : edges) {
        roundtrip([&](ByteWriter& w) { w.svarint(value); },
                  [&](ByteReader& r) { CHECK(r.svarint() == value); });
    }

    Rng rng(0x51674ED, /*stream=*/2);
    for (int i = 0; i < 20000; ++i) {
        const int64_t value = int64_t(rng.next()) >> (rng.next() % 63);
        roundtrip([&](ByteWriter& w) { w.svarint(value); },
                  [&](ByteReader& r) { CHECK(r.svarint() == value); });
    }
}

TEST_CASE("поток: varint экономит место на малых числах") {
    // Ради этого он и нужен: бюджет 20 КБ/с на игрока в бою, а номера
    // систем, тиков и количеств почти всегда маленькие.
    uint8_t buffer[64];

    ByteWriter small(buffer, sizeof(buffer));
    small.varint(42);
    CHECK(small.size() == 1);

    ByteWriter medium(buffer, sizeof(buffer));
    medium.varint(300);
    CHECK(medium.size() == 2);

    ByteWriter negativeOne(buffer, sizeof(buffer));
    negativeOne.svarint(-1);
    CHECK(negativeOne.size() == 1);   // без zigzag было бы десять
}

TEST_CASE("поток: fixed-point проходит без потерь") {
    const fx values[] = {fx::zero(), fx::one(), fx::fromInt(-1), fx::fromInt(1000000),
                         fx::fromFraction(1, 3), fx::fromFraction(-22, 7),
                         fx::fromRaw(INT64_MAX), fx::fromRaw(INT64_MIN)};
    for (fx value : values) {
        roundtrip([&](ByteWriter& w) { w.fixed(value); },
                  [&](ByteReader& r) { CHECK(r.fixed().raw() == value.raw()); });
    }
}

TEST_CASE("поток: смешанная запись читается в том же порядке") {
    roundtrip(
        [](ByteWriter& w) {
            w.u8(7);
            w.varint(123456);
            w.boolean(true);
            w.fixed(fx::fromFraction(3, 4));
            w.string("флот");
            w.boolean(false);
            w.svarint(-9000);
        },
        [](ByteReader& r) {
            CHECK(r.u8() == 7);
            CHECK(r.varint() == 123456);
            CHECK(r.boolean() == true);
            CHECK(r.fixed().raw() == fx::fromFraction(3, 4).raw());
            CHECK(r.string() == "флот");
            CHECK(r.boolean() == false);
            CHECK(r.svarint() == -9000);
        });
}

// ---------------------------------------------------------------------------
// Формат одинаков на всех платформах
// ---------------------------------------------------------------------------

TEST_CASE("поток: порядок байт задан протоколом, а не машиной") {
    // Если этот тест упадёт на другой архитектуре, значит запись взяла
    // порядок байт у процессора — и клиенты на ARM и x86 разошлись бы.
    uint8_t buffer[16];
    ByteWriter writer(buffer, sizeof(buffer));
    writer.u32(0x01020304);
    REQUIRE(writer.size() == 4);
    CHECK(buffer[0] == 0x04);
    CHECK(buffer[1] == 0x03);
    CHECK(buffer[2] == 0x02);
    CHECK(buffer[3] == 0x01);

    ByteWriter wide(buffer, sizeof(buffer));
    wide.u16(0xAABB);
    CHECK(buffer[0] == 0xBB);
    CHECK(buffer[1] == 0xAA);
}

TEST_CASE("поток: varint кодируется байт в байт по LEB128") {
    uint8_t buffer[16];
    ByteWriter writer(buffer, sizeof(buffer));
    writer.varint(300);
    REQUIRE(writer.size() == 2);
    CHECK(buffer[0] == 0xAC);
    CHECK(buffer[1] == 0x02);
}

// ---------------------------------------------------------------------------
// Битый пакет
//
// Всё, что приходит из сети, приходит от кого угодно. Эти проверки — про то,
// что мусор не роняет сервер и не читается частично.
// ---------------------------------------------------------------------------

TEST_CASE("битый пакет: чтение за границей поднимает флаг, а не падает") {
    uint8_t buffer[4] = {1, 2, 3, 4};
    ByteReader reader(buffer, sizeof(buffer));
    CHECK(reader.u32() == 0x04030201);
    CHECK_FALSE(reader.failed());

    CHECK(reader.u8() == 0);
    CHECK(reader.failed());
    CHECK(reader.remaining() == 0);
}

TEST_CASE("битый пакет: испорченный поток не чинится сам") {
    // Частично разобранный пакет опаснее отброшенного: половина полей новая,
    // половина осталась от прошлого состояния.
    uint8_t buffer[2] = {0xFF, 0xFF};
    ByteReader reader(buffer, sizeof(buffer));
    CHECK(reader.u32() == 0);
    REQUIRE(reader.failed());

    CHECK(reader.u8() == 0);
    CHECK(reader.varint() == 0);
    CHECK(reader.string().empty());
    CHECK(reader.failed());
    CHECK_FALSE(reader.complete());
}

TEST_CASE("битый пакет: бесконечный varint не зацикливает разбор") {
    // Десять байт с поднятым старшим битом — попытка заставить читателя
    // крутиться. Одиннадцатого он не ждёт.
    std::vector<uint8_t> evil(64, 0x80);
    ByteReader reader(evil.data(), evil.size());
    CHECK(reader.varint() == 0);
    CHECK(reader.failed());
}

TEST_CASE("битый пакет: длина строки не заставляет выделять память") {
    // Одно поле «длина» с огромным значением — самая дешёвая атака на
    // получателя, если её не проверять.
    uint8_t buffer[16];
    ByteWriter writer(buffer, sizeof(buffer));
    writer.varint(0xFFFFFFFFull);   // заявленная длина
    writer.u8('a');                 // а данных один байт

    ByteReader reader(buffer, writer.size());
    CHECK(reader.string().empty());
    CHECK(reader.failed());
}

TEST_CASE("битый пакет: строка длиннее предела отбрасывается") {
    uint8_t buffer[1024];
    ByteWriter writer(buffer, sizeof(buffer));
    writer.varint(kMaxStringLength + 1);
    for (uint32_t i = 0; i <= kMaxStringLength; ++i) writer.u8('x');

    ByteReader reader(buffer, writer.size());
    CHECK(reader.string().empty());
    CHECK(reader.failed());
}

TEST_CASE("битый пакет: хвост в пакете — это рассинхрон схемы") {
    // Лишние байты означают, что отправитель и получатель понимают пакет
    // по-разному. Дальше разойдётся и состояние, поэтому пакет не «почти
    // корректен», а некорректен.
    uint8_t buffer[16];
    ByteWriter writer(buffer, sizeof(buffer));
    writer.u32(1);
    writer.u32(2);

    ByteReader reader(buffer, writer.size());
    CHECK(reader.u32() == 1);
    CHECK_FALSE(reader.failed());
    CHECK_FALSE(reader.complete());   // прочитали не всё
}

TEST_CASE("битый пакет: случайный мусор никогда не роняет разбор") {
    // Обстрел: тысяча случайных буферов, разобранных как осмысленный пакет.
    // Единственное требование — не упасть и не соврать про complete().
    Rng rng(0xBADDA7A, /*stream=*/3);
    for (int attempt = 0; attempt < 2000; ++attempt) {
        uint8_t noise[64];
        const size_t size = size_t(rng.next() % sizeof(noise));
        for (size_t i = 0; i < size; ++i) noise[i] = uint8_t(rng.next());

        ByteReader reader(noise, size);
        reader.u8();
        reader.varint();
        reader.svarint();
        reader.fixed();
        reader.string();
        reader.u64();
        // Требование ровно одно: разбор завершился и сказал правду о себе.
        if (reader.complete()) CHECK_FALSE(reader.failed());
    }
}

// ---------------------------------------------------------------------------
// Переполнение при записи
// ---------------------------------------------------------------------------

TEST_CASE("запись: переполнение поднимает флаг и не портит соседей") {
    uint8_t buffer[8];
    // Канарейка за границей: если запись вылезет, мы это увидим.
    uint8_t guard[8];
    std::memset(guard, 0xCD, sizeof(guard));

    ByteWriter writer(buffer, sizeof(buffer));
    writer.u64(0x1122334455667788ull);
    CHECK_FALSE(writer.overflowed());
    CHECK(writer.size() == 8);

    writer.u8(1);
    CHECK(writer.overflowed());
    CHECK(writer.size() == 8);   // размер не вырос

    for (uint8_t byte : guard) CHECK(byte == 0xCD);
}

TEST_CASE("запись: после переполнения ничего не пишется") {
    uint8_t buffer[4];
    ByteWriter writer(buffer, sizeof(buffer));
    writer.u32(0);
    writer.u32(0xFFFFFFFF);
    REQUIRE(writer.overflowed());
    CHECK(writer.size() == 4);
    for (uint8_t byte : buffer) CHECK(byte == 0);
}

TEST_CASE("запись: строка длиннее предела обрезается, а не переполняет") {
    uint8_t buffer[1024];
    ByteWriter writer(buffer, sizeof(buffer));
    writer.string(std::string(kMaxStringLength * 2, 'z'));
    CHECK_FALSE(writer.overflowed());

    ByteReader reader(buffer, writer.size());
    CHECK(reader.string().size() == kMaxStringLength);
    CHECK(reader.complete());
}
