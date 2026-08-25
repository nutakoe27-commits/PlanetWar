// pw_core — детерминированная арифметика с фиксированной точкой.
//
// ПРАВИЛО ПРОЕКТА: внутри pw_sim нет ни одного float и ни одного double.
// Симуляция обязана давать побитово одинаковый результат на x86 и ARM, иначе
// не работают ни клиентское предсказание, ни реплеи, ни валидация матчей, ни
// восстановление ноды из журнала команд. Плавающая точка этого не гарантирует:
// разные компиляторы вправе сворачивать выражения по-разному, а libm на разных
// платформах даёт разные последние биты.
//
// Два типа:
//   fx  — Q32.32 в int64. Позиции, накопления ресурсов, время. Диапазон
//         примерно +-2 миллиарда, шаг около 2.3e-10.
//   fxs — Q16.16 в int32. Множители, проценты, доли. Диапазон примерно
//         +-32768, шаг около 1.5e-5.
//
// Округление умножения — ВНИЗ, к минус бесконечности (арифметический сдвиг).
// Деление усекает К НУЛЮ. Эти правила выбраны за однозначность; менять их
// после начала сезона нельзя — изменится каждый расчёт в игре.
#pragma once

#include <cstdint>
#include <compare>

#include "pw/core/int128.h"

namespace pw {

namespace detail {

template <typename Store>
struct FixedOps;

template <>
struct FixedOps<int32_t> {
    static constexpr int32_t kMax = INT32_MAX;
    static constexpr int32_t kMin = INT32_MIN;

    static constexpr int32_t mul(int32_t a, int32_t b, int frac) {
        return int32_t((int64_t(a) * int64_t(b)) >> frac);
    }
    static constexpr int32_t div(int32_t a, int32_t b, int frac) {
        if (b == 0) return a >= 0 ? kMax : kMin;
        return int32_t((int64_t(a) << frac) / int64_t(b));
    }
    static constexpr int32_t sqrtRaw(int32_t v, int frac) {
        if (v <= 0) return 0;
        // Корень по биту на 64-битном промежуточном значении: точный, без float.
        uint64_t n = uint64_t(v) << frac;
        uint64_t rem = 0, root = 0;
        for (int i = 31; i >= 0; --i) {
            root <<= 1;
            rem = (rem << 2) | ((n >> (i * 2)) & 3u);
            const uint64_t trial = root | 1u;
            if (rem >= trial) {
                rem -= trial;
                root |= 2u;
            }
        }
        return int32_t(root >> 1);
    }
};

template <>
struct FixedOps<int64_t> {
    static constexpr int64_t kMax = INT64_MAX;
    static constexpr int64_t kMin = INT64_MIN;

    static int64_t mul(int64_t a, int64_t b, int frac) { return mulShift(a, b, frac); }
    static int64_t div(int64_t a, int64_t b, int frac) { return divShift(a, b, frac); }
    static int64_t sqrtRaw(int64_t v, int frac) { return sqrtShift(v, frac); }
};

}  // namespace detail

// ---------------------------------------------------------------------------

template <typename Store, int Frac>
class Fixed {
public:
    using Raw = Store;
    using Ops = detail::FixedOps<Store>;

    static constexpr int kFrac = Frac;
    static constexpr Store kOneRaw = Store(1) << Frac;

    constexpr Fixed() = default;

    static constexpr Fixed fromRaw(Store raw) {
        Fixed f;
        f.raw_ = raw;
        return f;
    }
    static constexpr Fixed fromInt(int64_t v) { return fromRaw(Store(v) << Frac); }

    /// Точная дробь num/den. Единственный правильный способ записать в коде
    /// значение вроде одной трети: fx::fromFraction(1, 3).
    static constexpr Fixed fromFraction(int64_t num, int64_t den) {
        return fromRaw(Ops::div(Store(num) << Frac, Store(den) << Frac, Frac));
    }

    /// Только для авторинга констант, тестов и импорта данных баланса.
    /// Внутри симуляции вызывать нельзя — на то есть fromFraction.
    static constexpr Fixed fromDouble(double v) {
        return fromRaw(Store(v * double(kOneRaw) + (v >= 0 ? 0.5 : -0.5)));
    }

    static constexpr Fixed zero() { return fromRaw(0); }
    static constexpr Fixed one() { return fromRaw(kOneRaw); }
    static constexpr Fixed epsilon() { return fromRaw(1); }
    static constexpr Fixed max() { return fromRaw(Ops::kMax); }
    static constexpr Fixed min() { return fromRaw(Ops::kMin); }

    constexpr Store raw() const { return raw_; }

    /// Усечение вниз, к минус бесконечности (а не к нулю, как у обычного int).
    constexpr int64_t floorToInt() const { return int64_t(raw_ >> Frac); }
    constexpr int64_t truncToInt() const {
        return raw_ >= 0 ? int64_t(raw_ >> Frac) : -int64_t((-raw_) >> Frac);
    }
    /// Только для рендера, логов и тестов. В симуляции — запрещено.
    double toDouble() const { return double(raw_) / double(kOneRaw); }

    constexpr Fixed operator-() const { return fromRaw(Store(-raw_)); }
    constexpr Fixed operator+(Fixed o) const { return fromRaw(Store(raw_ + o.raw_)); }
    constexpr Fixed operator-(Fixed o) const { return fromRaw(Store(raw_ - o.raw_)); }
    constexpr Fixed operator*(Fixed o) const { return fromRaw(Ops::mul(raw_, o.raw_, Frac)); }
    constexpr Fixed operator/(Fixed o) const { return fromRaw(Ops::div(raw_, o.raw_, Frac)); }

    constexpr Fixed operator*(int64_t k) const { return fromRaw(Store(raw_ * k)); }
    constexpr Fixed operator/(int64_t k) const { return k == 0 ? max() : fromRaw(Store(raw_ / k)); }

    constexpr Fixed& operator+=(Fixed o) { raw_ = Store(raw_ + o.raw_); return *this; }
    constexpr Fixed& operator-=(Fixed o) { raw_ = Store(raw_ - o.raw_); return *this; }
    constexpr Fixed& operator*=(Fixed o) { raw_ = Ops::mul(raw_, o.raw_, Frac); return *this; }
    constexpr Fixed& operator/=(Fixed o) { raw_ = Ops::div(raw_, o.raw_, Frac); return *this; }

    constexpr bool operator==(const Fixed&) const = default;
    constexpr auto operator<=>(const Fixed& o) const { return raw_ <=> o.raw_; }

private:
    Store raw_ = 0;
};

using fx  = Fixed<int64_t, 32>;  // Q32.32 — позиции, накопления, время
using fxs = Fixed<int32_t, 16>;  // Q16.16 — множители, проценты, доли

// ---------------------------------------------------------------------------
// Свободные функции
// ---------------------------------------------------------------------------

template <typename S, int F>
constexpr Fixed<S, F> abs(Fixed<S, F> v) {
    return v.raw() < 0 ? -v : v;
}

template <typename S, int F>
constexpr Fixed<S, F> min(Fixed<S, F> a, Fixed<S, F> b) {
    return a.raw() < b.raw() ? a : b;
}

template <typename S, int F>
constexpr Fixed<S, F> max(Fixed<S, F> a, Fixed<S, F> b) {
    return a.raw() > b.raw() ? a : b;
}

template <typename S, int F>
constexpr Fixed<S, F> clamp(Fixed<S, F> v, Fixed<S, F> lo, Fixed<S, F> hi) {
    return v.raw() < lo.raw() ? lo : (v.raw() > hi.raw() ? hi : v);
}

/// Линейная интерполяция. t вне [0,1] не обрезается — вызывающий решает сам.
template <typename S, int F>
constexpr Fixed<S, F> lerp(Fixed<S, F> a, Fixed<S, F> b, Fixed<S, F> t) {
    return a + (b - a) * t;
}

template <typename S, int F>
inline Fixed<S, F> sqrt(Fixed<S, F> v) {
    return Fixed<S, F>::fromRaw(Fixed<S, F>::Ops::sqrtRaw(v.raw(), F));
}

/// Перевод между разрядностями. Q32.32 -> Q16.16 теряет точность осознанно.
inline constexpr fxs narrow(fx v) { return fxs::fromRaw(int32_t(v.raw() >> 16)); }
inline constexpr fx widen(fxs v) { return fx::fromRaw(int64_t(v.raw()) << 16); }

}  // namespace pw
