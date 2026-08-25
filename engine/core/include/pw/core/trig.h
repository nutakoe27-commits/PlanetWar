// pw_core — детерминированная тригонометрия.
//
// Угол измеряется в ОБОРОТАХ: 1.0 = полный круг, 0.25 = четверть.
// Так из кода уходит число pi, а приведение угла к диапазону становится
// маскированием битов вместо деления с остатком на иррациональное число.
//
// Значения берутся из таблиц, сгенерированных tools/gen_trig_tables.py и
// закоммиченных в репозиторий. std::sin здесь использовать нельзя: разные
// платформы и версии libm дают разные последние биты, а нам нужен побитово
// одинаковый мир на x86 и ARM.
#pragma once

#include <cstdint>

#include "pw/core/fixed.h"

namespace pw {

namespace detail {
#include "pw/core/trig_tables.inc"

/// Синус на четверти волны. q — 30-битная позиция внутри четверти оборота,
/// допустимо ровно значение 2^30 (граница). Возврат — Q32.
inline int64_t quarterSin(uint32_t q) {
    const uint32_t idx = q >> 20;
    if (idx >= uint32_t(kTrigQuarter)) return kSinQuarter[kTrigQuarter];
    const int64_t a = kSinQuarter[idx];
    const int64_t b = kSinQuarter[idx + 1];
    const int64_t f = int64_t(q & 0xFFFFFu);
    return a + (((b - a) * f) >> 20);
}

/// atan(x)/2pi для x в [0,1]. r — 30-битная дробь, представляющая x.
inline int64_t unitAtan(uint32_t r) {
    const uint32_t idx = r >> 20;
    if (idx >= uint32_t(kTrigQuarter)) return kAtanUnit[kTrigQuarter];
    const int64_t a = kAtanUnit[idx];
    const int64_t b = kAtanUnit[idx + 1];
    const int64_t f = int64_t(r & 0xFFFFFu);
    return a + (((b - a) * f) >> 20);
}
}  // namespace detail

/// sin(2*pi*turns). Угол приводится к кругу автоматически, любой величины.
inline fx sinTurns(fx turns) {
    // Младшие 32 бита Q32.32 — это и есть дробная часть оборота.
    const uint32_t t = uint32_t(uint64_t(turns.raw()) & 0xFFFFFFFFull);
    const uint32_t quad = t >> 30;
    const uint32_t q = t & 0x3FFFFFFFu;
    switch (quad) {
        case 0:  return fx::fromRaw(detail::quarterSin(q));
        case 1:  return fx::fromRaw(detail::quarterSin(0x40000000u - q));
        case 2:  return fx::fromRaw(-detail::quarterSin(q));
        default: return fx::fromRaw(-detail::quarterSin(0x40000000u - q));
    }
}

/// cos(2*pi*turns) — тот же синус, сдвинутый на четверть оборота.
inline fx cosTurns(fx turns) {
    return sinTurns(turns + fx::fromRaw(int64_t(1) << 30));
}

/// Угол вектора (x, y) в оборотах, диапазон [0, 1). Для (0,0) возвращает 0.
inline fx atan2Turns(fx y, fx x) {
    const int64_t rx = x.raw(), ry = y.raw();
    if (rx == 0 && ry == 0) return fx::zero();

    const uint64_t ax = uint64_t(rx < 0 ? -rx : rx);
    const uint64_t ay = uint64_t(ry < 0 ? -ry : ry);

    int64_t turns;
    if (ay <= ax) {
        // Пологий сектор: считаем atan(ay/ax) напрямую.
        const uint32_t ratio = uint32_t(detail::divShift(int64_t(ay), int64_t(ax), 30));
        turns = detail::unitAtan(ratio);
    } else {
        // Крутой сектор: зеркалим относительно диагонали, 0.125 оборота.
        const uint32_t ratio = uint32_t(detail::divShift(int64_t(ax), int64_t(ay), 30));
        turns = (int64_t(1) << 30) - detail::unitAtan(ratio);
    }

    // Разворот по квадрантам. Полный оборот — 2^32 в Q32.32.
    constexpr int64_t kTurn = int64_t(1) << 32;
    constexpr int64_t kHalf = int64_t(1) << 31;
    if (rx < 0) turns = kHalf - turns;
    if (ry < 0) turns = -turns;
    if (turns < 0) turns += kTurn;
    if (turns >= kTurn) turns -= kTurn;
    return fx::fromRaw(turns);
}

/// Длина вектора. Промежуточные квадраты считаются точно, без переполнения
/// на разумных игровых координатах.
inline fx length(fx x, fx y) { return sqrt(x * x + y * y); }

}  // namespace pw
