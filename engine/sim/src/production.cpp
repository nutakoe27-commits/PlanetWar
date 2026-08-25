#include "pw/sim/production.h"

#include <algorithm>
#include <vector>

#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/galaxy.h"

namespace pw::sim {

void registerProductionComponents(World& world) {
    world.registerComponent<BuildQueue>("BuildQueue");
}

void initialiseProduction(World& world, const Galaxy& galaxy) {
    for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
        world.add<BuildQueue>(galaxy.systemEntity(index),
                              BuildQueue{uint8_t(Hull::None), 0, 0, 0, 0, fx::zero()});
    }
}

void enqueueBuild(BuildQueue& queue, Hull hull, uint32_t count) {
    if (hull == Hull::None || count == 0) {
        queue.hull = uint8_t(Hull::None);
        queue.remaining = 0;
        queue.invested = fx::zero();
        return;
    }
    // Смена типа обнуляет вложенное. Недостроенный крейсер не превращается
    // в корвет: за перестройку планов на ходу надо платить.
    if (queue.hull != uint8_t(hull)) queue.invested = fx::zero();
    queue.hull = uint8_t(hull);
    queue.remaining = count;
}

void systemProduction(World& world, const TickContext& context) {
    const Galaxy* galaxy = world.resource<Galaxy>();
    Commands* commands = world.resource<Commands>();
    if (galaxy == nullptr || commands == nullptr) return;

    const std::vector<uint32_t> shipyards =
        countBuildingsPerSystem(world, Building::Shipyard, galaxy->systemCount());

    // Казна империй копируется в вектор, тратится по ходу обхода систем
    // и записывается обратно. Прямой доступ к компоненту Empire изнутри
    // обхода систем означал бы вложенный обход — а он и медленный,
    // и хрупкий.
    uint32_t empires = 0;
    world.each<Empire>([&](Entity, Empire& empire) {
        if (empire.id != kNoEmpire && empire.id + 1 > empires) empires = empire.id + 1;
    });
    if (empires == 0 || empires > 4096) return;

    std::vector<fx> treasury(empires, fx::zero());
    world.each<Empire>([&](Entity, Empire& empire) {
        if (empire.id < empires) treasury[empire.id] = empire.alloys;
    });

    world.each<StarSystem, Owner, BuildQueue>(
        [&](Entity, StarSystem& system, Owner& owner, BuildQueue& queue) {
            if (queue.hull == uint8_t(Hull::None) || queue.remaining == 0) return;
            if (owner.empire == kNoEmpire || owner.empire >= empires) return;
            if (system.index >= shipyards.size()) return;

            const uint32_t capacity = shipyards[system.index];
            if (capacity == 0) return;  // без верфи система строить не может

            const fx wanted = kBuildRateShipyard * fx::fromInt(capacity) * context.delta;
            // Тратим столько, сколько есть. При нехватке системы получают
            // сплавы в порядке обхода — порядок стабилен, поэтому результат
            // воспроизводим. Осмысленные приоритеты заказов появятся вместе
            // с интерфейсом; сейчас важнее, что распределение детерминировано.
            const fx spent = min(wanted, treasury[owner.empire]);
            if (spent <= fx::zero()) return;

            treasury[owner.empire] -= spent;
            queue.invested += spent;

            const fx cost = fx::fromInt(hullCost(Hull(queue.hull)));
            if (queue.invested < cost) return;

            queue.invested -= cost;
            --queue.remaining;

            Fleet built{};
            fleetAdd(built, Hull(queue.hull), 1);
            commands->spawnFleet(owner.empire, system.index, built);

            if (queue.remaining == 0) {
                queue.hull = uint8_t(Hull::None);
                // Недоиспользованный остаток не пропадает: он пойдёт
                // в следующий заказ того же типа.
            }
        });

    world.each<Empire>([&](Entity, Empire& empire) {
        if (empire.id < empires) empire.alloys = treasury[empire.id];
    });
}

void systemDisbandEmpty(World& world, const TickContext&) {
    Commands* commands = world.resource<Commands>();
    if (commands == nullptr) return;

    world.each<Fleet, FleetLocation>([&](Entity entity, Fleet& fleet, FleetLocation&) {
        if (fleetEmpty(fleet)) commands->destroy(entity);
    });
}

}  // namespace pw::sim
