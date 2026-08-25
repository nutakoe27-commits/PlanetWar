#include "pw/net/socket.h"

#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
using SocketHandle = SOCKET;
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
using SocketHandle = int;
#endif

namespace pw::net {

namespace {

#if defined(_WIN32)
constexpr SocketHandle kBadSocket = INVALID_SOCKET;
int closeHandle(SocketHandle handle) { return ::closesocket(handle); }
bool wouldBlock() { return ::WSAGetLastError() == WSAEWOULDBLOCK; }
// Winsock берёт длину как int, POSIX — как size_t. Тип назван явно, чтобы
// приведение не превращалось в молчаливую смену знака.
using PayloadSize = int;
std::string lastErrorText() {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "код %d", ::WSAGetLastError());
    return buffer;
}
#else
constexpr SocketHandle kBadSocket = -1;
int closeHandle(SocketHandle handle) { return ::close(handle); }
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
using PayloadSize = size_t;
std::string lastErrorText() { return std::strerror(errno); }
#endif

int windowsUsers = 0;

}  // namespace

// ---------------------------------------------------------------------------
// Адрес
// ---------------------------------------------------------------------------

Address Address::loopback(uint16_t port) {
    Address address;
    address.host = 0x7F000001u;   // 127.0.0.1
    address.port = port;
    return address;
}

Address Address::parse(const std::string& text, uint16_t defaultPort) {
    Address address;
    address.port = defaultPort;

    std::string hostPart = text;
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        hostPart = text.substr(0, colon);
        const std::string portPart = text.substr(colon + 1);
        long parsed = 0;
        for (char c : portPart) {
            if (c < '0' || c > '9') return Address{};
            parsed = parsed * 10 + (c - '0');
            if (parsed > 65535) return Address{};
        }
        if (portPart.empty()) return Address{};
        address.port = uint16_t(parsed);
    }

    // Разбираем четыре октета вручную: inet_pton тянет разные заголовки на
    // разных системах, а формат здесь простой и его удобно проверять.
    uint32_t octets[4] = {0, 0, 0, 0};
    int index = 0;
    int digits = 0;
    for (char c : hostPart) {
        if (c == '.') {
            if (digits == 0 || index >= 3) return Address{};
            ++index;
            digits = 0;
            continue;
        }
        if (c < '0' || c > '9') return Address{};
        octets[index] = octets[index] * 10 + uint32_t(c - '0');
        if (octets[index] > 255) return Address{};
        ++digits;
    }
    if (index != 3 || digits == 0) return Address{};

    address.host = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return address;
}

std::string Address::toString() const {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u:%u",
                  (host >> 24) & 0xFF, (host >> 16) & 0xFF,
                  (host >> 8) & 0xFF, host & 0xFF, port);
    return buffer;
}

// ---------------------------------------------------------------------------
// Инициализация
// ---------------------------------------------------------------------------

bool initialiseSockets() {
#if defined(_WIN32)
    if (windowsUsers++ == 0) {
        WSADATA data;
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            windowsUsers = 0;
            return false;
        }
    }
#else
    ++windowsUsers;
#endif
    return true;
}

void shutdownSockets() {
    if (windowsUsers == 0) return;
    if (--windowsUsers > 0) return;
#if defined(_WIN32)
    ::WSACleanup();
#endif
}

// ---------------------------------------------------------------------------
// Сокет
// ---------------------------------------------------------------------------

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept
    : handle_(other.handle_), port_(other.port_), error_(std::move(other.error_)) {
    other.handle_ = kInvalid;
    other.port_ = 0;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        port_ = other.port_;
        error_ = std::move(other.error_);
        other.handle_ = kInvalid;
        other.port_ = 0;
    }
    return *this;
}

bool Socket::open(uint16_t port) {
    close();
    error_.clear();

    const SocketHandle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kBadSocket) {
        error_ = "не удалось создать сокет: " + lastErrorText();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error_ = "порт " + std::to_string(port) + " занят: " + lastErrorText();
        closeHandle(handle);
        return false;
    }

    // Неблокирующий режим. Сервер обязан крутить тик ровно 10 раз в секунду
    // независимо от того, пришло что-нибудь или нет.
#if defined(_WIN32)
    u_long nonBlocking = 1;
    if (::ioctlsocket(handle, FIONBIO, &nonBlocking) != 0) {
#else
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0) {
#endif
        error_ = "не удалось включить неблокирующий режим: " + lastErrorText();
        closeHandle(handle);
        return false;
    }

    // Реальный порт: при port = 0 его выбрала система, и мы обязаны знать
    // какой — иначе к нам не подключиться.
    sockaddr_in bound{};
#if defined(_WIN32)
    int boundSize = int(sizeof(bound));
#else
    socklen_t boundSize = sizeof(bound);
#endif
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &boundSize) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }

    handle_ = int64_t(handle);
    return true;
}

void Socket::close() {
    if (handle_ == kInvalid) return;
    closeHandle(SocketHandle(handle_));
    handle_ = kInvalid;
    port_ = 0;
}

bool Socket::send(const Address& to, const void* data, size_t size) {
    if (handle_ == kInvalid) return false;
    if (size == 0 || size > kMaxPacketSize) return false;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(to.host);
    address.sin_port = htons(to.port);

    const auto sent = ::sendto(SocketHandle(handle_),
                               static_cast<const char*>(data), static_cast<PayloadSize>(size), 0,
                               reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (sent < 0) {
        // Переполнение очереди отправки — не повод считать связь сломанной:
        // это ровно то, ради чего существует слой подтверждений.
        if (!wouldBlock()) error_ = "отправка не удалась: " + lastErrorText();
        return false;
    }
    return size_t(sent) == size;
}

size_t Socket::receive(Address& from, void* data, size_t capacity) {
    if (handle_ == kInvalid || capacity == 0) return 0;

    sockaddr_in address{};
#if defined(_WIN32)
    int addressSize = int(sizeof(address));
#else
    socklen_t addressSize = sizeof(address);
#endif

    const auto got = ::recvfrom(SocketHandle(handle_),
                                static_cast<char*>(data), static_cast<PayloadSize>(capacity), 0,
                                reinterpret_cast<sockaddr*>(&address), &addressSize);
    if (got <= 0) return 0;   // пустая очередь — обычное дело, не ошибка

    from.host = ntohl(address.sin_addr.s_addr);
    from.port = ntohs(address.sin_port);
    return size_t(got);
}

}  // namespace pw::net
