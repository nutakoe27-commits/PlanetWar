#include "pw/core/jobs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace pw {

namespace {
// Очередь на поток. Владелец берёт с хвоста (свежие задачи горячие в кэше),
// воры — с головы (меньше конфликтов за одну и ту же строку кэша).
struct Queue {
    std::mutex mutex;
    std::deque<std::function<void()>> jobs;
};
}  // namespace

struct JobSystem::Impl {
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<Queue>> queues;  // на воркера + одна для главного
    std::atomic<int64_t> pending{0};
    std::atomic<bool> running{true};

    std::mutex sleepMutex;
    std::condition_variable wake;

    int workers = 0;
    size_t mainQueue = 0;

    bool popFrom(size_t index, std::function<void()>& out) {
        Queue& q = *queues[index];
        std::lock_guard<std::mutex> lock(q.mutex);
        if (q.jobs.empty()) return false;
        out = std::move(q.jobs.back());
        q.jobs.pop_back();
        return true;
    }

    bool stealFrom(size_t index, std::function<void()>& out) {
        Queue& q = *queues[index];
        std::lock_guard<std::mutex> lock(q.mutex);
        if (q.jobs.empty()) return false;
        out = std::move(q.jobs.front());
        q.jobs.pop_front();
        return true;
    }

    /// Взять свою задачу, иначе украсть у соседей по кругу.
    bool acquire(size_t self, std::function<void()>& out) {
        if (popFrom(self, out)) return true;
        const size_t n = queues.size();
        for (size_t i = 1; i < n; ++i) {
            if (stealFrom((self + i) % n, out)) return true;
        }
        return false;
    }

    void push(size_t index, std::function<void()> job) {
        {
            Queue& q = *queues[index];
            std::lock_guard<std::mutex> lock(q.mutex);
            q.jobs.push_back(std::move(job));
        }
        pending.fetch_add(1, std::memory_order_release);
        wake.notify_one();
    }

    /// Выполнить одну задачу, если она нашлась. true — работа была.
    bool runOne(size_t self) {
        std::function<void()> job;
        if (!acquire(self, job)) return false;
        job();
        pending.fetch_sub(1, std::memory_order_release);
        return true;
    }

    void workerLoop(size_t self) {
        while (running.load(std::memory_order_acquire)) {
            if (runOne(self)) continue;
            std::unique_lock<std::mutex> lock(sleepMutex);
            wake.wait_for(lock, std::chrono::milliseconds(2), [this] {
                return !running.load(std::memory_order_acquire) ||
                       pending.load(std::memory_order_acquire) > 0;
            });
        }
    }
};

JobSystem::JobSystem(int workers) : impl_(std::make_unique<Impl>()) {
    if (workers < 0) {
        impl_->workers = 0;  // синхронный режим
    } else if (workers == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        impl_->workers = hw > 1 ? int(hw) - 1 : 0;
    } else {
        impl_->workers = workers;
    }

    const size_t total = size_t(impl_->workers) + 1;
    impl_->queues.reserve(total);
    for (size_t i = 0; i < total; ++i) impl_->queues.push_back(std::make_unique<Queue>());
    impl_->mainQueue = size_t(impl_->workers);

    impl_->threads.reserve(size_t(impl_->workers));
    for (int i = 0; i < impl_->workers; ++i) {
        impl_->threads.emplace_back([this, i] { impl_->workerLoop(size_t(i)); });
    }
}

JobSystem::~JobSystem() {
    waitIdle();
    impl_->running.store(false, std::memory_order_release);
    impl_->wake.notify_all();
    for (auto& t : impl_->threads) {
        if (t.joinable()) t.join();
    }
}

int JobSystem::workerCount() const { return impl_->workers; }

void JobSystem::dispatch(std::function<void()> job) {
    if (impl_->workers == 0) {
        job();
        return;
    }
    impl_->push(impl_->mainQueue, std::move(job));
}

void JobSystem::waitIdle() {
    while (impl_->pending.load(std::memory_order_acquire) > 0) {
        if (!impl_->runOne(impl_->mainQueue)) std::this_thread::yield();
    }
}

void JobSystem::parallelFor(int64_t count, int64_t grain,
                            const std::function<void(int64_t, int64_t)>& body) {
    if (count <= 0) return;
    if (grain < 1) grain = 1;

    const int64_t chunks = (count + grain - 1) / grain;
    if (impl_->workers == 0 || chunks == 1) {
        body(0, count);
        return;
    }

    std::atomic<int64_t> remaining{chunks};
    for (int64_t c = 0; c < chunks; ++c) {
        const int64_t begin = c * grain;
        const int64_t end = begin + grain < count ? begin + grain : count;
        impl_->push(impl_->mainQueue, [&body, &remaining, begin, end] {
            body(begin, end);
            remaining.fetch_sub(1, std::memory_order_release);
        });
    }

    // Главный поток не простаивает: он такой же исполнитель, как воркеры.
    while (remaining.load(std::memory_order_acquire) > 0) {
        if (!impl_->runOne(impl_->mainQueue)) std::this_thread::yield();
    }
}

}  // namespace pw
