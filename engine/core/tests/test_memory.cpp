#include "doctest.h"
#include "pw/core/arena.h"
#include "pw/core/pool.h"

#include <cstdint>

using namespace pw;

TEST_CASE("arena: выделяет последовательно и уважает выравнивание") {
    Arena arena(4096);
    auto* a = static_cast<uint8_t*>(arena.allocate(1, 1));
    auto* b = static_cast<uint64_t*>(arena.allocate(8, 8));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(b) % 8 == 0);
    CHECK(arena.used() >= 9);
}

TEST_CASE("arena: возвращает nullptr вместо порчи памяти") {
    Arena arena(64);
    CHECK(arena.allocate(32) != nullptr);
    CHECK(arena.allocate(1024) == nullptr);
    // После неудачи арена остаётся пригодной.
    CHECK(arena.allocate(16) != nullptr);
}

TEST_CASE("arena: откат по метке и полный сброс") {
    Arena arena(1024);
    arena.allocate(100);
    const size_t marker = arena.mark();
    arena.allocate(200);
    CHECK(arena.used() > marker);
    arena.release(marker);
    CHECK(arena.used() == marker);
    arena.reset();
    CHECK(arena.used() == 0);
}

TEST_CASE("arena: ArenaScope откатывает автоматически") {
    Arena arena(1024);
    arena.allocate(64);
    const size_t before = arena.used();
    {
        ArenaScope scope(arena);
        arena.allocate(256);
        CHECK(arena.used() > before);
    }
    CHECK(arena.used() == before);
}

TEST_CASE("arena: peak запоминает максимум, а не текущее") {
    Arena arena(1024);
    arena.allocate(512);
    arena.reset();
    arena.allocate(8);
    CHECK(arena.peak() >= 512);
}

TEST_CASE("arena: create вызывает конструктор") {
    struct Fleet {
        int ships;
        explicit Fleet(int n) : ships(n) {}
    };
    Arena arena(1024);
    auto* f = arena.create<Fleet>(42);
    REQUIRE(f != nullptr);
    CHECK(f->ships == 42);
}

TEST_CASE("pool: выдаёт, возвращает и переиспользует блоки") {
    struct Convoy {
        int cargo = 0;
    };
    Pool<Convoy> pool(3);
    CHECK(pool.capacity() == 3);

    auto* a = pool.create();
    auto* b = pool.create();
    auto* c = pool.create();
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(pool.live() == 3);

    // Пул исчерпан — возвращаем nullptr, а не растём молча.
    CHECK(pool.create() == nullptr);

    pool.destroy(b);
    CHECK(pool.live() == 2);
    auto* d = pool.create();
    CHECK(d == b);  // блок переиспользован
    CHECK(pool.live() == 3);
}

TEST_CASE("pool: destroy(nullptr) безопасен") {
    Pool<int> pool(2);
    pool.destroy(nullptr);
    CHECK(pool.live() == 0);
}
