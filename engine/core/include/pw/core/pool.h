// pw_core — пул объектов фиксированного размера.
//
// Для сущностей, которые появляются и исчезают поодиночке в течение сезона:
// флоты, конвои, приказы. Свободные блоки связаны в список внутри самой
// памяти блоков, поэтому накладных расходов на учёт нет вообще.
//
// Порядок выдачи блоков детерминирован (LIFO), но полагаться на конкретные
// адреса в симуляции нельзя: обходить сущности следует по плотному индексу,
// иначе раскладка памяти начнёт влиять на результат.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

namespace pw {

template <typename T>
class Pool {
public:
    Pool() = default;
    explicit Pool(size_t capacity) { reserve(capacity); }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    ~Pool() { std::free(storage_); }

    void reserve(size_t capacity) {
        std::free(storage_);
        storage_ = static_cast<Slot*>(std::malloc(sizeof(Slot) * capacity));
        capacity_ = storage_ ? capacity : 0;
        live_ = 0;
        free_ = nullptr;
        // Связываем свободные слоты в обратном порядке, чтобы первая выдача
        // пришлась на слот 0 — так дампы состояния читаются глазами.
        for (size_t i = capacity_; i-- > 0;) {
            storage_[i].next = free_;
            free_ = &storage_[i];
        }
    }

    template <typename... Args>
    T* create(Args&&... args) {
        if (!free_) return nullptr;
        Slot* slot = free_;
        free_ = slot->next;
        ++live_;
        return new (&slot->value) T(std::forward<Args>(args)...);
    }

    void destroy(T* obj) {
        if (!obj) return;
        obj->~T();
        auto* slot = reinterpret_cast<Slot*>(obj);
        slot->next = free_;
        free_ = slot;
        --live_;
    }

    size_t live() const { return live_; }
    size_t capacity() const { return capacity_; }

private:
    union Slot {
        Slot* next;
        alignas(T) unsigned char value[sizeof(T)];
        Slot() {}
        ~Slot() {}
    };

    Slot* storage_ = nullptr;
    Slot* free_ = nullptr;
    size_t capacity_ = 0;
    size_t live_ = 0;
};

}  // namespace pw
