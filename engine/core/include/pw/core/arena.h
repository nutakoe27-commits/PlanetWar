// pw_core — арена-аллокатор (bump allocator).
//
// Симуляция за тик создаёт много короткоживущих данных: списки видимых
// объектов, промежуточные результаты боя, очереди приказов. Гонять их через
// malloc/free — это и фрагментация, и непредсказуемая задержка, и разный
// порядок адресов на разных запусках. Арена решает всё сразу: выделение —
// это сложение указателя, освобождение — сброс всей арены за одну операцию.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

namespace pw {

class Arena {
public:
    Arena() = default;
    explicit Arena(size_t capacity) { reserve(capacity); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& o) noexcept
        : base_(o.base_), size_(o.size_), used_(o.used_), peak_(o.peak_) {
        o.base_ = nullptr;
        o.size_ = o.used_ = o.peak_ = 0;
    }

    ~Arena() { std::free(base_); }

    void reserve(size_t capacity) {
        std::free(base_);
        base_ = static_cast<uint8_t*>(std::malloc(capacity));
        size_ = base_ ? capacity : 0;
        used_ = 0;
        peak_ = 0;
    }

    /// Возвращает nullptr при нехватке места — вызывающий обязан проверить.
    /// Молчаливое падение здесь дороже, чем явная проверка.
    void* allocate(size_t bytes, size_t align = alignof(std::max_align_t)) {
        const size_t start = (used_ + align - 1) & ~(align - 1);
        if (start + bytes > size_) return nullptr;
        used_ = start + bytes;
        if (used_ > peak_) peak_ = used_;
        return base_ + start;
    }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return mem ? new (mem) T(std::forward<Args>(args)...) : nullptr;
    }

    /// Массив БЕЗ вызова конструкторов — для POD-данных симуляции.
    template <typename T>
    T* allocateArray(size_t count) {
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }

    /// Метка для отката. Деструкторы не вызываются: арена рассчитана на
    /// тривиально разрушаемые данные, и это её осознанное ограничение.
    size_t mark() const { return used_; }
    void release(size_t marker) { if (marker <= used_) used_ = marker; }
    void reset() { used_ = 0; }

    size_t used() const { return used_; }
    size_t capacity() const { return size_; }
    size_t peak() const { return peak_; }

private:
    uint8_t* base_ = nullptr;
    size_t size_ = 0;
    size_t used_ = 0;
    size_t peak_ = 0;
};

/// RAII-откат арены до состояния на момент создания.
class ArenaScope {
public:
    explicit ArenaScope(Arena& a) : arena_(a), marker_(a.mark()) {}
    ~ArenaScope() { arena_.release(marker_); }
    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;

private:
    Arena& arena_;
    size_t marker_;
};

}  // namespace pw
