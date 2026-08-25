// pw_season — прогон сезона на ботах.
//
// Первый запуск, где ВСЕ подсистемы Фазы 1 работают вместе: галактика,
// флоты, поиск пути, владение, осада, экономика, производство и бой.
//
// Боты нарочно тупые: строят по списку, шлют флот в ближайшую ничью систему,
// заказывают корабли, когда есть сплавы. Умный бот сейчас только помешал бы —
// проверяется не он, а то, что механики стыкуются между собой и дают
// осмысленную динамику, а не вырождаются.
//
// Это тот самый ночной прогон из docs/04-EDITOR-AND-TOOLS.md, только пока
// без отчёта в CI. Он же — предшественник экономического солвера.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pw/sim/battle_system.h"
#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/production.h"

using namespace pw;
using namespace pw::sim;

namespace {

/// Порядок застройки планеты. Простая очередь приоритетов: сначала сырьё,
/// потом переработка, потом верфи, потом энергия под содержание флота.
const Building kBuildOrder[] = {
    Building::Mine, Building::Mine, Building::Mine,
    Building::Foundry, Building::Foundry,
    Building::Shipyard,
    Building::PowerPlant, Building::PowerPlant,
    Building::Mine, Building::Foundry,
    Building::Laboratory, Building::Fortress,
};

struct Bot {
    uint32_t empire = 0;
    Entity entity;
    uint32_t home = 0;
    /// Чем вооружается эта империя. Разные боты — разные билды, иначе
    /// контр-система никак себя не проявит.
    FleetArmament doctrine{};
    const char* name = "";
};

/// Слепок одного флота: чтобы после боя увидеть, что именно он потерял.
struct Snapshot {
    Entity entity;
    uint32_t system;
    uint32_t empire;
    Fleet fleet;
};

void snapshot(World& world, std::vector<Snapshot>& out) {
    out.clear();
    world.each<Fleet, FleetLocation, Owner>(
        [&](Entity entity, Fleet& fleet, FleetLocation& location, Owner& owner) {
            if (location.system != location.nextSystem) return;
            out.push_back(Snapshot{entity, location.system, owner.empire, fleet});
        });
}

/// Напечатать, кто и сколько потерял за только что прошедший тик боёв.
///
/// Эта диагностика нашла главный дефект модели потерь: флот 4/3/1/0 получал
/// три процента урона и терял единственный крейсер — то есть больше половины
/// тоннажа. Ни один из юнит-тестов боя этого не видел.
void reportLosses(World& world, const std::vector<Snapshot>& before, int64_t tick) {
    for (const Snapshot& was : before) {
        const Fleet* now = world.get<Fleet>(was.entity);
        const Fleet left = now != nullptr ? *now : Fleet{};
        if (left.corvettes == was.fleet.corvettes && left.destroyers == was.fleet.destroyers &&
            left.cruisers == was.fleet.cruisers && left.battleships == was.fleet.battleships) {
            continue;
        }
        std::printf("[бой t=%lld] система %u, империя %u: %u/%u/%u/%u -> %u/%u/%u/%u\n",
                    static_cast<long long>(tick), was.system, was.empire,
                    was.fleet.corvettes, was.fleet.destroyers, was.fleet.cruisers,
                    was.fleet.battleships, left.corvettes, left.destroyers, left.cruisers,
                    left.battleships);
    }
}

/// Суммарный тоннаж всех живых флотов.
uint32_t totalTonnage(World& world) {
    uint32_t total = 0;
    world.each<Fleet>([&](Entity, Fleet& fleet) { total += fleetTonnage(fleet); });
    return total;
}

struct Stats {
    uint32_t systems = 0;
    uint32_t planets = 0;
    uint32_t buildings = 0;
    uint32_t tonnage = 0;
    uint32_t fleets = 0;
};

FleetArmament build(uint8_t k, uint8_t e, uint8_t m, uint8_t sh, uint8_t ar, uint8_t pd,
                    Doctrine d) {
    FleetArmament a{};
    a.kinetic = k; a.energy = e; a.missile = m;
    a.shields = sh; a.armour = ar; a.pointDefense = pd;
    a.doctrine = uint8_t(d);
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t seed = 0x5EA50FF;
    uint32_t systems = 200;
    int64_t hours = 6;
    bool traceBattles = false;
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--systems" && i + 1 < argc) systems = uint32_t(std::atoi(argv[++i]));
        else if (arg == "--hours" && i + 1 < argc) hours = std::atoi(argv[++i]);
        else if (arg == "--battles") traceBattles = true;
        else if (arg == "--check") check = true;
        else {
            std::printf("pw_season — прогон сезона на ботах\n\n"
                        "  --seed <n>     сид сезона\n"
                        "  --systems <n>  размер галактики\n"
                        "  --hours <n>    сколько игровых часов прогнать\n"
                        "  --battles      печатать потери каждого сражения\n"
                        "  --check        проверять инварианты, код возврата 1 при нарушении\n");
            return arg == "--help" ? 0 : 2;
        }
    }

    World world;
    Galaxy galaxy;
    Ledger ledger;
    Commands commands;
    Presence presence;

    registerGalaxyComponents(world);
    registerFleetComponents(world);
    registerControlComponents(world);
    registerEconomyComponents(world);
    registerProductionComponents(world);
    registerBattleComponents(world);

    GalaxyParams params;
    params.seed = seed;
    params.systemCount = systems;
    galaxy.generate(world, params);
    initialiseControl(world, galaxy);
    initialiseEconomy(world);
    initialiseProduction(world, galaxy);
    initialiseBattles(world, galaxy);

    world.setResource(&galaxy);
    world.setResource(&ledger);
    world.setResource(&commands);
    world.setResource(&presence);

    // --- боты ---
    //
    // Четыре империи с РАЗНЫМИ билдами: иначе контр-система не проявится
    // и прогон покажет только, кто быстрее строил.
    std::vector<Bot> bots = {
        {0, {}, 0, build(100, 0, 0, 20, 60, 20, Doctrine::Line),     "Кинетики"},
        {1, {}, 0, build(0, 100, 0, 60, 20, 20, Doctrine::Envelop),  "Энергетики"},
        {2, {}, 0, build(0, 0, 100, 20, 60, 20, Doctrine::Line),     "Ракетчики"},
        {3, {}, 0, build(40, 30, 30, 40, 40, 20, Doctrine::Evasive), "Универсалы"},
    };

    // Стартовые системы разносим по галактике, чтобы они не оказались
    // соседями и первая встреча случилась не на второй минуте.
    const uint32_t count = galaxy.systemCount();
    for (size_t i = 0; i < bots.size(); ++i) {
        bots[i].home = uint32_t((i * count) / bots.size());
        bots[i].entity = world.create();
        world.add<Empire>(bots[i].entity,
                          Empire{fx::fromInt(500), fx::fromInt(200), fx::fromInt(300),
                                 fx::zero(), fx::zero(), bots[i].empire, bots[i].home});
        // Вооружение империи: его унаследуют все построенные корабли.
        world.add<FleetArmament>(bots[i].entity, bots[i].doctrine);
        world.get<Owner>(galaxy.systemEntity(bots[i].home))->empire = bots[i].empire;
        world.get<SystemDefense>(galaxy.systemEntity(bots[i].home))->readiness = kReadinessMax;

        // Стартовый флот.
        const Entity fleet = world.create();
        world.add<Fleet>(fleet, Fleet{8, 2, 0, 0});
        world.add<FleetLocation>(fleet, FleetLocation{bots[i].home, bots[i].home, fx::zero()});
        world.add<MoveOrder>(fleet, MoveOrder{kNoSystem, 0});
        world.add<Owner>(fleet, Owner{bots[i].empire, 0});
        world.add<FleetArmament>(fleet, bots[i].doctrine);
    }

    std::printf("Сезон на ботах, сид 0x%llX\n", static_cast<unsigned long long>(seed));
    std::printf("  галактика %u систем, %d игровых часов, %zu империй\n\n",
                count, int(hours), bots.size());

    const int64_t totalTicks = hours * 3600 * kTicksPerSecond;
    const int64_t policyEvery = 10 * kTicksPerSecond;   // бот думает раз в 10 секунд
    const int64_t reportEvery = totalTicks / 6;

    std::vector<uint32_t> owners(count, kNoEmpire);
    uint32_t battlesSeen = 0;
    uint32_t violations = 0;
    std::vector<uint32_t> peakSystems(bots.size(), 0);

    for (int64_t tick = 0; tick < totalTicks; ++tick) {
        TickContext context;
        context.tick = uint64_t(tick);

        // --- политика ботов ---
        if (tick % policyEvery == 0) {
            std::fill(owners.begin(), owners.end(), kNoEmpire);
            world.each<StarSystem, Owner>([&](Entity, StarSystem& s, Owner& o) {
                if (s.index < count) owners[s.index] = o.empire;
            });

            for (const Bot& bot : bots) {
                Empire* empire = world.get<Empire>(bot.entity);

                // 1. Достроить планеты в своих системах.
                //    Здания пока бесплатны — стоимость появится на Фазе 2
                //    вместе с очередью строительства.
                world.each<Planet, PlanetDevelopment>(
                    [&](Entity, Planet& planet, PlanetDevelopment& development) {
                        if (planet.system >= count) return;
                        if (owners[planet.system] != bot.empire) return;
                        const uint8_t limit = std::min<uint8_t>(planet.slots, kMaxSlots);
                        for (uint8_t slot = 0; slot < limit; ++slot) {
                            if (development.buildings[slot] != uint8_t(Building::None)) continue;
                            development.buildings[slot] =
                                uint8_t(kBuildOrder[slot % (sizeof(kBuildOrder) /
                                                            sizeof(kBuildOrder[0]))]);
                            return;  // по одному зданию за раз
                        }
                    });

                // 2. Заказать корабль там, где есть верфь и хватает сплавов.
                //
                // Берём самый крупный корпус, который империя может себе
                // позволить С ЗАПАСОМ, — и обязательно доходим до корвета.
                //
                // Прежняя версия начинала с эсминца, и разорённая империя
                // намертво застревала: доход давал 220-310 сплавов, порог
                // был 320, и она не строила НИЧЕГО шесть часов подряд.
                // Это выглядело как проблема баланса, а было дырой в боте.
                const uint32_t affordable =
                    empire->alloys > fx::fromInt(kCostBattleship * 2)  ? kCostBattleship
                    : empire->alloys > fx::fromInt(kCostCruiser * 2)   ? kCostCruiser
                    : empire->alloys > fx::fromInt(kCostDestroyer * 2) ? kCostDestroyer
                    : empire->alloys > fx::fromInt(kCostCorvette)      ? kCostCorvette
                                                                      : 0u;
                if (affordable > 0) {
                    const Hull hull = affordable == kCostBattleship ? Hull::Battleship
                                    : affordable == kCostCruiser    ? Hull::Cruiser
                                    : affordable == kCostDestroyer  ? Hull::Destroyer
                                                                    : Hull::Corvette;
                    world.each<StarSystem, Owner, BuildQueue>(
                        [&](Entity, StarSystem&, Owner& o, BuildQueue& queue) {
                            if (o.empire != bot.empire) return;
                            if (queue.remaining > 0) return;
                            enqueueBuild(queue, hull, 1);
                        });
                }

                // 3. Отправить простаивающий флот: сначала к ближайшей ничьей
                //    системе, а когда ничьих не осталось — к ближайшей чужой.
                //
                // Без второй половины правила галактика делилась за час, флоты
                // вставали на месте и слияние копило их в один отряд из 839
                // крейсеров. Прогон переставал проверять бой вообще.
                world.each<Fleet, FleetLocation, MoveOrder, Owner>(
                    [&](Entity, Fleet&, FleetLocation& location, MoveOrder& order,
                        Owner& owner) {
                        if (owner.empire != bot.empire) return;
                        if (order.target != kNoSystem) return;
                        if (location.system != location.nextSystem) return;
                        if (owners[location.system] == kNoEmpire) return;  // занимаем эту

                        int32_t best = -1;
                        int32_t bestHops = 1 << 20;
                        int32_t enemy = -1;
                        int32_t enemyHops = 1 << 20;
                        for (uint32_t target = 0; target < count; ++target) {
                            if (owners[target] == bot.empire) continue;
                            const int32_t hops = galaxy.hopDistance(location.system, target);
                            if (hops < 0) continue;
                            if (owners[target] == kNoEmpire) {
                                if (hops < bestHops) { bestHops = hops; best = int32_t(target); }
                            } else if (hops < enemyHops) {
                                enemyHops = hops;
                                enemy = int32_t(target);
                            }
                        }
                        if (best >= 0)       order.target = uint32_t(best);
                        else if (enemy >= 0) order.target = uint32_t(enemy);
                    });
            }
        }

        // --- тик мира ---
        systemFleetMovement(world, context);

        // Журнал сражений. Снимок делается ПОСЛЕ движения: флот, прилетевший
        // в этом же тике, тоже участвует в бою, и без него разбор потерь
        // показывал бы только одну сторону.
        //
        // Разбор живёт в инструменте, а не в pw_sim: библиотека симуляции
        // ничего не печатает.
        std::vector<Snapshot> before;
        if (traceBattles) snapshot(world, before);

        // Инварианты режима --check. Проверяются КАЖДЫЙ тик: дефект,
        // проявляющийся раз в час, иначе не поймать.
        const uint32_t beforeBattle = check ? totalTonnage(world) : 0;

        systemBattles(world, context);
        if (traceBattles) reportLosses(world, before, tick);

        if (check && totalTonnage(world) > beforeBattle) {
            std::printf("НАРУШЕНИЕ t=%lld: бой ДОБАВИЛ тоннаж (%u -> %u)\n",
                        static_cast<long long>(tick), beforeBattle, totalTonnage(world));
            ++violations;
        }

        systemPresence(world, context);
        systemSiege(world, context);
        systemEconomy(world, context);
        systemDefenceCap(world, context);
        systemProduction(world, context);

        // Слияние не имеет права ни терять, ни удваивать корабли: оно только
        // переносит их из отряда в отряд.
        //
        // Срез берётся вплотную вокруг systemMergeFleets. Дальше в тике
        // применяется буфер команд, а там появляются построенные корабли —
        // и более широкая рамка ловила бы производство, а не слияние.
        const uint32_t beforeMerge = check ? totalTonnage(world) : 0;
        systemMergeFleets(world, context);
        if (check && totalTonnage(world) != beforeMerge) {
            std::printf("НАРУШЕНИЕ t=%lld: слияние изменило тоннаж (%u -> %u)\n",
                        static_cast<long long>(tick), beforeMerge, totalTonnage(world));
            ++violations;
        }

        systemDisbandEmpty(world, context);
        systemApplyCommands(world, context);
        if (check && violations > 8) {
            std::printf("Слишком много нарушений, останавливаюсь.\n");
            return 1;
        }

        // --- отчёт ---
        if ((tick + 1) % reportEvery == 0) {
            std::vector<Stats> stats(bots.size());
            std::fill(owners.begin(), owners.end(), kNoEmpire);
            world.each<StarSystem, Owner>([&](Entity, StarSystem& system, Owner& owner) {
                if (owner.empire < stats.size()) ++stats[owner.empire].systems;
                if (system.index < count) owners[system.index] = owner.empire;
            });
            for (size_t i = 0; i < stats.size(); ++i) {
                peakSystems[i] = std::max(peakSystems[i], stats[i].systems);
            }
            world.each<Planet, PlanetDevelopment>(
                [&](Entity, Planet& planet, PlanetDevelopment& d) {
                    if (planet.system >= count) return;
                    const uint32_t who = owners[planet.system];
                    if (who >= stats.size()) return;
                    ++stats[who].planets;
                    for (uint8_t slot = 0; slot < kMaxSlots; ++slot) {
                        if (d.buildings[slot] != uint8_t(Building::None)) ++stats[who].buildings;
                    }
                });
            world.each<Fleet, Owner>([&](Entity, Fleet& fleet, Owner& o) {
                if (o.empire >= stats.size()) return;
                stats[o.empire].tonnage += fleetTonnage(fleet);
                ++stats[o.empire].fleets;
            });

            uint32_t fighting = 0;
            world.each<BattleState>([&](Entity, BattleState& battle) {
                if (battle.cooldown > 0) ++fighting;
            });
            battlesSeen += fighting;

            const int64_t minutes = (tick + 1) / (60 * kTicksPerSecond);
            std::printf("== %lld ч %02lld мин ==   идёт сражений: %u\n",
                        static_cast<long long>(minutes / 60),
                        static_cast<long long>(minutes % 60), fighting);
            for (size_t i = 0; i < bots.size(); ++i) {
                const Empire* empire = world.get<Empire>(bots[i].entity);
                std::printf("   %-11s систем %3u  планет %3u  зданий %3u  "
                            "флот %5u т в %2u отрядах  сплавы %6lld  энергия %6lld\n",
                            bots[i].name, stats[i].systems, stats[i].planets,
                            stats[i].buildings, stats[i].tonnage, stats[i].fleets,
                            static_cast<long long>(empire->alloys.floorToInt()),
                            static_cast<long long>(empire->energy.floorToInt()));
            }
            std::printf("\n");
        }
    }

    std::printf("Итог: хеш мира %016llX\n",
                static_cast<unsigned long long>(world.hash()));
    std::printf("      сущностей %u, тиков %lld\n",
                world.liveCount(), static_cast<long long>(totalTicks));

    if (!check) return 0;

    // Итоговые проверки. Смысл у них один: прогон обязан оставаться
    // ОСМЫСЛЕННЫМ. Вырожденный сезон — где никто ни с кем не воевал или где
    // империя выбыла на пятнадцатой минуте и больше ничего не делала —
    // ничего не проверяет, но выглядит как успешный запуск.
    std::vector<uint32_t> finalSystems(bots.size(), 0);
    world.each<StarSystem, Owner>([&](Entity, StarSystem&, Owner& owner) {
        if (owner.empire < finalSystems.size()) ++finalSystems[owner.empire];
    });

    for (size_t i = 0; i < bots.size(); ++i) {
        if (peakSystems[i] < 2) {
            std::printf("НАРУШЕНИЕ: %s ни разу не вышли за пределы одной системы\n",
                        bots[i].name);
            ++violations;
        }
    }
    if (battlesSeen == 0) {
        std::printf("НАРУШЕНИЕ: за весь сезон не случилось ни одного сражения\n");
        ++violations;
    }

    if (violations > 0) {
        std::printf("\nНарушений инвариантов: %u\n", violations);
        return 1;
    }
    std::printf("\nИнварианты соблюдены: тоннаж сходится, сезон не выродился.\n");
    return 0;
}
