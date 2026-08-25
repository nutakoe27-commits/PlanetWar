// pw_sim — тик и расписание систем.
//
// Система — это функция, обрабатывающая мир за один тик. Расписание хранит
// их в порядке добавления и выполняет строго в нём.
//
// Порядок явный и неизменный, потому что от него зависит результат. Если бы
// системы запускались в порядке хеш-таблицы или по мере готовности, мир
// начал бы зависеть от раскладки памяти и от планировщика потоков — то есть
// перестал бы воспроизводиться.
//
// Параллелизм появляется ВНУТРИ системы (обход разбивается на куски через
// job system из pw_core), а не между системами. Тогда порядок применения
// результатов остаётся под нашим контролем.
#pragma once

#include <cstdint>
#include <vector>

#include "pw/core/fixed.h"
#include "pw/sim/world.h"

namespace pw::sim {

/// Частота тика горячих систем: тех, где есть онлайн-игроки, бой или осада.
/// Тёплые тикают раз в секунду, холодные не тикают вовсе и досчитываются
/// при чтении — см. docs/03-NETWORK-AND-SERVER.md.
inline constexpr int64_t kTicksPerSecond = 10;

/// Что система знает о текущем тике.
///
/// Шаг времени — фиксированная точная дробь, а не измеренное время кадра.
/// Переменный шаг сделал бы результат зависящим от нагрузки на машину,
/// и два клиента посчитали бы разное.
struct TickContext {
    uint64_t tick = 0;
    fx delta = fx::fromFraction(1, kTicksPerSecond);

    /// Игровое время от начала сезона, в секундах.
    fx elapsed() const { return fx::fromFraction(int64_t(tick), kTicksPerSecond); }
};

class Schedule {
public:
    using SystemFn = void (*)(World&, const TickContext&);

    /// Имя нужно для профилирования и для сообщений об ошибках: когда мир
    /// разъезжается, первым делом хочется знать, на какой системе.
    void add(const char* name, SystemFn function);

    void run(World& world, const TickContext& context) const;

    size_t systemCount() const { return systems_.size(); }
    const char* systemName(size_t index) const { return systems_[index].name; }

private:
    struct Entry {
        const char* name;
        SystemFn function;
    };
    std::vector<Entry> systems_;
};

/// Мир, расписание и счётчик тиков вместе.
///
/// Это и есть то, что компилируется в headless-сервер без единой строки
/// рендера, и то же самое исполняет клиент. Один код правил на обоих концах —
/// расхождение баланса между ними технически невозможно.
class Simulation {
public:
    World& world() { return world_; }
    const World& world() const { return world_; }
    Schedule& schedule() { return schedule_; }

    uint64_t tick() const { return tick_; }

    /// Один тик.
    void step();
    /// Несколько тиков подряд.
    void advance(uint64_t ticks);

    /// Хеш состояния: мир плюс номер тика.
    uint64_t hash() const;

private:
    World world_;
    Schedule schedule_;
    uint64_t tick_ = 0;
};

}  // namespace pw::sim
