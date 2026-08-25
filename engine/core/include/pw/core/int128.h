// pw_core — точная 128-битная арифметика для fixed-point.
//
// Все операции здесь ТОЧНЫЕ: промежуточный результат считается в 128 битах,
// без потери разрядов и без чисел с плавающей точкой. Быстрый путь использует
// встроенный __int128 (GCC/Clang на всех наших платформах), запасной —
// переносимую реализацию на паре uint64. Оба пути дают побитово одинаковый
// результат, поэтому детерминизм не зависит от компилятора.
#pragma once

#include <cstdint>

namespace pw::detail {

#if defined(__SIZEOF_INT128__)
#  define PW_HAS_INT128 1
// __int128 — расширение компилятора, а не часть стандарта. Оно есть у GCC и
// Clang на всех пяти наших платформах; -Wpedantic об этом честно предупреждает,
// поэтому глушим предупреждение точечно, а не отключаем педантичность целиком.
#  if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpedantic"
#  endif
__extension__ typedef __int128 I128;
__extension__ typedef unsigned __int128 U128;
#else
#  define PW_HAS_INT128 0
#endif

#if !PW_HAS_INT128
// ---------------------------------------------------------------------------
// Переносимое беззнаковое 128-битное число. Используется только там, где нет
// встроенного __int128 (например, MSVC без clang-cl).
// ---------------------------------------------------------------------------
struct U128Soft {
    uint64_t hi = 0;
    uint64_t lo = 0;
};

inline U128Soft mulU64(uint64_t a, uint64_t b) {
    const uint64_t aLo = a & 0xFFFFFFFFull, aHi = a >> 32;
    const uint64_t bLo = b & 0xFFFFFFFFull, bHi = b >> 32;

    const uint64_t p0 = aLo * bLo;
    const uint64_t p1 = aLo * bHi;
    const uint64_t p2 = aHi * bLo;
    const uint64_t p3 = aHi * bHi;

    const uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFull) + (p2 & 0xFFFFFFFFull);

    U128Soft r;
    r.lo = (p0 & 0xFFFFFFFFull) | (mid << 32);
    r.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    return r;
}

inline U128Soft shlU128(U128Soft v, int s) {
    if (s == 0) return v;
    if (s >= 128) return {0, 0};
    if (s >= 64) return {v.lo << (s - 64), 0};
    return {(v.hi << s) | (v.lo >> (64 - s)), v.lo << s};
}

inline U128Soft shrU128(U128Soft v, int s) {
    if (s == 0) return v;
    if (s >= 128) return {0, 0};
    if (s >= 64) return {0, v.hi >> (s - 64)};
    return {v.hi >> s, (v.lo >> s) | (v.hi << (64 - s))};
}

inline bool geU128(U128Soft a, U128Soft b) {
    return a.hi != b.hi ? a.hi > b.hi : a.lo >= b.lo;
}

inline U128Soft subU128(U128Soft a, U128Soft b) {
    U128Soft r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo ? 1u : 0u);
    return r;
}

// Деление 128/64 столбиком. Частное обязано помещаться в 64 бита —
// вызывающий код это гарантирует диапазонами fixed-point.
inline uint64_t divU128(U128Soft n, uint64_t d) {
    if (d == 0) return ~uint64_t(0);
    if (n.hi == 0) return n.lo / d;

    U128Soft rem{0, 0}, div{0, d};
    uint64_t quo = 0;
    for (int i = 127; i >= 0; --i) {
        rem = shlU128(rem, 1);
        const uint64_t bit = (i >= 64) ? ((n.hi >> (i - 64)) & 1u) : ((n.lo >> i) & 1u);
        rem.lo |= bit;
        if (geU128(rem, div)) {
            rem = subU128(rem, div);
            if (i < 64) quo |= (uint64_t(1) << i);
        }
    }
    return quo;
}

// Целочисленный квадратный корень из 128-битного числа, метод «по биту».
inline uint64_t sqrtU128(U128Soft v) {
    U128Soft rem{0, 0}, root{0, 0};
    for (int i = 63; i >= 0; --i) {
        root = shlU128(root, 1);
        rem = shlU128(rem, 2);
        const int sh = i * 2;
        uint64_t two;
        if (sh >= 64)      two = (v.hi >> (sh - 64)) & 3u;
        else if (sh == 62) two = ((v.lo >> 62) & 3u);
        else               two = (v.lo >> sh) & 3u;
        rem.lo |= two;

        U128Soft trial = root;
        trial.lo |= 1u;
        if (geU128(rem, trial)) {
            rem = subU128(rem, trial);
            root.lo |= 2u;
        }
    }
    return root.lo >> 1;
}
#endif  // !PW_HAS_INT128

// ---------------------------------------------------------------------------
// Публичные точные операции.
// ---------------------------------------------------------------------------

/// (a * b) >> shift, с точным 128-битным промежуточным результатом.
/// Сдвиг арифметический: округление всегда вниз, к минус бесконечности.
/// Это выбранное нами правило округления — оно однозначно и одинаково везде.
inline int64_t mulShift(int64_t a, int64_t b, int shift) {
#if PW_HAS_INT128
    return int64_t((I128(a) * I128(b)) >> shift);
#else
    const bool neg = (a < 0) != (b < 0);
    const uint64_t ua = a < 0 ? uint64_t(-(a + 1)) + 1u : uint64_t(a);
    const uint64_t ub = b < 0 ? uint64_t(-(b + 1)) + 1u : uint64_t(b);
    U128Soft p = mulU64(ua, ub);
    if (!neg) return int64_t(shrU128(p, shift).lo);
    // Отрицательный результат: инвертируем ДО сдвига, чтобы получить floor,
    // как даёт арифметический сдвиг вправо у знаковых чисел.
    U128Soft np = subU128(U128Soft{0, 0}, p);
    // Арифметический сдвиг: заполняем единицами старшие разряды.
    U128Soft sh = shrU128(np, shift);
    if (shift > 0 && shift < 128) {
        const U128Soft mask = shlU128(U128Soft{~uint64_t(0), ~uint64_t(0)}, 128 - shift);
        sh.hi |= mask.hi;
        sh.lo |= mask.lo;
    }
    return int64_t(sh.lo);
#endif
}

/// (a << shift) / b, с точным 128-битным числителем. Деление усекает к нулю.
inline int64_t divShift(int64_t a, int64_t b, int shift) {
    if (b == 0) return a >= 0 ? INT64_MAX : INT64_MIN;
#if PW_HAS_INT128
    return int64_t((I128(a) << shift) / I128(b));
#else
    const bool neg = (a < 0) != (b < 0);
    const uint64_t ua = a < 0 ? uint64_t(-(a + 1)) + 1u : uint64_t(a);
    const uint64_t ub = b < 0 ? uint64_t(-(b + 1)) + 1u : uint64_t(b);
    const uint64_t q = divU128(shlU128(U128Soft{0, ua}, shift), ub);
    return neg ? -int64_t(q) : int64_t(q);
#endif
}

/// Целочисленный корень из (v << shift). v должно быть неотрицательным.
inline int64_t sqrtShift(int64_t v, int shift) {
    if (v <= 0) return 0;
#if PW_HAS_INT128
    U128 n = U128(uint64_t(v)) << shift;
    // Корень «по биту»: точный, без плавающей точки, одинаковый на всех платформах.
    U128 rem = 0, root = 0;
    for (int i = 63; i >= 0; --i) {
        root <<= 1;
        rem = (rem << 2) | ((n >> (i * 2)) & 3u);
        const U128 trial = root | 1u;
        if (rem >= trial) {
            rem -= trial;
            root |= 2u;
        }
    }
    return int64_t(uint64_t(root >> 1));
#else
    return int64_t(sqrtU128(shlU128(U128Soft{0, uint64_t(v)}, shift)));
#endif
}

}  // namespace pw::detail

#if PW_HAS_INT128 && (defined(__GNUC__) || defined(__clang__))
#  pragma GCC diagnostic pop
#endif
