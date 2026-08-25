#include "doctest.h"
#include "pw/core/hash.h"
#include "pw/core/rng.h"

#include <bit>
#include <set>

using namespace pw;

TEST_CASE("rng: одинаковый сид даёт одинаковую последовательность") {
    Rng a(12345, 1), b(12345, 1);
    for (int i = 0; i < 1000; ++i) CHECK(a.next() == b.next());
}

TEST_CASE("rng: разные потоки независимы при одном сиде") {
    Rng combat(12345, 1), anomalies(12345, 2);
    int same = 0;
    for (int i = 0; i < 1000; ++i) {
        if (combat.next() == anomalies.next()) ++same;
    }
    // Совпадения возможны случайно, но их должно быть исчезающе мало.
    CHECK(same < 5);
}

TEST_CASE("rng: below не выходит за границу и покрывает диапазон") {
    Rng r(777, 3);
    std::set<uint32_t> seen;
    for (int i = 0; i < 4000; ++i) {
        const uint32_t v = r.below(6);
        CHECK(v < 6);
        seen.insert(v);
    }
    CHECK(seen.size() == 6);
}

TEST_CASE("rng: range включает обе границы") {
    Rng r(999, 4);
    bool sawLo = false, sawHi = false;
    for (int i = 0; i < 2000; ++i) {
        const int32_t v = r.range(-3, 3);
        CHECK(v >= -3);
        CHECK(v <= 3);
        if (v == -3) sawLo = true;
        if (v == 3) sawHi = true;
    }
    CHECK(sawLo);
    CHECK(sawHi);
}

TEST_CASE("rng: unit лежит в [0,1)") {
    Rng r(555, 5);
    for (int i = 0; i < 2000; ++i) {
        const fx v = r.unit();
        CHECK(v >= fx::zero());
        CHECK(v < fx::one());
    }
}

TEST_CASE("hash: значение стабильно и не зависит от платформы") {
    // Эталон FNV-1a для строки "PlanetWar". Если этот тест упал — либо
    // сменился алгоритм (значит, сменилась вся процедурная галактика),
    // либо платформа считает иначе. Оба случая требуют разбирательства.
    Hasher h;
    for (char c : {'P', 'l', 'a', 'n', 'e', 't', 'W', 'a', 'r'}) {
        h.byte(uint8_t(c));
    }
    CHECK(h.value() == 0x68CD8AE4101E45DFull);
}

TEST_CASE("hash: mixCoord даёт лавину на соседних координатах") {
    const uint64_t seed = 0xDEADBEEF;
    const uint64_t a = mixCoord(seed, 100, 200);
    const uint64_t b = mixCoord(seed, 100, 201);

    // Соседние системы галактики обязаны быть некоррелированы, иначе
    // процедурная генерация даст видимые полосы и повторы.
    // std::popcount, а не __builtin_popcountll: последнего нет в MSVC,
    // и CI это поймал ровно на первом же прогоне матрицы платформ.
    const int differing = std::popcount(a ^ b);
    CHECK(differing > 20);
    CHECK(differing < 44);

    // И воспроизводимость: те же координаты — тот же результат всегда.
    CHECK(mixCoord(seed, 100, 200) == a);
}

TEST_CASE("hash: разные сиды сезона дают разные галактики") {
    CHECK(mixCoord(1, 5, 5) != mixCoord(2, 5, 5));
}
