#pragma once

// Номера пакетов и подтверждения.
//
// Транспорт — UDP: пакеты теряются, приходят не по порядку и дублируются.
// Этот файл — про то, как из такого потока получить честный ответ на вопрос
// «дошёл ли мой пакет номер N».
//
// Схема классическая (Quake 3 / Source): в каждом пакете едет номер
// последнего полученного от собеседника пакета и битовое поле на 32 пакета
// до него. Одно подтверждение подтверждает до 33 пакетов сразу, поэтому
// потеря самого подтверждения ничего не ломает — следующее договорит.
//
// Отдельных пакетов-подтверждений нет: подтверждение едет попутно в любом
// пакете. При 10 Гц тика это ноль дополнительного трафика.
//
// НОМЕРА ШЕСТНАДЦАТИБИТНЫЕ И ПЕРЕПОЛНЯЮТСЯ. При 10 Гц круг проходится
// за 109 минут, и в середине сезона номер 3 идёт ПОСЛЕ номера 65535.
// Поэтому сравнивать их обычным < нельзя — для этого здесь
// sequenceGreaterThan.

#include <cstdint>

namespace pw::net {

/// Сколько пакетов назад помнит битовое поле подтверждений.
inline constexpr uint32_t kAckWindow = 32;

/// Больше ли `a`, чем `b`, с учётом переполнения.
///
/// Половина кольца считается «вперёд», половина — «назад». Это единственный
/// корректный способ сравнить номера, которые ходят по кругу: разрыв больше
/// половины кольца означает, что мы смотрим на переполнение, а не на скачок.
constexpr bool sequenceGreaterThan(uint16_t a, uint16_t b) {
    constexpr uint16_t half = 32768;
    return (a > b && uint16_t(a - b) <= half) || (a < b && uint16_t(b - a) > half);
}

constexpr bool sequenceLessThan(uint16_t a, uint16_t b) { return sequenceGreaterThan(b, a); }

/// Разница a - b с учётом кольца. Знаковая: отрицательная, если a раньше b.
constexpr int32_t sequenceDifference(uint16_t a, uint16_t b) {
    return int32_t(int16_t(uint16_t(a - b)));
}

// ---------------------------------------------------------------------------
// Приём: что мы получили от собеседника
// ---------------------------------------------------------------------------

/// Помнит, какие пакеты пришли, и собирает из этого подтверждение.
class AckTracker {
public:
    /// Отметить пришедший пакет.
    ///
    /// Возвращает false, если пакет уже был или слишком стар: UDP дублирует
    /// пакеты, и обработать один и тот же дважды — значит применить команду
    /// игрока дважды.
    bool onReceived(uint16_t sequence);

    /// Номер последнего полученного пакета — едет в каждом исходящем.
    uint16_t latest() const { return latest_; }
    /// Битовое поле: бит i означает «получен пакет latest - 1 - i».
    uint32_t bits() const { return bits_; }

    bool received(uint16_t sequence) const;

private:
    uint16_t latest_ = 0;
    uint32_t bits_ = 0;
    bool started_ = false;
};

// ---------------------------------------------------------------------------
// Отправка: что собеседник получил от нас
// ---------------------------------------------------------------------------

/// Результат разбора чужого подтверждения.
struct AckResult {
    /// Сколько наших пакетов подтвердилось этим сообщением.
    uint32_t acked = 0;
    /// Сколько признано потерянными: они вышли из окна неподтверждёнными.
    uint32_t lost = 0;
};

/// Следит за судьбой отправленных пакетов.
///
/// Пакет считается потерянным не по таймауту, а по выходу из окна: если
/// собеседник подтвердил номер на kAckWindow позже нашего, а наш всё ещё
/// не подтверждён, договорить уже нечем.
class SentTracker {
public:
    /// Занять номер для нового исходящего пакета.
    uint16_t next();

    /// Применить подтверждение собеседника.
    ///
    /// `onAcked` и `onLost` вызываются по одному разу на пакет: повторное
    /// подтверждение того же номера ничего не вызывает. На этом держится
    /// слой повторной отправки — иначе одно сообщение слалось бы вечно.
    template <typename OnAcked, typename OnLost>
    AckResult onAck(uint16_t ack, uint32_t bits, OnAcked onAcked, OnLost onLost);

    uint16_t lastSent() const { return uint16_t(nextSequence_ - 1); }
    /// Сколько пакетов отправлено и пока никак не разрешилось.
    uint32_t pending() const;

private:
    // Кольцевой буфер состояний. Размер вдвое больше окна подтверждений:
    // так пакет успевает дожить до момента, когда его судьба ясна.
    static constexpr uint32_t kSlots = kAckWindow * 2;

    enum class State : uint8_t { Free, Pending, Resolved };

    struct Slot {
        uint16_t sequence = 0;
        State state = State::Free;
    };

    Slot slots_[kSlots];
    uint16_t nextSequence_ = 0;

    Slot& slotFor(uint16_t sequence) { return slots_[sequence % kSlots]; }
    const Slot& slotFor(uint16_t sequence) const { return slots_[sequence % kSlots]; }
};

template <typename OnAcked, typename OnLost>
AckResult SentTracker::onAck(uint16_t ack, uint32_t bits, OnAcked onAcked, OnLost onLost) {
    AckResult result;

    // Разбираем подтверждение: сначала сам ack, потом биты за ним.
    for (uint32_t offset = 0; offset <= kAckWindow; ++offset) {
        if (offset > 0 && (bits & (1u << (offset - 1))) == 0) continue;

        const uint16_t sequence = uint16_t(ack - offset);
        Slot& slot = slotFor(sequence);
        if (slot.state != State::Pending || slot.sequence != sequence) continue;

        slot.state = State::Resolved;
        ++result.acked;
        onAcked(sequence);
    }

    // Всё, что старше окна и не подтвердилось, потеряно окончательно:
    // собеседник уже не может о нём рассказать.
    const uint16_t oldest = uint16_t(ack - kAckWindow);
    for (uint32_t i = 0; i < kSlots; ++i) {
        Slot& slot = slots_[i];
        if (slot.state != State::Pending) continue;
        if (!sequenceLessThan(slot.sequence, oldest)) continue;

        slot.state = State::Resolved;
        ++result.lost;
        onLost(slot.sequence);
    }
    return result;
}

}  // namespace pw::net
