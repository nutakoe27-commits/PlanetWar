#include "pw/sim/commands.h"

#include <cstring>
#include <vector>

#include "pw/sim/combat.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/world.h"

namespace pw::sim {

namespace {

/// Наименьший свободный номер отряда у этой империи.
///
/// НОМЕР ПОГИБШЕГО ОТРЯДА ОСВОБОЖДАЕТСЯ. Игрок ждёт именно этого:
/// «третьего больше нет, значит следующий будет третьим». Сквозной
/// счётчик за сезон дорос бы до четырёхзначных номеров, и «отряд 1743»
/// перестал бы что-либо называть.
///
/// Обход всех отрядов империи на каждое появление. Появления редки —
/// корабль со стапеля, разделение по приказу, — а состояния, которое
/// надо было бы держать в синхронизации, здесь нет вовсе.
uint16_t nextTag(World& world, uint32_t empire) {
    constexpr size_t kLimit = 4096;
    std::vector<bool> used(kLimit, false);
    world.each<FleetOrders, Owner>([&](Entity, FleetOrders& orders, Owner& owner) {
        if (owner.empire != empire) return;
        if (orders.tag > 0 && size_t(orders.tag) < kLimit) used[orders.tag] = true;
    });
    for (size_t tag = 1; tag < kLimit; ++tag) {
        if (!used[tag]) return uint16_t(tag);
    }
    // Больше четырёх тысяч отрядов у одной империи. Номера кончились,
    // но флот всё равно обязан появиться: безымянный отряд лучше,
    // чем ненайденный корабль.
    return 0;
}

}  // namespace

void Commands::spawnFleet(uint32_t empire, uint32_t system, const Fleet& composition,
                          const FleetArmament* armament, const FleetBirth& birth) {
    SpawnFleet spawn{empire, system, composition, {}, armament != nullptr, birth};
    if (armament != nullptr) std::memcpy(spawn.armament, armament, sizeof(FleetArmament));
    spawns_.push_back(spawn);
}

void Commands::destroy(Entity entity) {
    destroys_.push_back(entity);
}

void Commands::apply(World& world) {
    // Сначала появление, потом исчезновение.
    //
    // Порядок не произволен: так только что созданный флот можно удалить
    // в том же тике, если он оказался пустым, и не остаётся полутиковых
    // состояний, которые видно снаружи.
    for (const SpawnFleet& spawn : spawns_) {
        // Номер считается ДО создания компонента: иначе новый отряд попал
        // бы в собственный обход с нулевым номером и занял бы единицу.
        const uint16_t tag = nextTag(world, spawn.empire);

        const Entity entity = world.create();
        world.add<Fleet>(entity, spawn.composition);
        world.add<FleetLocation>(entity, standingAt(spawn.system));

        FleetOrders orders = idleOrders(
            spawn.birth.anchor == kNoSystem ? spawn.system : spawn.birth.anchor,
            spawn.birth.anchorOrbit, tag, spawn.birth.stance);
        orders.evade = spawn.birth.evade ? 1u : 0u;
        world.add<FleetOrders>(entity, orders);
        world.add<Owner>(entity, Owner{spawn.empire, 0});
        // Сбалансированное вооружение по умолчанию. Без него флот не попал бы
        // в обход сражений вовсе и стал бы неуязвимым призраком.
        // Новый корабль получает вооружение своей империи, а не абстрактно
        // сбалансированное: иначе выбор билда, сделанный игроком, не доезжал
        // бы до построенных кораблей вовсе.
        FleetArmament armament = balancedArmament();
        if (spawn.hasArmament) std::memcpy(&armament, spawn.armament, sizeof(FleetArmament));
        world.add<FleetArmament>(entity, armament);
    }

    for (const Entity entity : destroys_) {
        world.destroy(entity);  // повторное удаление безопасно: alive его отсеет
    }

    clear();
}

void Commands::clear() {
    spawns_.clear();
    destroys_.clear();
}

void systemApplyCommands(World& world, const TickContext&) {
    Commands* commands = world.resource<Commands>();
    if (commands == nullptr) return;
    commands->apply(world);
}

}  // namespace pw::sim
