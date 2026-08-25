#include "pw/net/sequence.h"

namespace pw::net {

bool AckTracker::onReceived(uint16_t sequence) {
    if (!started_) {
        started_ = true;
        latest_ = sequence;
        bits_ = 0;
        return true;
    }

    if (sequenceGreaterThan(sequence, latest_)) {
        // Пакет новее известного: сдвигаем окно. Сдвиг больше 32 обнуляет
        // поле целиком — всё, что было, вышло за окно.
        const int32_t shift = sequenceDifference(sequence, latest_);
        if (shift >= int32_t(kAckWindow)) {
            bits_ = 0;
        } else {
            bits_ = (bits_ << shift) | (1u << (shift - 1));
        }
        latest_ = sequence;
        return true;
    }

    // Пакет старее или равен известному: либо дубль, либо пришёл не по
    // порядку. Дубль обязан быть отброшен — иначе команда игрока применится
    // дважды, а это уже не «мелкая сетевая шероховатость».
    const int32_t back = sequenceDifference(latest_, sequence);
    if (back == 0) return false;
    if (back > int32_t(kAckWindow)) return false;   // слишком стар, окна нет

    const uint32_t bit = 1u << (back - 1);
    if (bits_ & bit) return false;                  // уже был
    bits_ |= bit;
    return true;
}

bool AckTracker::received(uint16_t sequence) const {
    if (!started_) return false;
    if (sequence == latest_) return true;
    if (sequenceGreaterThan(sequence, latest_)) return false;

    const int32_t back = sequenceDifference(latest_, sequence);
    if (back <= 0 || back > int32_t(kAckWindow)) return false;
    return (bits_ & (1u << (back - 1))) != 0;
}

uint16_t SentTracker::next() {
    const uint16_t sequence = nextSequence_++;
    Slot& slot = slotFor(sequence);
    slot.sequence = sequence;
    slot.state = State::Pending;
    return sequence;
}

uint32_t SentTracker::pending() const {
    uint32_t total = 0;
    for (const Slot& slot : slots_) {
        if (slot.state == State::Pending) ++total;
    }
    return total;
}

}  // namespace pw::net
