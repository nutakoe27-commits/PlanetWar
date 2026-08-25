#include "pw/sim/commands.h"

#include <cstring>

#include "pw/sim/combat.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/world.h"

namespace pw::sim {

void Commands::spawnFleet(uint32_t empire, uint32_t system, const Fleet& composition,
                          const FleetArmament* armament) {
    SpawnFleet spawn{empire, system, composition, {}, armament != nullptr};
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
        const Entity entity = world.create();
        world.add<Fleet>(entity, spawn.composition);
        world.add<FleetLocation>(entity, FleetLocation{spawn.system, spawn.system, fx::zero()});
        world.add<MoveOrder>(entity, MoveOrder{kNoSystem, 0});
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
