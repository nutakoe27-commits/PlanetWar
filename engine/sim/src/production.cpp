#include "pw/sim/production.h"

#include <algorithm>
#include <vector>

#include "pw/sim/combat.h"
#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/galaxy.h"

namespace pw::sim {

PlanetConstruction emptyConstruction() {
    PlanetConstruction site{};
    site.slot = PlanetConstruction::kNoSlot;
    site.building = uint8_t(Building::None);
    site.paid = 0;
    for (uint8_t i = 0; i < PlanetConstruction::kQueueLimit; ++i) {
        site.queueSlots[i] = PlanetConstruction::kNoSlot;
        site.queueBuildings[i] = uint8_t(Building::None);
    }
    site.elapsed = 0;
    return site;
}

void registerProductionComponents(World& world) {
    world.registerComponent<BuildQueue>("BuildQueue");
    world.registerComponent<PlanetConstruction>("PlanetConstruction");
}

void initialiseProduction(World& world, const Galaxy& galaxy) {
    for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
        world.add<BuildQueue>(galaxy.systemEntity(index),
                              BuildQueue{uint8_t(Hull::None), 0, 0, 0, 0, fx::zero()});
    }

    // Стройка навешивается СРАЗУ ВСЕМ планетам, включая ничьи: заказ тогда
    // становится записью в поле, а не добавлением компонента. Сущность не
    // переезжает между таблицами в момент, когда игрок нажал клавишу.
    //
    // Обход по указателю галактики, а не по миру: добавление компонента
    // меняет таблицы, и делать это прямо в обходе мира нельзя.
    for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
        for (uint32_t orbit = 0; orbit < galaxy.planetCount(index); ++orbit) {
            const Entity planet = galaxy.planetEntity(index, orbit);
            if (!planet.valid()) continue;
            world.add<PlanetConstruction>(planet, emptyConstruction());
        }
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

namespace {

/// Снять первый заказ из очереди и сделать его текущим.
void promoteQueued(PlanetConstruction& site) {
    site.elapsed = 0;
    site.paid = 0;
    if (site.queued == 0) {
        site.slot = PlanetConstruction::kNoSlot;
        site.building = uint8_t(Building::None);
        return;
    }
    site.slot = site.queueSlots[0];
    site.building = site.queueBuildings[0];
    for (uint8_t i = 1; i < site.queued; ++i) {
        site.queueSlots[i - 1] = site.queueSlots[i];
        site.queueBuildings[i - 1] = site.queueBuildings[i];
    }
    --site.queued;
    site.queueSlots[site.queued] = PlanetConstruction::kNoSlot;
    site.queueBuildings[site.queued] = uint8_t(Building::None);
}

/// Вычеркнуть из очереди все заказы на этот слот.
void dropQueued(PlanetConstruction& site, uint8_t slot) {
    uint8_t kept = 0;
    for (uint8_t i = 0; i < site.queued; ++i) {
        if (site.queueSlots[i] == slot) continue;
        site.queueSlots[kept] = site.queueSlots[i];
        site.queueBuildings[kept] = site.queueBuildings[i];
        ++kept;
    }
    for (uint8_t i = kept; i < PlanetConstruction::kQueueLimit; ++i) {
        site.queueSlots[i] = PlanetConstruction::kNoSlot;
        site.queueBuildings[i] = uint8_t(Building::None);
    }
    site.queued = kept;
}

}  // namespace

bool enqueueConstruction(PlanetConstruction& site, uint8_t slot, Building building) {
    if (slot == PlanetConstruction::kNoSlot) {
        // Отмена всего разом: и текущего, и очереди.
        site.queued = 0;
        for (uint8_t i = 0; i < PlanetConstruction::kQueueLimit; ++i) {
            site.queueSlots[i] = PlanetConstruction::kNoSlot;
            site.queueBuildings[i] = uint8_t(Building::None);
        }
        promoteQueued(site);
        return true;
    }

    if (building == Building::None) {
        dropQueued(site, slot);
        // Брошенная стройка теряет вложенное: за перестройку планов
        // на ходу надо платить — то же правило, что и на верфи.
        if (site.slot == slot) promoteQueued(site);
        return true;
    }

    if (site.slot == PlanetConstruction::kNoSlot) {
        site.slot = slot;
        site.building = uint8_t(building);
        site.elapsed = 0;
        site.paid = 0;
        return true;
    }

    if (site.queued >= PlanetConstruction::kQueueLimit) return false;
    site.queueSlots[site.queued] = slot;
    site.queueBuildings[site.queued] = uint8_t(building);
    ++site.queued;
    return true;
}

int64_t buildingTicks(Building building) {
    const int64_t cost = int64_t(buildingCost(building));
    return cost * kBuildSecondsPerMineral * kTicksPerSecond;
}

uint32_t constructionPercent(const PlanetConstruction& site) {
    if (site.slot == PlanetConstruction::kNoSlot || site.paid == 0) return 0;
    const int64_t total = buildingTicks(Building(site.building));
    if (total <= 0) return 0;
    const int64_t percent = int64_t(site.elapsed) * 100 / total;
    return uint32_t(std::clamp<int64_t>(percent, 0, 100));
}

// ---------------------------------------------------------------------------
// Казна империй
// ---------------------------------------------------------------------------

namespace {

/// Сколько всего империй в мире. Ноль означает «строить некому».
uint32_t empireCount(World& world) {
    uint32_t empires = 0;
    world.each<Empire>([&](Entity, Empire& empire) {
        if (empire.id != kNoEmpire && empire.id + 1 > empires) empires = empire.id + 1;
    });
    return empires > 4096 ? 0 : empires;
}

}  // namespace

void planetConstructionTick(World& world, const TickContext&) {
    const uint32_t empires = empireCount(world);
    if (empires == 0) return;

    // Казна копируется, тратится по ходу обхода и записывается обратно.
    // Прямой доступ к Empire изнутри обхода планет означал бы вложенный
    // обход — и медленный, и хрупкий. Тот же приём, что и на верфи.
    std::vector<fx> minerals(empires, fx::zero());
    world.each<Empire>([&](Entity, Empire& empire) {
        if (empire.id < empires) minerals[empire.id] = empire.minerals;
    });

    world.each<Planet, Owner, PlanetConstruction, PlanetDevelopment>(
        [&](Entity, Planet& planet, Owner& owner, PlanetConstruction& site,
            PlanetDevelopment& development) {
            if (site.slot == PlanetConstruction::kNoSlot) return;

            // Ничья планета не строит. Захваченная — тоже: стройку сбрасывает
            // осада в момент смены владельца, но проверка нужна и здесь, на
            // случай любого другого пути смены хозяина.
            if (owner.empire == kNoEmpire || owner.empire >= empires) return;
            if (site.slot >= planet.slots || site.slot >= kMaxSlots) {
                promoteQueued(site);
                return;
            }

            const uint32_t cost = buildingCost(Building(site.building));
            if (cost == 0) {
                promoteQueued(site);
                return;
            }

            // --- оплата ---
            //
            // ЦЕЛИКОМ И СРАЗУ. Размазанная по времени оплата давала тупик:
            // империя с десятью начатыми стройками делила скудный доход
            // на десять, ни одна не доходила до конца, шахты не появлялись —
            // и доход не рос никогда. Прогон сезона показал империю,
            // которая за два часа не построила ни одного здания.
            //
            // Заказ при этом не отменяется: он ждёт минералов. Игрок
            // намерение выразил, и отменять его за него — значит съесть
            // приказ.
            if (site.paid == 0) {
                const fx price = fx::fromInt(int64_t(cost));
                if (minerals[owner.empire] < price) return;
                minerals[owner.empire] -= price;
                site.paid = 1;
                site.elapsed = 0;
                return;
            }

            ++site.elapsed;
            if (int64_t(site.elapsed) < buildingTicks(Building(site.building))) return;

            development.buildings[site.slot] = site.building;
            promoteQueued(site);
        });

    world.each<Empire>([&](Entity, Empire& empire) {
        if (empire.id < empires) empire.minerals = minerals[empire.id];
    });
}

void systemProduction(World& world, const TickContext& context) {
    const Galaxy* galaxy = world.resource<Galaxy>();
    Commands* commands = world.resource<Commands>();
    if (galaxy == nullptr || commands == nullptr) return;

    // Верфи считаются ТОЛЬКО на планетах владельца системы. Чужой анклав,
    // удержавшийся на дальней орбите, не строит флот хозяину системы —
    // иначе захват половины системы дарил бы врагу вашу верфь.
    const uint32_t systemCount = galaxy->systemCount();
    std::vector<uint32_t> systemOwner(systemCount, kNoEmpire);
    world.each<StarSystem, Owner>([&](Entity, StarSystem& system, Owner& owner) {
        if (system.index < systemCount) systemOwner[system.index] = owner.empire;
    });

    std::vector<uint32_t> shipyards(systemCount, 0);
    world.each<Planet, Owner, PlanetDevelopment>(
        [&](Entity, Planet& planet, Owner& owner, PlanetDevelopment& development) {
            if (planet.system >= systemCount) return;
            if (owner.empire == kNoEmpire) return;
            if (owner.empire != systemOwner[planet.system]) return;
            const uint8_t limit = std::min<uint8_t>(planet.slots, kMaxSlots);
            for (uint8_t slot = 0; slot < limit; ++slot) {
                if (development.buildings[slot] == uint8_t(Building::Shipyard)) {
                    ++shipyards[planet.system];
                }
            }
        });

    // Казна империй копируется в вектор, тратится по ходу обхода систем
    // и записывается обратно. Прямой доступ к компоненту Empire изнутри
    // обхода систем означал бы вложенный обход — а он и медленный,
    // и хрупкий.
    const uint32_t empires = empireCount(world);
    if (empires == 0) return;

    std::vector<fx> treasury(empires, fx::zero());
    // Стандартное вооружение империи: компонент FleetArmament на её сущности.
    // Без него новые корабли получали бы абстрактно сбалансированный набор,
    // и выбор билда, сделанный игроком, не доезжал бы до верфи.
    std::vector<FleetArmament> doctrine(empires, balancedArmament());
    world.each<Empire>([&](Entity entity, Empire& empire) {
        if (empire.id >= empires) return;
        treasury[empire.id] = empire.alloys;
        if (const FleetArmament* armament = world.get<FleetArmament>(entity)) {
            doctrine[empire.id] = *armament;
        }
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
            commands->spawnFleet(owner.empire, system.index, built, &doctrine[owner.empire]);

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
