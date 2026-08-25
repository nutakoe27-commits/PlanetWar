#pragma once

// UDP-сокет.
//
// Слой тонкий намеренно: он прячет разницу между POSIX и Winsock и больше
// ничего не делает. Вся логика надёжности живёт выше (sequence.h, channel.h,
// connection.h) и о существовании сокетов не знает — поэтому её можно
// проверять симулятором плохой сети, без единого системного вызова.
//
// Заголовки операционной системы сюда не подключаются: <winsock2.h> тянет
// за собой пол-Windows и ломает сборку всем, кто просто хотел отправить
// пакет. Дескриптор хранится как обычное целое.

#include <cstdint>
#include <string>

namespace pw::net {

/// Максимальный размер датаграммы, которую мы отправляем.
///
/// 1200 байт, а не 1500: типовой MTU минус запас на заголовки IP, UDP и
/// туннели. Пакет крупнее рискует быть фрагментированным, а фрагментация
/// в UDP означает, что потеря одного фрагмента убивает весь пакет.
inline constexpr uint32_t kMaxPacketSize = 1200;

/// Адрес IPv4.
///
/// Только IPv4 на старте — это осознанное упрощение, а не недосмотр:
/// поддержка IPv6 добавится вместе с боевым Gateway, там же, где TLS
/// и аутентификация.
struct Address {
    uint32_t host = 0;   // в порядке хоста, не сети
    uint16_t port = 0;

    static Address loopback(uint16_t port);
    /// Разобрать "127.0.0.1:9000" или "127.0.0.1". Пустой адрес при ошибке.
    static Address parse(const std::string& text, uint16_t defaultPort = 0);

    bool valid() const { return port != 0; }
    std::string toString() const;

    bool operator==(const Address& other) const {
        return host == other.host && port == other.port;
    }
    bool operator!=(const Address& other) const { return !(*this == other); }
    bool operator<(const Address& other) const {
        if (host != other.host) return host < other.host;
        return port < other.port;
    }
};

/// Неблокирующий UDP-сокет.
///
/// Неблокирующий всегда: сервер обязан крутить свой тик ровно 10 раз в
/// секунду независимо от того, пришло что-нибудь или нет. Сокет, который
/// умеет ждать, рано или поздно заставит ждать симуляцию.
class Socket {
public:
    Socket() = default;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    /// Открыть и привязать к порту. Порт 0 — выбрать любой свободный.
    bool open(uint16_t port);
    void close();
    bool valid() const { return handle_ != kInvalid; }

    /// Порт, на котором сокет реально сидит. Полезно при port = 0.
    uint16_t port() const { return port_; }

    /// Отправить датаграмму. false — отправить не удалось.
    ///
    /// UDP не гарантирует доставку, поэтому true означает только «ядро
    /// приняло пакет». Всё остальное — забота слоя подтверждений.
    bool send(const Address& to, const void* data, size_t size);

    /// Забрать одну датаграмму. 0 — очередь пуста (это не ошибка).
    size_t receive(Address& from, void* data, size_t capacity);

    /// Текст последней ошибки. Пустой, если ошибок не было.
    const std::string& error() const { return error_; }

private:
    static constexpr int64_t kInvalid = -1;

    int64_t handle_ = kInvalid;
    uint16_t port_ = 0;
    std::string error_;
};

/// Инициализация сетевой подсистемы.
///
/// Нужна только Windows (WSAStartup), но вызывать обязаны все: иначе код
/// «работает у меня» и падает у половины игроков. Повторные вызовы
/// безопасны, счётчик внутри.
bool initialiseSockets();
void shutdownSockets();

}  // namespace pw::net
