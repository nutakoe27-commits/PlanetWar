#include "doctest.h"

#include <cstring>
#include <string>

#include "pw/net/socket.h"

using namespace pw;
using namespace pw::net;

namespace {

/// Сокеты нужно инициализировать один раз на процесс (это про Windows).
struct Sockets {
    Sockets() { REQUIRE(initialiseSockets()); }
    ~Sockets() { shutdownSockets(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// Адрес
// ---------------------------------------------------------------------------

TEST_CASE("адрес: разбор и обратная печать") {
    const Address address = Address::parse("192.168.1.10:9000");
    CHECK(address.valid());
    CHECK(address.host == 0xC0A8010Au);
    CHECK(address.port == 9000);
    CHECK(address.toString() == "192.168.1.10:9000");
}

TEST_CASE("адрес: порт по умолчанию") {
    const Address address = Address::parse("127.0.0.1", 7777);
    CHECK(address.host == 0x7F000001u);
    CHECK(address.port == 7777);
    CHECK(address == Address::loopback(7777));
}

TEST_CASE("адрес: мусор не разбирается") {
    // Адрес приходит из командной строки и из конфигов, то есть от человека.
    const char* bad[] = {"", "1.2.3", "1.2.3.4.5", "256.0.0.1", "a.b.c.d",
                         "1.2.3.4:", "1.2.3.4:70000", "1.2.3.4:abc", "..."};
    for (const char* text : bad) {
        CHECK_FALSE(Address::parse(text, 1).valid());
    }
}

TEST_CASE("адрес: сравнение даёт полный порядок") {
    // На этом держится словарь соединений на сервере.
    CHECK(Address::loopback(1) < Address::loopback(2));
    CHECK(Address::parse("1.0.0.0:9") < Address::parse("2.0.0.0:1"));
    CHECK(Address::loopback(5) == Address::loopback(5));
    CHECK(Address::loopback(5) != Address::loopback(6));
}

// ---------------------------------------------------------------------------
// Сокет
// ---------------------------------------------------------------------------

TEST_CASE("сокет: открывается на свободном порту и знает какой это порт") {
    Sockets guard;
    Socket socket;
    REQUIRE(socket.open(0));
    CHECK(socket.valid());
    CHECK(socket.port() != 0);   // систему просили выбрать — она выбрала
    socket.close();
    CHECK_FALSE(socket.valid());
}

TEST_CASE("сокет: занятый порт даёт внятную ошибку") {
    Sockets guard;
    Socket first;
    REQUIRE(first.open(0));

    Socket second;
    CHECK_FALSE(second.open(first.port()));
    CHECK_FALSE(second.error().empty());
}

TEST_CASE("сокет: датаграмма доходит от одного к другому") {
    Sockets guard;
    Socket sender, receiver;
    REQUIRE(sender.open(0));
    REQUIRE(receiver.open(0));

    const char payload[] = "флот 7 в систему 42";
    REQUIRE(sender.send(Address::loopback(receiver.port()), payload, sizeof(payload)));

    // Локальная петля доставляет сразу, но не обязана: пробуем несколько раз.
    char buffer[256] = {};
    Address from;
    size_t got = 0;
    for (int attempt = 0; attempt < 1000 && got == 0; ++attempt) {
        got = receiver.receive(from, buffer, sizeof(buffer));
    }

    REQUIRE(got == sizeof(payload));
    CHECK(std::memcmp(buffer, payload, sizeof(payload)) == 0);
    CHECK(from.port == sender.port());
}

TEST_CASE("сокет: пустая очередь — это ноль, а не ошибка") {
    // Сервер опрашивает сокет каждый тик. Если пустая очередь считалась бы
    // ошибкой, журнал заполнился бы мусором за минуту.
    Sockets guard;
    Socket socket;
    REQUIRE(socket.open(0));

    char buffer[64];
    Address from;
    CHECK(socket.receive(from, buffer, sizeof(buffer)) == 0);
    CHECK(socket.error().empty());
}

TEST_CASE("сокет: приём не блокирует тик") {
    // Главное свойство: сервер обязан крутить свои 10 Гц независимо от того,
    // пришло что-нибудь или нет. Блокирующий сокет остановил бы симуляцию.
    Sockets guard;
    Socket socket;
    REQUIRE(socket.open(0));

    char buffer[64];
    Address from;
    for (int i = 0; i < 10000; ++i) {
        socket.receive(from, buffer, sizeof(buffer));   // просто не зависаем
    }
}

TEST_CASE("сокет: слишком большой пакет не отправляется") {
    // Больше MTU — риск фрагментации, а потеря одного фрагмента убивает
    // весь пакет. Лучше честный отказ, чем случайная потеря.
    Sockets guard;
    Socket socket;
    REQUIRE(socket.open(0));

    std::string huge(kMaxPacketSize + 1, 'x');
    CHECK_FALSE(socket.send(Address::loopback(socket.port()), huge.data(), huge.size()));

    std::string limit(kMaxPacketSize, 'x');
    CHECK(socket.send(Address::loopback(socket.port()), limit.data(), limit.size()));
}

TEST_CASE("сокет: закрытый сокет ничего не делает молча") {
    Sockets guard;
    Socket socket;
    char buffer[16] = {};
    Address from;
    CHECK_FALSE(socket.send(Address::loopback(9), buffer, sizeof(buffer)));
    CHECK(socket.receive(from, buffer, sizeof(buffer)) == 0);
}

TEST_CASE("сокет: перемещается без потери дескриптора") {
    Sockets guard;
    Socket first;
    REQUIRE(first.open(0));
    const uint16_t port = first.port();

    Socket second = std::move(first);
    CHECK(second.valid());
    CHECK(second.port() == port);
    CHECK_FALSE(first.valid());

    // И принимает — то есть дескриптор действительно тот же.
    Socket sender;
    REQUIRE(sender.open(0));
    const char payload[] = "проверка";
    REQUIRE(sender.send(Address::loopback(port), payload, sizeof(payload)));

    char buffer[64] = {};
    Address from;
    size_t got = 0;
    for (int attempt = 0; attempt < 1000 && got == 0; ++attempt) {
        got = second.receive(from, buffer, sizeof(buffer));
    }
    CHECK(got == sizeof(payload));
}
