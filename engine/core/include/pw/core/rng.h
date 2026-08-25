// pw_core — детерминированный генератор псевдослучайных чисел.
//
// PCG32 (O'Neill, 2014): проходит статистические тесты, состояние — 128 бит,
// шаг — несколько целочисленных операций. std::mt19937 не подходит: стандарт
// не фиксирует поведение распределений (std::uniform_int_distribution даёт
// разные результаты в libstdc++ и libc++), а нам нужен одинаковый мир везде.
//
// ВАЖНО ДЛЯ ДЕТЕРМИНИЗМА: у каждой подсистемы должен быть СВОЙ поток
// случайности. Если бой и генератор аномалий тянут числа из общего потока,
// порядок их вызова начинает влиять на результат — и симуляция расходится
// при малейшем изменении планировщика.
#pragma once

#include <cstdint>

#include "pw/core/fixed.h"

namespace pw {

class Rng {
public:
    /// stream — идентификатор подсистемы. Разные stream дают независимые
    /// последовательности из одного сида сезона.
    constexpr explicit Rng(uint64_t seed = 0x853C49E6748FEA9Bull, uint64_t stream = 1)
        : inc_((stream << 1) | 1u) {
        step();
        state_ += seed;
        step();
    }

    constexpr uint32_t next() {
        const uint64_t prev = state_;
        step();
        const uint32_t xorshifted = uint32_t(((prev >> 18u) ^ prev) >> 27u);
        const uint32_t rot = uint32_t(prev >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    constexpr uint64_t next64() {
        const uint64_t hi = next();
        return (hi << 32) | next();
    }

    /// Равномерное целое в [0, bound). Отбраковка убирает смещение по модулю —
    /// она детерминирована, так как зависит только от потока чисел.
    constexpr uint32_t below(uint32_t bound) {
        if (bound == 0) return 0;
        const uint32_t threshold = (~bound + 1u) % bound;
        for (;;) {
            const uint32_t r = next();
            if (r >= threshold) return r % bound;
        }
    }

    /// Целое в [lo, hi] включительно.
    constexpr int32_t range(int32_t lo, int32_t hi) {
        if (hi <= lo) return lo;
        return lo + int32_t(below(uint32_t(hi - lo + 1)));
    }

    /// Дробь в [0, 1).
    fx unit() { return fx::fromRaw(int64_t(next())); }

    /// Бросок с вероятностью p (p из [0,1]).
    bool chance(fx p) { return unit() < p; }

    constexpr uint64_t state() const { return state_; }

private:
    constexpr void step() { state_ = state_ * 6364136223846793005ull + inc_; }

    uint64_t state_ = 0;
    uint64_t inc_ = 1;
};

}  // namespace pw
