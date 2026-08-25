#include "doctest.h"

#include <algorithm>
#include <set>
#include <vector>

#include "pw/core/rng.h"
#include "pw/net/sequence.h"

using namespace pw;
using namespace pw::net;

// ---------------------------------------------------------------------------
// Сравнение номеров через переполнение
//
// Это самое коварное место всего транспорта: при 10 Гц кольцо проходится за
// 109 минут, то есть КАЖДЫЙ сезон каждый игрок проходит через переполнение
// не один десяток раз. Ошибка здесь означала бы, что раз в два часа связь
// на секунду разваливается — и найти это на живом сервере почти невозможно.
// ---------------------------------------------------------------------------

TEST_CASE("номера: обычное сравнение внутри кольца") {
    CHECK(sequenceGreaterThan(1, 0));
    CHECK(sequenceGreaterThan(100, 42));
    CHECK_FALSE(sequenceGreaterThan(42, 100));
    CHECK_FALSE(sequenceGreaterThan(7, 7));
}

TEST_CASE("номера: 0 идёт ПОСЛЕ 65535") {
    CHECK(sequenceGreaterThan(0, 65535));
    CHECK(sequenceGreaterThan(3, 65535));
    CHECK_FALSE(sequenceGreaterThan(65535, 0));
    CHECK(sequenceLessThan(65530, 5));
}

TEST_CASE("номера: разность знаковая и переживает круг") {
    CHECK(sequenceDifference(10, 3) == 7);
    CHECK(sequenceDifference(3, 10) == -7);
    CHECK(sequenceDifference(2, 65535) == 3);
    CHECK(sequenceDifference(65535, 2) == -3);
}

TEST_CASE("номера: порядок сохраняется на всём круге") {
    // Прогоняем полный оборот и требуем, чтобы каждый следующий номер
    // считался большим предыдущего — включая момент переполнения.
    uint16_t previous = 0;
    for (uint32_t step = 1; step <= 70000; ++step) {
        const uint16_t current = uint16_t(step);
        CHECK(sequenceGreaterThan(current, previous));
        previous = current;
    }
}

// ---------------------------------------------------------------------------
// Приём
// ---------------------------------------------------------------------------

TEST_CASE("приём: подряд идущие пакеты набивают битовое поле") {
    AckTracker tracker;
    for (uint16_t sequence = 0; sequence < 10; ++sequence) {
        CHECK(tracker.onReceived(sequence));
    }
    CHECK(tracker.latest() == 9);
    for (uint16_t sequence = 0; sequence < 10; ++sequence) {
        CHECK(tracker.received(sequence));
    }
    CHECK_FALSE(tracker.received(10));
}

TEST_CASE("приём: дубль отбрасывается") {
    // UDP дублирует пакеты сам, без всякого злого умысла. Обработать команду
    // игрока дважды — значит построить два корабля вместо одного.
    AckTracker tracker;
    CHECK(tracker.onReceived(5));
    CHECK_FALSE(tracker.onReceived(5));

    CHECK(tracker.onReceived(6));
    CHECK_FALSE(tracker.onReceived(6));
    CHECK_FALSE(tracker.onReceived(5));
}

TEST_CASE("приём: пакет не по порядку принимается и запоминается") {
    AckTracker tracker;
    CHECK(tracker.onReceived(10));
    CHECK(tracker.onReceived(12));
    CHECK(tracker.latest() == 12);
    CHECK_FALSE(tracker.received(11));

    // Опоздавший 11 приходит позже — он всё ещё полезен.
    CHECK(tracker.onReceived(11));
    CHECK(tracker.received(11));
    CHECK(tracker.latest() == 12);   // окно не двигали назад

    CHECK_FALSE(tracker.onReceived(11));
}

TEST_CASE("приём: слишком старый пакет отбрасывается") {
    AckTracker tracker;
    tracker.onReceived(0);
    tracker.onReceived(uint16_t(kAckWindow + 5));
    // Пакет 0 давно вышел за окно — подтвердить его уже нечем.
    CHECK_FALSE(tracker.onReceived(0));
}

TEST_CASE("приём: большой скачок обнуляет окно, а не сдвигает мусор") {
    AckTracker tracker;
    for (uint16_t sequence = 0; sequence < 20; ++sequence) tracker.onReceived(sequence);
    REQUIRE(tracker.bits() != 0);

    tracker.onReceived(1000);
    CHECK(tracker.latest() == 1000);
    CHECK(tracker.bits() == 0);
    CHECK_FALSE(tracker.received(19));
}

TEST_CASE("приём: переполнение номера не сбрасывает связь") {
    AckTracker tracker;
    for (uint32_t step = 0; step < 200; ++step) {
        const uint16_t sequence = uint16_t(65500 + step);
        CHECK(tracker.onReceived(sequence));
        CHECK(tracker.latest() == sequence);
    }
    CHECK(tracker.received(uint16_t(65500 + 199)));
    CHECK(tracker.received(uint16_t(65500 + 199 - 5)));
}

// ---------------------------------------------------------------------------
// Отправка
// ---------------------------------------------------------------------------

namespace {

struct Log {
    std::vector<uint16_t> acked;
    std::vector<uint16_t> lost;

    auto onAcked() { return [this](uint16_t s) { acked.push_back(s); }; }
    auto onLost() { return [this](uint16_t s) { lost.push_back(s); }; }
};

}  // namespace

TEST_CASE("отправка: подтверждение закрывает пакет ровно один раз") {
    // На этом держится повторная отправка: если бы один и тот же пакет
    // подтверждался дважды, сообщение слалось бы вечно.
    SentTracker sent;
    const uint16_t a = sent.next();
    const uint16_t b = sent.next();
    CHECK(a == 0);
    CHECK(b == 1);
    CHECK(sent.pending() == 2);

    Log log;
    sent.onAck(1, 0b1, log.onAcked(), log.onLost());
    CHECK(log.acked.size() == 2);
    CHECK(log.lost.empty());
    CHECK(sent.pending() == 0);

    // Повторное подтверждение того же — тишина.
    Log again;
    sent.onAck(1, 0b1, again.onAcked(), again.onLost());
    CHECK(again.acked.empty());
    CHECK(again.lost.empty());
}

TEST_CASE("отправка: битовое поле подтверждает пропущенные") {
    SentTracker sent;
    for (int i = 0; i < 5; ++i) sent.next();   // 0..4

    Log log;
    // Собеседник получил 4, а также 2 и 0 (биты 1 и 3).
    sent.onAck(4, 0b1010, log.onAcked(), log.onLost());
    std::sort(log.acked.begin(), log.acked.end());
    CHECK(log.acked == std::vector<uint16_t>{0, 2, 4});
    CHECK(log.lost.empty());
}

TEST_CASE("отправка: пакет теряется, выйдя из окна") {
    // Потеря определяется не таймером, а тем, что собеседник ушёл вперёд
    // настолько, что рассказать о нашем пакете уже нечем.
    SentTracker sent;
    const uint16_t doomed = sent.next();
    for (uint32_t i = 0; i < kAckWindow + 10; ++i) sent.next();

    Log log;
    sent.onAck(uint16_t(kAckWindow + 10), 0, log.onAcked(), log.onLost());
    CHECK(std::find(log.lost.begin(), log.lost.end(), doomed) != log.lost.end());
}

TEST_CASE("отправка: пакет не может быть и подтверждён, и потерян") {
    SentTracker sent;
    std::vector<uint16_t> all;
    for (uint32_t i = 0; i < 200; ++i) all.push_back(sent.next());

    Log log;
    for (uint32_t ack = 0; ack < 200; ack += 7) {
        sent.onAck(uint16_t(ack), 0xFFFFFFFF, log.onAcked(), log.onLost());
    }

    std::set<uint16_t> acked(log.acked.begin(), log.acked.end());
    for (uint16_t sequence : log.lost) {
        CHECK(acked.count(sequence) == 0);
    }
    // И ни один номер не разрешился дважды.
    std::set<uint16_t> seen;
    for (uint16_t sequence : log.acked) CHECK(seen.insert(sequence).second);
    for (uint16_t sequence : log.lost) CHECK(seen.insert(sequence).second);
}

// ---------------------------------------------------------------------------
// Сеть целиком
// ---------------------------------------------------------------------------

TEST_CASE("сеть: подтверждения выживают при потерях, дублях и перестановке") {
    // Симулятор плохого канала. Проверяется главное свойство схемы: пакет,
    // который на самом деле дошёл, рано или поздно подтверждается, а пакет,
    // который не дошёл, рано или поздно объявляется потерянным. Не бывает
    // так, чтобы отправитель остался в неведении навсегда.
    struct Packet {
        uint16_t sequence;
        uint32_t deliverAt;
    };

    Rng rng(0xC0FFEE, /*stream=*/4);
    SentTracker sender;
    AckTracker receiver;

    std::set<uint16_t> reallyArrived;
    std::set<uint16_t> reportedAcked;
    std::set<uint16_t> reportedLost;
    std::vector<Packet> inFlight;

    constexpr uint32_t kSteps = 4000;
    for (uint32_t step = 0; step < kSteps; ++step) {
        // Отправляем пакет; двадцать процентов теряются, часть задерживается,
        // часть дублируется.
        const uint16_t sequence = sender.next();
        const uint32_t roll = uint32_t(rng.next() % 100);
        if (roll >= 20) {
            const uint32_t delay = uint32_t(rng.next() % 4);
            inFlight.push_back(Packet{sequence, step + delay});
            if (roll >= 95) inFlight.push_back(Packet{sequence, step + delay + 1});
        }

        // Доставляем всё, чей срок пришёл.
        for (auto it = inFlight.begin(); it != inFlight.end();) {
            if (it->deliverAt > step) { ++it; continue; }
            if (receiver.onReceived(it->sequence)) reallyArrived.insert(it->sequence);
            it = inFlight.erase(it);
        }

        // Подтверждение едет обратно. Его тоже иногда теряем.
        if (rng.next() % 100 >= 15) {
            sender.onAck(receiver.latest(), receiver.bits(),
                         [&](uint16_t s) { reportedAcked.insert(s); },
                         [&](uint16_t s) { reportedLost.insert(s); });
        }
    }

    // 1. Ни один пакет не объявлен одновременно дошедшим и потерянным.
    for (uint16_t sequence : reportedAcked) CHECK(reportedLost.count(sequence) == 0);

    // 2. Подтверждено только то, что действительно дошло. Ложное
    //    подтверждение хуже потери: отправитель выбросит данные, которых
    //    у собеседника нет.
    for (uint16_t sequence : reportedAcked) CHECK(reallyArrived.count(sequence) == 1);

    // 3. Схема вообще работает: подавляющее большинство дошедших пакетов
    //    подтвердилось, а не потерялось в учёте.
    CHECK(reportedAcked.size() > reallyArrived.size() * 9 / 10);

    // 4. Отправитель не копит вечно неразрешённые пакеты.
    CHECK(sender.pending() <= kAckWindow * 2);
}
