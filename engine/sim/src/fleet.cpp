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

namespace {

/// Всё, что отличает один корпус от другого, — одной таблицей.
///
/// Раньше цена жила в switch, скорость в цепочке if, тоннаж в формуле,
/// а осадная мощь не жила нигде. Добавить корпус означало вспомнить
/// про четыре места; забыть одно — получить корабль, который ничего
/// не весит или летает с нулевой скоростью. Таблица делает пропуск
/// невозможным: у строки либо есть все поля, либо она не компилируется.
struct HullSpec {
    uint32_t cost;      // сплавов
    fx speed;           // игровых единиц в секунду
    uint32_t tonnage;   // вес в очках престижа и в оценке сил
    uint32_t siege;     // вклад в штурм планеты
};

const HullSpec& specOf(Hull hull) {
    static const HullSpec table[kHullClasses] = {
        /* корвет  */ {kCostCorvette,   kSpeedCorvette,   1,  0},
        /* тендер  */ {kCostTender,     kSpeedTender,     2,  0},
        /* эсминец */ {kCostDestroyer,  kSpeedDestroyer,  3,  1},
        /* носитель*/ {kCostCarrier,    kSpeedCarrier,    7,  2},
        /* крейсер */ {kCostCruiser,    kSpeedCruiser,    8,  3},
        /* монитор */ {kCostMonitor,    kSpeedMonitor,   10, 16},
        /* линкор  */ {kCostBattleship, kSpeedBattleship, 20, 6},
        /* титан   */ {kCostTitan,      kSpeedTitan,     70, 24},
    };
    static const HullSpec none{};
    if (hull == Hull::None || hull >= Hull::Count) return none;
    return table[size_t(hull) - 1];
}

}  // namespace

fx hullSpeed(Hull hull) { return specOf(hull).speed; }

fx fleetSpeed(const Fleet& fleet) {
    // Скорость флота задаёт самый медленный ПРИСУТСТВУЮЩИЙ корабль.
    // Перебираем все классы и берём минимум: цепочка if от медленного
    // к быстрому требовала помнить порядок скоростей, а он не совпадает
    // с порядком цен — монитор дешевле линкора и медленнее его.
    fx slowest = fx::zero();
    for (size_t index = 0; index < kHullClasses; ++index) {
        if (fleet.ships[index] == 0) continue;
        const fx speed = specOf(Hull(index + 1)).speed;
        if (slowest <= fx::zero() || speed < slowest) slowest = speed;
    }
    return slowest;
}

uint32_t fleetTonnage(const Fleet& fleet) {
    // Тоннаж растёт быстрее числа слотов: линкор дороже четырёх корветов
    // и в производстве, и в потерях.
    uint32_t total = 0;
    for (size_t index = 0; index < kHullClasses; ++index) {
        total += fleet.ships[index] * specOf(Hull(index + 1)).tonnage;
    }
    return total;
}

uint32_t fleetSiegePower(const Fleet& fleet) {
    uint32_t total = 0;
    for (size_t index = 0; index < kHullClasses; ++index) {
        total += fleet.ships[index] * specOf(Hull(index + 1)).siege;
    }
    return total;
}

fx fleetDamageControl(const Fleet& fleet) {
    uint32_t tenders = fleet[Hull::Tender];
    if (tenders == 0) return fx::zero();

    uint32_t ships = 0;
    for (size_t index = 0; index < kHullClasses; ++index) ships += fleet.ships[index];
    if (ships == 0) return fx::zero();

    // Один тендер на десять кораблей снимает десятую часть урона. Дальше
    // отдача падает: потолок в 35% выбран так, чтобы флот из одних тендеров
    // (которые не стреляют) всё равно проигрывал флоту с пушками.
    const fx share = fx::fromFraction(int64_t(tenders), int64_t(ships));
    return min(share, fx::fromFraction(7, 20));
}

uint32_t hullCost(Hull hull) { return specOf(hull).cost; }

void fleetAdd(Fleet& fleet, Hull hull, uint32_t count) {
    if (hull == Hull::None || hull >= Hull::Count) return;
    fleet[hull] += count;
}

uint32_t fleetCost(const Fleet& fleet) {
    uint32_t total = 0;
    for (size_t index = 0; index < kHullClasses; ++index) {
        total += fleet.ships[index] * specOf(Hull(index + 1)).cost;
    }
    return total;
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
                    for (size_t hull = 0; hull < kHullClasses; ++hull) {
                        into->ships[hull] += candidates[i].fleet.ships[hull];
                    }
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
