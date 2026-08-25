#include "pw/net/connection.h"

#include <algorithm>

namespace pw::net {

namespace {

/// Общая часть заголовка: метка протокола и тип пакета.
void writeHeader(ByteWriter& writer, PacketType type) {
    writer.u32(kProtocolId);
    writer.u8(uint8_t(type));
}

/// Разобрать общую часть. false — пакет чужой.
bool readHeader(ByteReader& reader, PacketType& type) {
    if (reader.u32() != kProtocolId) return false;
    const uint8_t raw = reader.u8();
    if (reader.failed()) return false;
    if (raw < uint8_t(PacketType::Request) || raw > uint8_t(PacketType::Disconnect)) {
        return false;
    }
    type = PacketType(raw);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Жизненный цикл
// ---------------------------------------------------------------------------

void Connection::connect(const Address& server, int64_t now) {
    *this = Connection{};
    peer_ = server;
    state_ = ConnectionState::Connecting;
    connectStarted_ = now;
    lastReceived_ = now;
    // Минус интервал: первый запрос уходит сразу, а не через четверть секунды.
    lastRequest_ = now - kRequestRetryMilliseconds;
}

void Connection::accept(const Address& client, uint32_t playerId, int64_t now) {
    *this = Connection{};
    peer_ = client;
    playerId_ = playerId;
    state_ = ConnectionState::Connected;
    lastReceived_ = now;
    lastSent_ = now - kKeepAliveMilliseconds;
    needAck_ = true;
    needAccept_ = true;
}

void Connection::disconnect() {
    state_ = ConnectionState::Disconnected;
    reason_ = DisconnectReason::Closed;
}

void Connection::fail(DisconnectReason reason) {
    state_ = ConnectionState::Failed;
    reason_ = reason;
}

void Connection::update(int64_t now) {
    if (state_ == ConnectionState::Connecting) {
        if (now - connectStarted_ >= kConnectTimeoutMilliseconds) fail(DisconnectReason::Timeout);
        return;
    }
    if (state_ != ConnectionState::Connected) return;
    if (now - lastReceived_ >= kTimeoutMilliseconds) fail(DisconnectReason::Timeout);
}

// ---------------------------------------------------------------------------
// Приём
// ---------------------------------------------------------------------------

bool Connection::parseRequest(const uint8_t* data, size_t size, uint16_t& version) {
    ByteReader reader(data, size);
    PacketType type = PacketType::Payload;
    if (!readHeader(reader, type)) return false;
    if (type != PacketType::Request) return false;
    version = reader.u16();
    return !reader.failed();
}

bool Connection::receive(const uint8_t* data, size_t size, int64_t now, ReceivedPacket& out) {
    ByteReader reader(data, size);
    PacketType type = PacketType::Payload;
    if (!readHeader(reader, type)) return false;
    out.type = type;
    out.payload.clear();

    switch (type) {
        case PacketType::Accept: {
            if (state_ != ConnectionState::Connecting) return false;
            const uint32_t playerId = reader.u32();
            if (reader.failed()) return false;
            playerId_ = playerId;
            state_ = ConnectionState::Connected;
            lastReceived_ = now;
            needAck_ = true;
            return true;
        }

        case PacketType::Reject: {
            if (state_ != ConnectionState::Connecting) return false;
            const uint8_t raw = reader.u8();
            if (reader.failed()) return false;
            rejectReason_ = RejectReason(raw);
            fail(DisconnectReason::Rejected);
            return true;
        }

        case PacketType::Disconnect: {
            if (state_ == ConnectionState::Disconnected) return false;
            state_ = ConnectionState::Disconnected;
            reason_ = DisconnectReason::Closed;
            return true;
        }

        case PacketType::Payload: {
            if (state_ != ConnectionState::Connected) return false;

            const uint16_t sequence = reader.u16();
            const uint16_t ack = reader.u16();
            const uint32_t ackBits = reader.u32();
            if (reader.failed()) return false;

            // Дубль или слишком старый пакет: разбирать нельзя. Иначе
            // команда игрока применилась бы дважды.
            if (!acks_.onReceived(sequence)) return false;

            sent_.onAck(ack, ackBits,
                        [&](uint16_t s) { onAcked(s, now); },
                        [&](uint16_t s) { onLost(s); });

            if (!channel_.unpack(reader)) return false;

            const uint64_t snapshotSize = reader.varint();
            if (reader.failed()) return false;
            if (snapshotSize > reader.remaining()) return false;
            if (snapshotSize > 0) {
                out.payload.resize(size_t(snapshotSize));
                reader.bytes(out.payload.data(), out.payload.size());
                if (reader.failed()) return false;
            }

            lastReceived_ = now;
            // Подтверждение обязано уехать: собеседник ждёт именно его,
            // чтобы понять, дошли ли его команды.
            needAck_ = true;
            return true;
        }

        case PacketType::Request:
            // Запрос разбирается отдельно — parseRequest, до создания
            // соединения. Здесь он означает, что клиент не увидел Accept
            // и повторяет попытку: отвечаем ещё раз, состояние не трогаем.
            if (state_ != ConnectionState::Connected) return false;
            needAccept_ = true;
            lastReceived_ = now;
            return true;
    }
    return false;
}

void Connection::onAcked(uint16_t sequence, int64_t now) {
    channel_.onPacketAcked(sequence);
    ++ackedCount_;

    const int64_t when = sentAt_[sequence % kTimeSlots];
    if (when <= 0) return;
    const int64_t sample = now - when;
    if (sample < 0) return;

    // Экспоненциальное сглаживание: одна задержавшаяся датаграмма не должна
    // сдвигать оценку, а устойчивое ухудшение канала — должно.
    roundTrip_ = roundTrip_ == 0 ? sample : (roundTrip_ * 7 + sample) / 8;
}

void Connection::onLost(uint16_t sequence) {
    channel_.onPacketLost(sequence);
    ++lostCount_;
}

uint32_t Connection::lossPercent() const {
    const uint32_t total = ackedCount_ + lostCount_;
    if (total == 0) return 0;
    return lostCount_ * 100 / total;
}

// ---------------------------------------------------------------------------
// Отправка
// ---------------------------------------------------------------------------

bool Connection::shouldSend(int64_t now) const {
    if (state_ == ConnectionState::Connecting) {
        return now - lastRequest_ >= kRequestRetryMilliseconds;
    }
    if (state_ != ConnectionState::Connected) return false;
    if (needAccept_) return true;
    if (needAck_) return true;
    if (!channel_.empty()) return true;
    return now - lastSent_ >= kKeepAliveMilliseconds;
}

size_t Connection::build(uint8_t* buffer, size_t capacity, int64_t now,
                         const void* snapshot, size_t snapshotSize) {
    if (state_ == ConnectionState::Connecting) {
        ByteWriter writer(buffer, capacity);
        writeHeader(writer, PacketType::Request);
        writer.u16(kProtocolVersion);
        if (writer.overflowed()) return 0;
        lastRequest_ = now;
        return writer.size();
    }

    if (state_ != ConnectionState::Connected) return 0;

    // Accept уезжает отдельным пакетом и повторяется, пока клиент не
    // подтвердит его, прислав рабочий пакет.
    if (needAccept_) {
        ByteWriter writer(buffer, capacity);
        writeHeader(writer, PacketType::Accept);
        writer.u32(playerId_);
        if (writer.overflowed()) return 0;
        needAccept_ = false;
        lastSent_ = now;
        return writer.size();
    }

    const uint16_t sequence = sent_.next();
    sentAt_[sequence % kTimeSlots] = now;

    ByteWriter writer(buffer, capacity);
    writeHeader(writer, PacketType::Payload);
    writer.u16(sequence);
    writer.u16(acks_.latest());
    writer.u32(acks_.bits());

    channel_.pack(writer, sequence);

    // Снапшот идёт последним и без надёжности: свежий полностью заменяет
    // прошлый, поэтому пересылать потерянный незачем — только зря занимать
    // канал устаревшими данными.
    const size_t room = writer.capacity() - writer.size();
    const size_t fits = snapshot != nullptr && snapshotSize > 0 &&
                                snapshotSize + varintSize(snapshotSize) <= room
                            ? snapshotSize
                            : 0;
    writer.varint(fits);
    if (fits > 0) writer.bytes(snapshot, fits);

    if (writer.overflowed()) return 0;

    lastSent_ = now;
    needAck_ = false;
    return writer.size();
}

size_t Connection::buildDisconnect(uint8_t* buffer, size_t capacity) const {
    ByteWriter writer(buffer, capacity);
    writeHeader(writer, PacketType::Disconnect);
    return writer.overflowed() ? 0 : writer.size();
}

size_t Connection::buildReject(uint8_t* buffer, size_t capacity, RejectReason reason) {
    ByteWriter writer(buffer, capacity);
    writeHeader(writer, PacketType::Reject);
    writer.u8(uint8_t(reason));
    return writer.overflowed() ? 0 : writer.size();
}

// ---------------------------------------------------------------------------
// Надёжные сообщения
// ---------------------------------------------------------------------------

bool Connection::sendReliable(const void* data, size_t size) {
    if (state_ != ConnectionState::Connected) return false;
    return channel_.send(data, size);
}

bool Connection::receiveReliable(std::vector<uint8_t>& out) { return channel_.receive(out); }

}  // namespace pw::net
