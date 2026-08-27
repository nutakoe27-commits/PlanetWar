#include "pw/sim/battle_system.h"

#include <algorithm>
#include <map>
#include <vector>

#include "pw/core/hash.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

namespace pw::sim {

void registerBattleComponents(World& world) {
    world.registerComponent<FleetArmament>("FleetArmament");
    world.registerComponent<BattleState>("BattleState");
}

void initialiseBattles(World& world, const Galaxy& galaxy) {
    for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
        world.add<BattleState>(galaxy.systemEntity(index),
                               BattleState{0, 0, kBattleNobody, kBattleNobody, 0, 0, 0});
    }
}

namespace {

/// Один флот, стоящий в системе.
struct Present {
    uint32_t system;
    uint32_t empire;
    uint32_t entityIndex;   // для полного порядка сортировки
    Entity entity;
    Fleet fleet;
    FleetArmament armament;
    uint32_t tonnage;
};

/// Сторона сражения: сумма флотов одной империи в одной системе.
struct Party {
    uint32_t empire = kNoEmpire;
    Fleet fleet{};
    uint32_t tonnage = 0;
    // Вооружение усредняется по тоннажу: мнение большого флота весит больше.
    int64_t kinetic = 0, energy = 0, missile = 0;
    int64_t pointDefense = 0, shields = 0, armour = 0;
    uint32_t drydocks = 0;
    uint8_t doctrine = uint8_t(Doctrine::Line);
    uint32_t doctrineWeight = 0;
    size_t first = 0, last = 0;   // диапазон в списке присутствующих
};

void absorb(Party& party, const Present& present) {
    for (size_t hull = 0; hull < kHullClasses; ++hull) {
        party.fleet.ships[hull] += present.fleet.ships[hull];
    }
    party.tonnage += present.tonnage;

    const int64_t weight = int64_t(present.tonnage);
    party.kinetic += int64_t(present.armament.kinetic) * weight;
    party.energy += int64_t(present.armament.energy) * weight;
    party.missile += int64_t(present.armament.missile) * weight;
    party.pointDefense += int64_t(present.armament.pointDefense) * weight;
    party.shields += int64_t(present.armament.shields) * weight;
    party.armour += int64_t(present.armament.armour) * weight;

    // Доктрину задаёт крупнейший флот стороны: у сражения один командующий.
    if (present.tonnage > party.doctrineWeight) {
        party.doctrineWeight = present.tonnage;
        party.doctrine = present.armament.doctrine;
    }
}

BattleSide toSide(const Party& party) {
    BattleSide side;
    side.fleet = party.fleet;
    const int64_t weight = party.tonnage > 0 ? int64_t(party.tonnage) : 1;
    side.armament.kinetic = uint8_t(party.kinetic / weight);
    side.armament.energy = uint8_t(party.energy / weight);
    side.armament.missile = uint8_t(party.missile / weight);
    side.armament.pointDefense = uint8_t(party.pointDefense / weight);
    side.armament.shields = uint8_t(party.shields / weight);
    side.armament.armour = uint8_t(party.armour / weight);
    side.armament.doctrine = party.doctrine;
    side.doctrine = Doctrine(party.doctrine);
    side.drydocks = party.drydocks;
    return side;
}

/// Разнести потери стороны по её флотам.
///
/// Пропорционально вкладу каждого флота, остаток — крупнейшему. Крупнейший
/// определяется по тоннажу, при равенстве — по номеру сущности, поэтому
/// распределение воспроизводимо.
void distribute(World& world, std::vector<Present>& present, size_t first, size_t last,
                const Fleet& losses) {
    // Суммы по классам внутри стороны.
    uint32_t sums[kHullClasses] = {};
    for (size_t i = first; i <= last; ++i) {
        for (size_t hull = 0; hull < kHullClasses; ++hull) {
            sums[hull] += present[i].fleet.ships[hull];
        }
    }
    uint32_t assigned[kHullClasses] = {};

    // Крупнейший флот стороны: ему достанется остаток от округления.
    size_t biggest = first;
    for (size_t i = first; i <= last; ++i) {
        if (present[i].tonnage > present[biggest].tonnage) biggest = i;
    }

    for (size_t i = first; i <= last; ++i) {
        Fleet* fleet = world.get<Fleet>(present[i].entity);
        if (fleet == nullptr) continue;

        for (size_t hull = 0; hull < kHullClasses; ++hull) {
            const uint32_t wanted = losses.ships[hull];
            const uint32_t owned = present[i].fleet.ships[hull];
            if (sums[hull] == 0 || wanted == 0) continue;

            uint32_t take = uint32_t(uint64_t(wanted) * owned / sums[hull]);
            if (i == biggest) {
                // Остаток от целочисленного деления добираем здесь, иначе
                // часть потерь потерялась бы при округлении вниз.
                take += wanted - assigned[hull] - take;
            }
            take = std::min(take, fleet->ships[hull]);
            fleet->ships[hull] -= take;
            assigned[hull] += take;
        }
    }
}

/// Куда отходить проигравшему.
///
/// Своя соседняя система, иначе любая соседняя, иначе некуда — тогда флот
/// остаётся и будет добит. Выбор по наименьшему номеру: он воспроизводим,
/// а осмысленный выбор направления появится вместе с постоянными приказами.
uint32_t retreatTarget(const Galaxy& galaxy, const std::vector<uint32_t>& owners,
                       uint32_t from, uint32_t empire) {
    uint32_t fallback = kNoSystem;
    for (uint32_t k = 0; k < galaxy.neighborCount(from); ++k) {
        const uint32_t neighbour = galaxy.neighbors(from)[k];
        if (neighbour < owners.size() && owners[neighbour] == empire) return neighbour;
        if (fallback == kNoSystem) fallback = neighbour;
    }
    return fallback;
}

}  // namespace

void systemBattles(World& world, const TickContext& context) {
    const Galaxy* galaxy = world.resource<Galaxy>();
    if (galaxy == nullptr) return;
    const uint32_t systemCount = galaxy->systemCount();

    // --- владельцы систем ---
    std::vector<uint32_t> owners(systemCount, kNoEmpire);
    world.each<StarSystem, Owner>([&](Entity, StarSystem& system, Owner& owner) {
        if (system.index < systemCount) owners[system.index] = owner.empire;
    });

    // --- кто где стоит ---
    //
    // Только стоящие флоты: летящий находится между узлами и в сражении
    // не участвует. Флот, вышедший из системы, не должен в ней воевать.
    std::vector<Present> present;
    world.each<Fleet, FleetLocation, Owner, FleetArmament>(
        [&](Entity entity, Fleet& fleet, FleetLocation& location, Owner& owner,
            FleetArmament& armament) {
            if (location.system != location.nextSystem) return;
            if (location.system >= systemCount) return;
            if (fleetEmpty(fleet)) return;
            present.push_back(Present{location.system, owner.empire, entity.index, entity,
                                      fleet, armament, fleetTonnage(fleet)});
        });

    // Полный порядок сортировки: система, империя, номер сущности. Без
    // последнего ключа два флота одной империи могли бы встать в любом
    // порядке, и разнос потерь перестал бы воспроизводиться.
    std::sort(present.begin(), present.end(), [](const Present& a, const Present& b) {
        if (a.system != b.system) return a.system < b.system;
        if (a.empire != b.empire) return a.empire < b.empire;
        return a.entityIndex < b.entityIndex;
    });

    // --- ремонтные доки: чья территория ---
    //
    // Считаем ДО боя и по владельцу ПЛАНЕТЫ, а не системы: система может
    // быть спорной, а док стоит на конкретной планете конкретного хозяина,
    // и чинит он только его корабли.
    std::map<uint64_t, uint32_t> drydocks;
    world.each<Planet, PlanetDevelopment, Owner>(
        [&](Entity, Planet& planet, PlanetDevelopment& development, Owner& owner) {
            if (owner.empire == kNoEmpire || planet.system >= systemCount) return;
            const uint8_t count = countBuildings(planet, development, Building::Drydock);
            if (count == 0) return;
            drydocks[(uint64_t(planet.system) << 32) | owner.empire] += count;
        });

    // --- откат сражений и поиск столкновений ---
    std::vector<BattleState*> states(systemCount, nullptr);
    world.each<StarSystem, BattleState>([&](Entity, StarSystem& system, BattleState& state) {
        if (system.index < systemCount) states[system.index] = &state;
    });

    for (BattleState* state : states) {
        if (state != nullptr && state->cooldown > 0) --state->cooldown;
    }

    size_t index = 0;
    while (index < present.size()) {
        const uint32_t system = present[index].system;
        size_t systemEnd = index;
        while (systemEnd + 1 < present.size() && present[systemEnd + 1].system == system) {
            ++systemEnd;
        }

        // Стороны: по одной на империю.
        std::vector<Party> parties;
        for (size_t i = index; i <= systemEnd; ++i) {
            if (parties.empty() || parties.back().empire != present[i].empire) {
                Party party;
                party.empire = present[i].empire;
                party.first = i;
                parties.push_back(party);
            }
            absorb(parties.back(), present[i]);
            parties.back().last = i;
        }
        for (Party& party : parties) {
            const auto found = drydocks.find((uint64_t(system) << 32) | party.empire);
            party.drydocks = found == drydocks.end() ? 0u : found->second;
        }

        const size_t next = systemEnd + 1;
        if (parties.size() < 2 || states[system] == nullptr || states[system]->cooldown > 0) {
            index = next;
            continue;
        }

        // Кто с кем. Владелец системы сражается всегда, если он здесь:
        // это его дом. Иначе бьются два сильнейших.
        size_t a = 0, b = 1;
        for (size_t i = 0; i < parties.size(); ++i) {
            if (parties[i].empire == owners[system]) { a = i; break; }
        }
        uint32_t bestTonnage = 0;
        bool found = false;
        for (size_t i = 0; i < parties.size(); ++i) {
            if (i == a) continue;
            if (!found || parties[i].tonnage > bestTonnage) {
                bestTonnage = parties[i].tonnage;
                b = i;
                found = true;
            }
        }
        if (!found) {
            index = next;
            continue;
        }

        // Собственный поток случайности на сражение: от сида сезона, номера
        // системы и тика. Ни порядок обхода, ни другие подсистемы на исход
        // не влияют.
        Rng rng(mixCoord(galaxy->seed(), int64_t(system), int64_t(context.tick)), /*stream=*/7);
        const BattleResult result = resolveBattle(toSide(parties[a]), toSide(parties[b]), rng);

        distribute(world, present, parties[a].first, parties[a].last, result.lossesA);
        distribute(world, present, parties[b].first, parties[b].last, result.lossesB);

        states[system]->lastRounds = uint16_t(result.rounds);
        states[system]->cooldown = uint32_t(kBattleIntervalSeconds * kTicksPerSecond);

        // Исход запоминаем здесь же: сервер прочитает его и скажет игрокам.
        // И при ничьей запоминаем УЧАСТНИКОВ, а не одну метку: сервер
        // обязан сказать о бое обеим сторонам, а для этого их надо знать.
        states[system]->drawn = result.outcome == 2 ? 1u : 0u;
        if (result.outcome == 2) {
            states[system]->lastWinner = uint8_t(parties[a].empire & 0xFFu);
            states[system]->lastLoser = uint8_t(parties[b].empire & 0xFFu);
        } else {
            const Party& winner = result.outcome == 0 ? parties[a] : parties[b];
            const Party& loser = result.outcome == 0 ? parties[b] : parties[a];
            states[system]->lastWinner = uint8_t(winner.empire & 0xFFu);
            states[system]->lastLoser = uint8_t(loser.empire & 0xFFu);
        }

        // Проигравший отходит. Победитель остаётся: система за ним.
        if (result.outcome != 2) {
            const Party& loser = result.outcome == 0 ? parties[b] : parties[a];
            const uint32_t target = retreatTarget(*galaxy, owners, system, loser.empire);
            if (target != kNoSystem) {
                for (size_t i = loser.first; i <= loser.last; ++i) {
                    Fleet* fleet = world.get<Fleet>(present[i].entity);
                    if (fleet == nullptr || fleetEmpty(*fleet)) continue;
                    MoveOrder* order = world.get<MoveOrder>(present[i].entity);
                    if (order != nullptr) order->target = target;
                }
            }
        }

        index = next;
    }
}

}  // namespace pw::sim
