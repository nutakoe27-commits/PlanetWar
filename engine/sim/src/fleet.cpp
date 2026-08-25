#include "pw/sim/fleet.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "pw/sim/combat.h"
#include "pw/sim/commands.h"
#include "pw/sim/galaxy.h"

namespace pw::sim {

void registerFleetComponents(World& world) {
    world.registerComponent<Fleet>("Fleet");
    world.registerComponent<FleetLocation>("FleetLocation");
    world.registerComponent<MoveOrder>("MoveOrder");
}

fx fleetSpeed(const Fleet& fleet) {
    // Ищем самый медленный ПРИСУТСТВУЮЩИЙ класс. Порядок проверок от
    // медленного к быстрому, поэтому первое совпадение и есть ответ.
    if (fleet.battleships > 0) return kSpeedBattleship;
    if (fleet.cruisers > 0) return kSpeedCruiser;
    if (fleet.destroyers > 0) return kSpeedDestroyer;
    if (fleet.corvettes > 0) return kSpeedCorvette;
    return fx::zero();
}

uint32_t fleetTonnage(const Fleet& fleet) {
    // Тоннаж растёт быстрее числа слотов: линкор дороже четырёх корветов
    // и в производстве, и в потерях.
    return fleet.corvettes * 1u + fleet.destroyers * 3u +
           fleet.cruisers * 8u + fleet.battleships * 20u;
}

uint32_t hullCost(Hull hull) {
    switch (hull) {
        case Hull::Corvette:   return kCostCorvette;
        case Hull::Destroyer:  return kCostDestroyer;
        case Hull::Cruiser:    return kCostCruiser;
        case Hull::Battleship: return kCostBattleship;
        default:               return 0;
    }
}

void fleetAdd(Fleet& fleet, Hull hull, uint32_t count) {
    switch (hull) {
        case Hull::Corvette:   fleet.corvettes += count; break;
        case Hull::Destroyer:  fleet.destroyers += count; break;
        case Hull::Cruiser:    fleet.cruisers += count; break;
        case Hull::Battleship: fleet.battleships += count; break;
        default: break;
    }
}

uint32_t fleetCost(const Fleet& fleet) {
    return fleet.corvettes * kCostCorvette + fleet.destroyers * kCostDestroyer +
           fleet.cruisers * kCostCruiser + fleet.battleships * kCostBattleship;
}

void systemFleetMovement(World& world, const TickContext& context) {
    const Galaxy* galaxy = world.resource<Galaxy>();
    if (galaxy == nullptr) return;

    world.each<Fleet, FleetLocation, MoveOrder>(
        [&](Entity, Fleet& fleet, FleetLocation& location, MoveOrder& order) {
            const bool moving = location.system != location.nextSystem;

            if (!moving) {
                // Стоим. Есть ли куда идти?
                if (order.target == kNoSystem || order.target == location.system) {
                    order.target = kNoSystem;
                    return;
                }
                const int32_t hop = galaxy->nextHop(location.system, order.target);
                if (hop < 0) {
                    // Пути нет — цель недостижима. Приказ снимаем, а не
                    // оставляем висеть: иначе флот будет молча стоять,
                    // и игрок не поймёт, почему.
                    order.target = kNoSystem;
                    return;
                }
                location.nextSystem = uint32_t(hop);
                location.progress = fx::zero();
                return;
            }

            // В пути. Доля линии за тик = скорость * шаг / длина линии.
            const fx length = galaxy->laneLength(location.system, location.nextSystem);
            if (length <= fx::zero()) {
                // Линии больше нет: карта изменилась под флотом. Возвращаем
                // его в исходную систему, оттуда он пересчитает маршрут.
                location.nextSystem = location.system;
                location.progress = fx::zero();
                return;
            }

            const fx speed = fleetSpeed(fleet);
            if (speed <= fx::zero()) return;  // пустой флот никуда не идёт

            location.progress += (speed * context.delta) / length;

            if (location.progress < fx::one()) return;

            // Прибыли. Дробный остаток не переносим на следующую линию:
            // узел — это точка принятия решения, и флот в ней всегда
            // оказывается ровно.
            location.system = location.nextSystem;
            location.progress = fx::zero();

            if (location.system == order.target) {
                order.target = kNoSystem;  // дошли
            } else {
                const int32_t hop = galaxy->nextHop(location.system, order.target);
                if (hop < 0) {
                    order.target = kNoSystem;
                } else {
                    location.nextSystem = uint32_t(hop);
                }
            }
        });
}

namespace {

/// Ключ слияния: одна империя, одна система, одинаковое вооружение.
struct MergeKey {
    uint32_t system;
    uint32_t empire;
    uint8_t armament[8];

    bool operator<(const MergeKey& other) const {
        if (system != other.system) return system < other.system;
        if (empire != other.empire) return empire < other.empire;
        return std::memcmp(armament, other.armament, sizeof(armament)) < 0;
    }
    bool operator==(const MergeKey& other) const {
        return system == other.system && empire == other.empire &&
               std::memcmp(armament, other.armament, sizeof(armament)) == 0;
    }
};

struct MergeCandidate {
    MergeKey key;
    uint32_t entityIndex;
    Entity entity;
    Fleet fleet;
};

}  // namespace

void systemMergeFleets(World& world, const TickContext&) {
    Commands* commands = world.resource<Commands>();
    if (commands == nullptr) return;

    std::vector<MergeCandidate> candidates;
    world.each<Fleet, FleetLocation, MoveOrder, Owner>(
        [&](Entity entity, Fleet& fleet, FleetLocation& location, MoveOrder& order,
            Owner& owner) {
            // Сливаем только стоящие и без приказа: флот в пути или идущий
            // к цели — это отдельное намерение игрока, и склеивать его
            // с чужим нельзя.
            if (location.system != location.nextSystem) return;
            if (order.target != kNoSystem) return;
            if (fleetEmpty(fleet)) return;

            const FleetArmament* armament = world.get<FleetArmament>(entity);
            if (armament == nullptr) return;

            MergeCandidate candidate{};
            candidate.key.system = location.system;
            candidate.key.empire = owner.empire;
            std::memcpy(candidate.key.armament, armament, sizeof(candidate.key.armament));
            candidate.entityIndex = entity.index;
            candidate.entity = entity;
            candidate.fleet = fleet;
            candidates.push_back(candidate);
        });

    // Порядок полный: ключ, затем номер сущности. Принимающим становится
    // флот с наименьшим номером — выбор воспроизводим.
    std::sort(candidates.begin(), candidates.end(),
              [](const MergeCandidate& a, const MergeCandidate& b) {
                  if (!(a.key == b.key)) return a.key < b.key;
                  return a.entityIndex < b.entityIndex;
              });

    size_t index = 0;
    while (index < candidates.size()) {
        size_t end = index;
        while (end + 1 < candidates.size() && candidates[end + 1].key == candidates[index].key) {
            ++end;
        }
        if (end > index) {
            Fleet* into = world.get<Fleet>(candidates[index].entity);
            if (into != nullptr) {
                for (size_t i = index + 1; i <= end; ++i) {
                    into->corvettes += candidates[i].fleet.corvettes;
                    into->destroyers += candidates[i].fleet.destroyers;
                    into->cruisers += candidates[i].fleet.cruisers;
                    into->battleships += candidates[i].fleet.battleships;
                    // Опустошаем сразу: до применения буфера мир не должен
                    // содержать удвоенных кораблей.
                    Fleet* from = world.get<Fleet>(candidates[i].entity);
                    if (from != nullptr) *from = Fleet{};
                    commands->destroy(candidates[i].entity);
                }
            }
        }
        index = end + 1;
    }
}

}  // namespace pw::sim
