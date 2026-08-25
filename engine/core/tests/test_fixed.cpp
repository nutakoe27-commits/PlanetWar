#include "doctest.h"
#include "pw/core/fixed.h"

#include <initializer_list>

using namespace pw;

TEST_CASE("fixed: базовая арифметика точна") {
    const fx a = fx::fromInt(7);
    const fx b = fx::fromInt(3);

    CHECK((a + b).floorToInt() == 10);
    CHECK((a - b).floorToInt() == 4);
    CHECK((a * b).floorToInt() == 21);
    CHECK((a / b).floorToInt() == 2);
}

TEST_CASE("fixed: fromFraction даёт точную дробь") {
    const fx third = fx::fromFraction(1, 3);
    // 1/3 в Q32.32 — это floor(2^32 / 3).
    CHECK(third.raw() == int64_t(0x100000000ll / 3));

    // Три трети не дают ровно единицу, и это ожидаемо: дробь непредставима.
    // Важно, что ошибка ограничена одним младшим разрядом на слагаемое.
    const fx sum = third + third + third;
    CHECK((fx::one() - sum).raw() <= 3);
}

TEST_CASE("fixed: умножение округляет вниз, деление усекает к нулю") {
    // -1 * 0.5 при округлении вниз даёт ровно -0.5, без сюрпризов на границе.
    const fx half = fx::fromFraction(1, 2);
    CHECK((fx::fromInt(-1) * half).raw() == -(int64_t(1) << 31));

    // Умножение двух наименьших положительных величин обнуляется вниз.
    CHECK((fx::epsilon() * fx::epsilon()).raw() == 0);
    // А для отрицательного результата округление вниз даёт -1, а не 0.
    CHECK((fx::epsilon() * -fx::epsilon()).raw() == -1);

    CHECK(fx::fromInt(-7).truncToInt() == -7);
    CHECK((fx::fromInt(-7) / fx::fromInt(2)).truncToInt() == -3);
    CHECK((fx::fromInt(-7) / fx::fromInt(2)).floorToInt() == -4);
}

TEST_CASE("fixed: корень точен на полных квадратах") {
    // Верхняя граница неслучайна: целая часть Q32.32 ограничена 2^31, поэтому
    // наибольший представимый полный квадрат — это 46340^2.
    for (int64_t n : {0, 1, 4, 9, 16, 144, 1000, 46340}) {
        const fx root = sqrt(fx::fromInt(n * n));
        CHECK(root.floorToInt() == n);
    }
}

TEST_CASE("fixed: границы диапазона Q32.32 задокументированы") {
    // Целая часть помещается в 32 бита со знаком. Для координат галактики и
    // накоплений за сезон запаса хватает с большим избытком, но знать предел
    // нужно: молчаливое переполнение здесь означает разъехавшуюся симуляцию.
    constexpr int64_t kLimit = int64_t(1) << 31;

    CHECK(fx::fromInt(kLimit - 1).floorToInt() == kLimit - 1);
    CHECK(fx::fromInt(-(kLimit - 1)).floorToInt() == -(kLimit - 1));

    // Величины сверх предела заворачиваются — это поведение int64, а не ошибка
    // в fixed-point. Значения такого порядка обязаны храниться целыми числами.
    CHECK(fx::fromInt(kLimit * 2).floorToInt() != kLimit * 2);

    // Q16.16 предсказуемо теснее: целая часть до 2^15.
    CHECK(fxs::fromInt(32767).floorToInt() == 32767);
}

TEST_CASE("fixed: корень сходится с квадратом на дробных значениях") {
    const fx two = fx::fromInt(2);
    const fx root = sqrt(two);
    const fx back = root * root;
    // Погрешность не превышает пары младших разрядов Q32.32.
    CHECK(abs(back - two).raw() < 16);
}

TEST_CASE("fixed: деление на ноль насыщается, а не падает") {
    CHECK(fx::fromInt(5) / fx::zero() == fx::max());
    CHECK(fx::fromInt(-5) / fx::zero() == fx::min());
}

TEST_CASE("fixed: Q16.16 работает независимо от Q32.32") {
    const fxs a = fxs::fromInt(150);
    const fxs pct = fxs::fromFraction(85, 100);
    const fxs result = a * pct;
    // 150 * 0.85 = 127.5, вниз до 127.
    CHECK(result.floorToInt() == 127);
}

TEST_CASE("fixed: narrow и widen согласованы") {
    const fx v = fx::fromFraction(3, 8);
    CHECK(widen(narrow(v)) == v);
}

TEST_CASE("fixed: clamp и lerp") {
    const fx lo = fx::fromInt(10), hi = fx::fromInt(20);
    CHECK(clamp(fx::fromInt(5), lo, hi) == lo);
    CHECK(clamp(fx::fromInt(25), lo, hi) == hi);
    CHECK(clamp(fx::fromInt(15), lo, hi) == fx::fromInt(15));
    CHECK(lerp(lo, hi, fx::fromFraction(1, 2)) == fx::fromInt(15));
}
