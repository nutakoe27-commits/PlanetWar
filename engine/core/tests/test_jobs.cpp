#include "doctest.h"
#include "pw/core/jobs.h"

#include <atomic>
#include <numeric>
#include <vector>

using namespace pw;

TEST_CASE("jobs: parallelFor обходит каждый индекс ровно один раз") {
    JobSystem jobs;
    constexpr int64_t kCount = 10000;
    std::vector<int> visits(kCount, 0);

    jobs.parallelFor(kCount, 64, [&](int64_t begin, int64_t end) {
        for (int64_t i = begin; i < end; ++i) visits[size_t(i)] += 1;
    });

    for (int64_t i = 0; i < kCount; ++i) {
        REQUIRE(visits[size_t(i)] == 1);
    }
}

TEST_CASE("jobs: результат не зависит от числа потоков") {
    constexpr int64_t kCount = 5000;
    std::vector<int64_t> reference(kCount);

    auto fill = [&](JobSystem& js, std::vector<int64_t>& out) {
        out.assign(kCount, 0);
        js.parallelFor(kCount, 32, [&](int64_t begin, int64_t end) {
            // Каждый чанк пишет только в свой диапазон — правило детерминизма.
            for (int64_t i = begin; i < end; ++i) out[size_t(i)] = i * i;
        });
    };

    JobSystem sync(-1);
    fill(sync, reference);

    for (int workers : {1, 2, 4, 8}) {
        JobSystem js(workers);
        std::vector<int64_t> result;
        fill(js, result);
        CHECK(result == reference);
    }
}

TEST_CASE("jobs: синхронный режим не создаёт потоков") {
    JobSystem jobs(-1);
    CHECK(jobs.workerCount() == 0);

    int value = 0;
    jobs.dispatch([&] { value = 7; });
    CHECK(value == 7);  // выполнено прямо в вызывающем потоке
}

TEST_CASE("jobs: dispatch и waitIdle доводят все задачи до конца") {
    JobSystem jobs(4);
    std::atomic<int> done{0};
    for (int i = 0; i < 500; ++i) {
        jobs.dispatch([&done] { done.fetch_add(1, std::memory_order_relaxed); });
    }
    jobs.waitIdle();
    CHECK(done.load() == 500);
}

TEST_CASE("jobs: пустой и единичный диапазон") {
    JobSystem jobs(2);
    int calls = 0;
    jobs.parallelFor(0, 8, [&](int64_t, int64_t) { ++calls; });
    CHECK(calls == 0);

    jobs.parallelFor(1, 8, [&](int64_t begin, int64_t end) {
        ++calls;
        CHECK(begin == 0);
        CHECK(end == 1);
    });
    CHECK(calls == 1);
}

TEST_CASE("jobs: остаток диапазона не теряется при некратном grain") {
    JobSystem jobs(4);
    std::atomic<int64_t> sum{0};
    jobs.parallelFor(1003, 100, [&](int64_t begin, int64_t end) {
        int64_t local = 0;
        for (int64_t i = begin; i < end; ++i) local += i;
        sum.fetch_add(local, std::memory_order_relaxed);
    });
    CHECK(sum.load() == 1002 * 1003 / 2);
}
