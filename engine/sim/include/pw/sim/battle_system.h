// pw_sim — сведение боёв в мире.
//
// Здесь чистая функция resolveBattle из combat.h встречается с реальностью:
// надо найти, кто с кем столкнулся, собрать стороны, разнести потери обратно
// по флотам и увести разбитого.
//
// ТЕМП. Сражение разыгрывается не каждый тик, а раз в минуту игрового
// времени. Иначе два флота в одной системе перемалывали бы друг друга
// десять раз в секунду, и бой длился бы доли мгновения — а он должен идти
// достаточно долго, чтобы подкрепление успело вмешаться.
#pragma once

#include <cstdint>

#include "pw/sim/combat.h"
#include "pw/sim/schedule.h"
#include "pw/sim/world.h"

namespace pw::sim {

/// Сколько игрового времени проходит между сражениями в одной системе.
inline constexpr int64_t kBattleIntervalSeconds = 60;

/// Состояние сражений в системе. Висит на сущности системы.
struct BattleState {
    uint32_t cooldown;    // тиков до следующего сражения
    uint32_t lastRounds;  // сколько раундов длилось последнее — для интерфейса
};
static_assert(sizeof(BattleState) == 8, "структура обязана быть без дыр");

/// Свести и разыграть все сражения этого тика.
///
/// Идёт ПОСЛЕ движения флотов (кто куда пришёл, уже известно) и ДО осады:
/// разбитый осаждающий не должен продолжать осаду в том же тике.
void systemBattles(World& world, const TickContext& context);

void registerBattleComponents(World& world);

/// Навесить состояние сражений на все системы галактики.
void initialiseBattles(World& world, const class Galaxy& galaxy);

}  // namespace pw::sim
