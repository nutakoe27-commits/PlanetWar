#include "pw/sim/schedule.h"

namespace pw::sim {

void Schedule::add(const char* name, SystemFn function) {
    if (function == nullptr) return;
    systems_.push_back(Entry{name, function});
}

void Schedule::run(World& world, const TickContext& context) const {
    // Строго в порядке добавления. Никакой сортировки, никакого обхода
    // по хеш-таблице: порядок систем — часть правил игры.
    for (const Entry& entry : systems_) {
        entry.function(world, context);
    }
}

void Simulation::step() {
    TickContext context;
    context.tick = tick_;
    schedule_.run(world_, context);
    ++tick_;
}

void Simulation::advance(uint64_t ticks) {
    for (uint64_t i = 0; i < ticks; ++i) step();
}

uint64_t Simulation::hash() const {
    return Hasher().u64(tick_).u64(world_.hash()).value();
}

}  // namespace pw::sim
