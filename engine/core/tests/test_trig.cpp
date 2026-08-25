#include "doctest.h"
#include "pw/core/trig.h"

using namespace pw;

namespace {
// Допуск: таблица на 1024 отсчёта четверти волны с линейной интерполяцией
// даёт ошибку порядка 1e-6. Берём запас.
constexpr int64_t kTol = 1 << 13;  // ~3e-6 в Q32.32

bool near(fx a, fx b, int64_t tol = kTol) { return abs(a - b).raw() <= tol; }
}  // namespace

TEST_CASE("trig: опорные точки синуса") {
    CHECK(sinTurns(fx::zero()) == fx::zero());
    CHECK(near(sinTurns(fx::fromFraction(1, 4)), fx::one()));
    CHECK(near(sinTurns(fx::fromFraction(1, 2)), fx::zero()));
    CHECK(near(sinTurns(fx::fromFraction(3, 4)), -fx::one()));
}

TEST_CASE("trig: опорные точки косинуса") {
    CHECK(near(cosTurns(fx::zero()), fx::one()));
    CHECK(near(cosTurns(fx::fromFraction(1, 4)), fx::zero()));
    CHECK(near(cosTurns(fx::fromFraction(1, 2)), -fx::one()));
}

TEST_CASE("trig: основное тождество на всём круге") {
    for (int i = 0; i < 512; ++i) {
        const fx t = fx::fromFraction(i, 512);
        const fx s = sinTurns(t), c = cosTurns(t);
        CHECK(near(s * s + c * c, fx::one(), 1 << 15));
    }
}

TEST_CASE("trig: угол приводится к кругу автоматически") {
    const fx base = fx::fromFraction(1, 8);
    CHECK(sinTurns(base) == sinTurns(base + fx::fromInt(1)));
    CHECK(sinTurns(base) == sinTurns(base + fx::fromInt(37)));
    CHECK(sinTurns(base) == sinTurns(base - fx::fromInt(12)));
}

TEST_CASE("trig: atan2 по квадрантам") {
    const fx one = fx::one(), zero = fx::zero();
    CHECK(near(atan2Turns(zero, one), fx::zero()));
    CHECK(near(atan2Turns(one, zero), fx::fromFraction(1, 4)));
    CHECK(near(atan2Turns(zero, -one), fx::fromFraction(1, 2)));
    CHECK(near(atan2Turns(-one, zero), fx::fromFraction(3, 4)));
    CHECK(near(atan2Turns(one, one), fx::fromFraction(1, 8)));
    CHECK(near(atan2Turns(-one, -one), fx::fromFraction(5, 8)));
}

TEST_CASE("trig: atan2 обращает sin и cos") {
    for (int i = 0; i < 256; ++i) {
        const fx t = fx::fromFraction(i, 256);
        const fx back = atan2Turns(sinTurns(t), cosTurns(t));
        // Около нуля результат может оказаться чуть меньше полного оборота.
        const fx diff = abs(back - t);
        const bool ok = diff.raw() <= (1 << 16) ||
                        abs(diff - fx::one()).raw() <= (1 << 16);
        CHECK(ok);
    }
}

TEST_CASE("trig: длина вектора") {
    CHECK(near(length(fx::fromInt(3), fx::fromInt(4)), fx::fromInt(5), 64));
}
