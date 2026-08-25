#include "pw/net/channel.h"

#include <cstring>

namespace pw::net {

namespace {

/// Слот сообщения по номеру. Номера идут подряд, поэтому остаток от деления
/// даёт свободный слот, пока очередь не переполнена.
uint32_t slotOf(uint16_t id, uint32_t count) { return uint32_t(id) % count; }

}  // namespace

ReliableChannel::Outgoing* ReliableChannel::findOutgoing(uint16_t id) {
    Outgoing& slot = outgoing_[slotOf(id, kMaxQueuedMessages)];
    if (!slot.used || slot.id != id) return nullptr;
    return &slot;
}

// ---------------------------------------------------------------------------
// Отправка
// ---------------------------------------------------------------------------

bool ReliableChannel::send(const void* data, size_t size) {
    if (size == 0 || size > kMaxMessageSize) return false;

    Outgoing& slot = outgoing_[slotOf(nextOutgoingId_, kMaxQueuedMessages)];
    // Слот занят неподтверждённым сообщением — очередь сделала полный круг.
    // Значит собеседник давно молчит, и врать «отправлено» нельзя.
    if (slot.used) return false;

    slot.id = nextOutgoingId_++;
    slot.used = true;
    slot.inFlight = false;
    slot.payload.assign(static_cast<const uint8_t*>(data),
                        static_cast<const uint8_t*>(data) + size);
    return true;
}

uint32_t ReliableChannel::pack(ByteWriter& writer, uint16_t packetSequence) {
    PacketRecord& record = packets_[packetSequence % kPacketSlots];
    record.sequence = packetSequence;
    record.used = true;
    record.ids.clear();

    // Счётчик сообщений идёт первым, а известен он только после укладки.
    // Поэтому сообщения сначала собираются в черновик, а в пакет уходят
    // счётчиком и черновиком следом.
    //
    // Размер каждого сообщения считается ДО записи в черновик. Первая версия
    // писала его и откатывалась при нехватке места — но откатываться
    // ByteWriter не умеет, и в пакет уезжал черновик с лишним сообщением:
    // счётчик говорил N, данных было N+1, пакет переполнялся и получатель
    // отвергал его целиком. Тесный канал переставал работать вообще,
    // а просторный вёл себя нормально.
    uint8_t staging[2048];
    ByteWriter draft(staging, sizeof(staging));

    // Место под счётчик: сообщений в пакете не больше kMaxQueuedMessages.
    const size_t countReserve = varintSize(kMaxQueuedMessages);

    uint32_t count = 0;
    // Порядок обхода — по номеру сообщения от самого старого. Так канал
    // не голодает: старое сообщение не может застрять навсегда позади
    // непрерывного потока новых.
    for (uint16_t offset = 0; offset < kMaxQueuedMessages; ++offset) {
        const uint16_t id = uint16_t(nextOutgoingId_ - kMaxQueuedMessages + offset);
        Outgoing* message = findOutgoing(id);
        if (message == nullptr || message->inFlight) continue;

        const size_t payloadSize = message->payload.size();
        const size_t entry = varintSize(id) + varintSize(payloadSize) + payloadSize;

        if (draft.size() + entry > sizeof(staging)) break;
        if (writer.size() + countReserve + draft.size() + entry > writer.capacity()) break;

        draft.varint(id);
        draft.varint(payloadSize);
        draft.bytes(message->payload.data(), payloadSize);

        message->inFlight = true;
        record.ids.push_back(id);
        ++count;
    }

    writer.varint(count);
    if (count > 0) writer.bytes(draft.data(), draft.size());
    return count;
}

void ReliableChannel::onPacketAcked(uint16_t packetSequence) {
    PacketRecord& record = packets_[packetSequence % kPacketSlots];
    if (!record.used || record.sequence != packetSequence) return;

    for (uint16_t id : record.ids) {
        Outgoing* message = findOutgoing(id);
        if (message == nullptr) continue;
        // Сообщение доставлено: освобождаем слот целиком.
        message->used = false;
        message->inFlight = false;
        message->payload.clear();
        message->payload.shrink_to_fit();
    }
    record.used = false;
    record.ids.clear();
}

void ReliableChannel::onPacketLost(uint16_t packetSequence) {
    PacketRecord& record = packets_[packetSequence % kPacketSlots];
    if (!record.used || record.sequence != packetSequence) return;

    for (uint16_t id : record.ids) {
        Outgoing* message = findOutgoing(id);
        if (message == nullptr) continue;
        // Снимаем отметку «в пути»: сообщение поедет со следующим пакетом.
        message->inFlight = false;
    }
    record.used = false;
    record.ids.clear();
}

// ---------------------------------------------------------------------------
// Приём
// ---------------------------------------------------------------------------

bool ReliableChannel::unpack(ByteReader& reader) {
    const uint64_t count = reader.varint();
    if (reader.failed()) return false;
    // Верхняя граница на число сообщений в пакете: без неё одно поле
    // заставило бы нас крутить цикл сколько угодно раз.
    if (count > kMaxQueuedMessages) return false;

    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t rawId = reader.varint();
        const uint64_t size = reader.varint();
        if (reader.failed()) return false;
        if (rawId > 0xFFFF || size == 0 || size > kMaxMessageSize) return false;
        if (size > reader.remaining()) return false;

        const uint16_t id = uint16_t(rawId);

        // Скобочная инициализация здесь читалась бы компилятором как
        // объявление функции — тот самый most vexing parse.
        std::vector<uint8_t> payload;
        payload.resize(size_t(size));
        reader.bytes(payload.data(), payload.size());
        if (reader.failed()) return false;

        // Уже отданное наверх приходить не должно, но UDP дублирует пакеты,
        // а повтор идёт по нашей же воле. Просто молча пропускаем.
        if (sequenceLessThan(id, nextExpectedId_)) continue;
        // Слишком далеко вперёд — придержать негде. Отправитель дошлёт:
        // сообщение остаётся у него неподтверждённым.
        if (uint16_t(id - nextExpectedId_) >= kReorderWindow) continue;

        Incoming& slot = incoming_[slotOf(id, kReorderWindow)];
        if (slot.used && slot.id == id) continue;   // дубль

        slot.id = id;
        slot.used = true;
        slot.payload = std::move(payload);
    }
    return true;
}

bool ReliableChannel::receive(std::vector<uint8_t>& out) {
    Incoming& slot = incoming_[slotOf(nextExpectedId_, kReorderWindow)];
    // Порядок строгий: пока не пришло сообщение N, N+1 наверх не уходит,
    // даже если давно лежит рядом. Иначе команды игрока применились бы
    // не в том порядке, в котором он их отдавал.
    if (!slot.used || slot.id != nextExpectedId_) return false;

    out = std::move(slot.payload);
    slot.used = false;
    slot.payload.clear();
    ++nextExpectedId_;
    return true;
}

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------

uint32_t ReliableChannel::queued() const {
    uint32_t total = 0;
    for (const Outgoing& slot : outgoing_) {
        if (slot.used) ++total;
    }
    return total;
}

uint32_t ReliableChannel::held() const {
    uint32_t total = 0;
    for (const Incoming& slot : incoming_) {
        if (slot.used) ++total;
    }
    return total;
}

}  // namespace pw::net
