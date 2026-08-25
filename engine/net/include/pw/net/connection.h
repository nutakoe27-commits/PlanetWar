#pragma once

// Соединение: одна пара «клиент — сервер» поверх UDP.
//
// Собирает вместе всё, что лежит рядом: заголовок пакета, номера и
// подтверждения (sequence.h), надёжный канал команд (channel.h) и
// ненадёжную полезную нагрузку — снапшоты состояния.
//
// ВРЕМЯ ПРИХОДИТ СНАРУЖИ. Ни одного обращения к часам внутри: соединение
// получает «сейчас» параметром. Без этого его нельзя было бы проверить
// иначе, чем настоящими паузами, — а значит, таймауты и повторные отправки
// проверялись бы плохо или не проверялись вообще. Здесь же тест
// прокручивает сутки за миллисекунду.
//
// СОЕДИНЕНИЕ НЕ ЗНАЕТ ПРО СОКЕТЫ. Оно отдаёт готовый пакет и принимает
// пришедший; кто их носит — дело вызывающего. Поэтому весь протокол
// проверяется симулятором плохой сети, без единого системного вызова.

#include <cstdint>
#include <vector>

#include "pw/net/bitstream.h"
#include "pw/net/channel.h"
#include "pw/net/sequence.h"
#include "pw/net/socket.h"

namespace pw::net {

/// Метка протокола в начале каждого пакета.
///
/// Отсеивает чужой трафик на порт до всякого разбора: в интернете на любой
/// открытый UDP-порт стучатся сканеры. Заодно ловит попытку соединить
/// клиент и сервер разных версий.
inline constexpr uint32_t kProtocolId = 0x50573031;   // "PW01"

/// Версия протокола. Растёт при любой несовместимой правке формата.
inline constexpr uint16_t kProtocolVersion = 1;

/// Тип пакета. Влезает в байт, потому что типов немного и не будет много:
/// всё разнообразие живёт в сообщениях внутри, а не в конверте.
enum class PacketType : uint8_t {
    /// Клиент просит соединение.
    Request = 1,
    /// Сервер принимает и сообщает номер игрока.
    Accept = 2,
    /// Сервер отказывает и объясняет причину.
    Reject = 3,
    /// Рабочий пакет: подтверждения, надёжные сообщения, снапшот.
    Payload = 4,
    /// Разрыв по желанию любой из сторон.
    Disconnect = 5,
};

/// Почему сервер отказал.
enum class RejectReason : uint8_t {
    None = 0,
    ServerFull = 1,
    VersionMismatch = 2,
    Banned = 3,
};

enum class ConnectionState : uint8_t {
    Disconnected = 0,
    /// Клиент послал запрос и ждёт ответа.
    Connecting = 1,
    Connected = 2,
    /// Разорвано; причина в reason().
    Failed = 3,
};

/// Почему соединение закончилось.
enum class DisconnectReason : uint8_t {
    None = 0,
    /// Собеседник молчит дольше kTimeoutSeconds.
    Timeout = 1,
    /// Собеседник попрощался.
    Closed = 2,
    /// Сервер отказал при подключении.
    Rejected = 3,
};

/// Через сколько молчания соединение считается разорванным.
///
/// Пять секунд — компромисс: меньше, и мобильный игрок будет вылетать на
/// каждом переключении вышки; больше, и место в лобби после вылета
/// освободится непозволительно поздно.
inline constexpr int64_t kTimeoutMilliseconds = 5000;

/// Как часто слать пакет, даже если сказать нечего.
///
/// Молчание неотличимо от обрыва, поэтому тишины в протоколе нет: пустой
/// пакет раз в сотню миллисекунд стоит меньше, чем ложный разрыв.
inline constexpr int64_t kKeepAliveMilliseconds = 100;

/// Как часто клиент повторяет запрос на соединение.
inline constexpr int64_t kRequestRetryMilliseconds = 250;

/// Сколько времени клиент ждёт ответа, прежде чем сдаться.
inline constexpr int64_t kConnectTimeoutMilliseconds = 5000;

/// Что делать с пришедшим пакетом, решает вызывающий.
struct ReceivedPacket {
    PacketType type = PacketType::Payload;
    /// Ненадёжная часть пакета: снапшот состояния. Пуста, если её не было.
    std::vector<uint8_t> payload;
};

/// Одно соединение. И на клиенте, и на сервере — один и тот же класс.
///
/// Разница между сторонами только в том, кто начинает рукопожатие, поэтому
/// отдельных классов нет: два почти одинаковых куска кода разъезжаются
/// быстрее, чем один с флагом.
class Connection {
public:
    // --- жизненный цикл ---

    /// Начать подключение к серверу (сторона клиента).
    void connect(const Address& server, int64_t now);
    /// Принять подключение (сторона сервера, после разбора Request).
    void accept(const Address& client, uint32_t playerId, int64_t now);
    /// Попрощаться. Пакет Disconnect надо отправить самому — см. buildDisconnect.
    void disconnect();

    ConnectionState state() const { return state_; }
    DisconnectReason reason() const { return reason_; }
    RejectReason rejectReason() const { return rejectReason_; }
    const Address& peer() const { return peer_; }
    uint32_t playerId() const { return playerId_; }

    // --- обмен ---

    /// Разобрать пришедший пакет.
    ///
    /// false означает «пакет не наш или битый» — вызывающий обязан молча
    /// его выбросить. Отвечать на мусор нельзя: это превратило бы сервер
    /// в усилитель чужого трафика.
    bool receive(const uint8_t* data, size_t size, int64_t now, ReceivedPacket& out);

    /// Нужно ли прямо сейчас отправить пакет.
    ///
    /// True при накопившихся сообщениях, при подтверждениях, которых
    /// собеседник ещё не видел, и по расписанию keepalive.
    bool shouldSend(int64_t now) const;

    /// Собрать очередной пакет. Возвращает его размер, 0 — нечего слать.
    ///
    /// `snapshot` — ненадёжная часть: свежий снапшот полностью заменяет
    /// прошлый, поэтому пересылать потерянный незачем.
    size_t build(uint8_t* buffer, size_t capacity, int64_t now,
                 const void* snapshot = nullptr, size_t snapshotSize = 0);

    /// Собрать пакет прощания.
    size_t buildDisconnect(uint8_t* buffer, size_t capacity) const;
    /// Собрать отказ (сторона сервера).
    static size_t buildReject(uint8_t* buffer, size_t capacity, RejectReason reason);

    /// Проверить таймаут. Вызывать каждый тик.
    void update(int64_t now);

    // --- надёжные сообщения ---

    /// Поставить команду в очередь. false — очередь переполнена.
    bool sendReliable(const void* data, size_t size);
    /// Забрать следующую пришедшую команду.
    bool receiveReliable(std::vector<uint8_t>& out);

    // --- наблюдение ---

    /// Оценка задержки туда-обратно, миллисекунды. 0, пока нечего мерить.
    int64_t roundTrip() const { return roundTrip_; }
    uint32_t queued() const { return channel_.queued(); }
    /// Доля потерянных пакетов, проценты.
    uint32_t lossPercent() const;

    /// Разобрать пакет Request, не имея соединения (сторона сервера).
    /// Возвращает false, если это не запрос или версия не та.
    static bool parseRequest(const uint8_t* data, size_t size, uint16_t& version);

private:
    ConnectionState state_ = ConnectionState::Disconnected;
    DisconnectReason reason_ = DisconnectReason::None;
    RejectReason rejectReason_ = RejectReason::None;
    Address peer_;
    uint32_t playerId_ = 0;

    SentTracker sent_;
    AckTracker acks_;
    ReliableChannel channel_;

    int64_t lastReceived_ = 0;
    int64_t lastSent_ = 0;
    int64_t connectStarted_ = 0;
    int64_t lastRequest_ = 0;

    // Время отправки каждого пакета — для оценки задержки. Кольцо на то же
    // окно, что и подтверждения: судьба пакета решается внутри него.
    static constexpr uint32_t kTimeSlots = kAckWindow * 2;
    int64_t sentAt_[kTimeSlots] = {};

    int64_t roundTrip_ = 0;
    uint32_t ackedCount_ = 0;
    uint32_t lostCount_ = 0;

    bool needAck_ = false;
    // Сервер обязан отправить Accept и повторить его, если клиент не увидел
    // и прислал запрос заново. Без повтора потеря одного пакета означала бы,
    // что игрок висит на «подключаюсь» до самого таймаута.
    bool needAccept_ = false;

    void onAcked(uint16_t sequence, int64_t now);
    void onLost(uint16_t sequence);
    void fail(DisconnectReason reason);
};

}  // namespace pw::net
