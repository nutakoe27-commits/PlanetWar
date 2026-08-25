#include "pw/sim/fleet.h"

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

}  // namespace pw::sim
