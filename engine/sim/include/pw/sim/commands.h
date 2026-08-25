// pw_sim — буфер отложенных структурных изменений.
//
// ЗАЧЕМ ОН НУЖЕН. Создание и удаление сущностей переносит строки между
// таблицами архетипов прямо под ногами у обхода: указатели на столбцы
// становятся недействительными, часть сущностей обходится дважды, часть
// не обходится вовсе. Поэтому обход мир менять не может, и это записано
// правилом в самом World.
//
// Но системам ЕСТЬ ЧТО менять: верфь достроила корабль, флот погиб в бою,
// два флота слились. Такие намерения копятся здесь и применяются после
// барьера, строго в порядке добавления.
//
// Порядок применения — часть детерминизма наравне с порядком систем. Если бы
// команды применялись по мере готовности или из хеш-таблицы, мир начал бы
// зависеть от планировщика потоков.
//
// Набор операций намеренно узкий и расширяется по мере надобности.
// Обобщённый буфер с типостиранием компонентов написать можно, но пока он
// решал бы задачу, которой нет.
#pragma once

#include <cstdint>
#include <vector>

#include "pw/sim/entity.h"
#include "pw/sim/fleet.h"

namespace pw::sim {

class World;

class Commands {
public:
    /// Создать флот в системе. Сущность появится при применении буфера.
    /// armament — вооружение новых кораблей; nullptr даёт сбалансированное.
    void spawnFleet(uint32_t empire, uint32_t system, const Fleet& composition,
                    const struct FleetArmament* armament = nullptr);

    /// Удалить сущность. Повторное удаление одной и той же безопасно.
    void destroy(Entity entity);

    /// Применить всё накопленное и очистить буфер.
    void apply(World& world);

    void clear();
    bool empty() const { return spawns_.empty() && destroys_.empty(); }
    size_t size() const { return spawns_.size() + destroys_.size(); }

private:
    struct SpawnFleet {
        uint32_t empire;
        uint32_t system;
        Fleet composition;
        uint8_t armament[8];   // FleetArmament, скопированный побайтово
        bool hasArmament;
    };

    std::vector<SpawnFleet> spawns_;
    std::vector<Entity> destroys_;
};

/// Применить буфер команд из ресурсов мира. Ставится ПОСЛЕДНЕЙ в расписании.
void systemApplyCommands(World& world, const TickContext& context);

}  // namespace pw::sim
