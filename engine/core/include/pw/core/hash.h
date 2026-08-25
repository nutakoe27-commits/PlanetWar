// pw_core — хеширование.
//
// Две задачи, обе критичные для проекта:
//
// 1. Хеш состояния мира. CI прогоняет одну и ту же последовательность команд
//    на x86 и ARM и сверяет хеши каждую 1000 тиков. Расхождение — красный
//    билд, а не предупреждение.
// 2. Процедурная генерация. Содержимое системы выводится из
//    hash(season_seed, coord) — галактика не хранится, она вычисляется.
//    Поэтому функция обязана быть стабильной НАВСЕГДА: смена алгоритма
//    меняет всю вселенную.
#pragma once

#include <cstdint>
#include <cstddef>

namespace pw {

/// FNV-1a, 64 бита. Константы зафиксированы стандартом де-факто и не меняются.
class Hasher {
public:
    static constexpr uint64_t kOffset = 14695981039346656037ull;
    static constexpr uint64_t kPrime = 1099511628211ull;

    constexpr Hasher() = default;

    constexpr Hasher& byte(uint8_t b) {
        h_ ^= uint64_t(b);
        h_ *= kPrime;
        return *this;
    }

    constexpr Hasher& u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) byte(uint8_t((v >> (i * 8)) & 0xFFu));
        return *this;
    }

    constexpr Hasher& i64(int64_t v) { return u64(uint64_t(v)); }
    constexpr Hasher& u32(uint32_t v) { return u64(v); }
    constexpr Hasher& i32(int32_t v) { return u64(uint64_t(int64_t(v))); }

    Hasher& bytes(const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) byte(p[i]);
        return *this;
    }

    constexpr uint64_t value() const { return h_; }

private:
    uint64_t h_ = kOffset;
};

/// Детерминированное перемешивание координат в сид. Основа процедурной
/// галактики: система по координатам всегда одна и та же, но соседние
/// координаты дают некоррелированные результаты.
constexpr uint64_t mixCoord(uint64_t seed, int64_t x, int64_t y) {
    uint64_t h = Hasher().u64(seed).i64(x).i64(y).value();
    // Финальное лавинообразное перемешивание (splitmix64).
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h;
}

}  // namespace pw
