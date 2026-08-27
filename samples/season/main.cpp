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
#include <set>
#include <string>
#include <vector>

#include "pw/sim/battle_system.h"
#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/production.h"
#include "pw/sim/season.h"

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
    // Инфраструктура обязана попадать в прогон. Она не производит ресурсов,
    // и бот, застраивающий планеты одними шахтами, оставил бы пять зданий
    // из двенадцати непроверенными на живой экономике и живой осаде.
    Building::SupplyDepot, Building::Garrison,
    Building::Drydock, Building::ShieldGenerator,
    Building::Laboratory, Building::Fortress,
    Building::Habitat,
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
        bool same = true;
        for (size_t hull = 0; hull < kHullClasses && same; ++hull) {
            same = left.ships[hull] == was.fleet.ships[hull];
        }
        if (same) continue;

        std::string before, after;
        for (size_t hull = 0; hull < kHullClasses; ++hull) {
            if (hull > 0) { before += '/'; after += '/'; }
            before += std::to_string(was.fleet.ships[hull]);
            after += std::to_string(left.ships[hull]);
        }
        std::printf("[бой t=%lld] система %u, империя %u: %s -> %s\n",
                    static_cast<long long>(tick), was.system, was.empire,
                    before.c_str(), after.c_str());
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
    /// Верфи и заказы в очереди.
    ///
    /// Появились после прогона, где четыре бота за три часа накопили
    /// по шестьдесят тысяч сплавов и не построили НИ ОДНОГО корабля.
    /// По отчёту было видно только следствие («флот 14 т»), а причина —
    /// есть ли вообще верфь и стоит ли заказ в очереди — не показывалась
    /// нигде, и её пришлось искать чтением кода.
    uint32_t shipyards = 0;
    uint32_t queued = 0;
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

/// Разнести стартовые системы по ГРАФУ, а не по номерам.
///
/// Номера систем идут вдоль спиральных рукавов, и «каждая четвёртая
/// система» превращалась в пригоршню соседей. Здесь тот же жадный выбор,
/// что и у сервера: каждая следующая столица — та, что дальше всех
/// от уже занятых.
std::vector<uint32_t> pickHomes(const Galaxy& galaxy, size_t players) {
    constexpr uint8_t kMinHomePlanets = 3;
    const uint32_t count = galaxy.systemCount();

    std::vector<uint32_t> homes;
    std::vector<bool> taken(count, false);

    for (uint32_t index = 0; index < count && homes.empty(); ++index) {
        if (galaxy.planetCount(index) < kMinHomePlanets) continue;
        homes.push_back(index);
        taken[index] = true;
    }
    if (homes.empty()) {
        homes.push_back(0);
        taken[0] = true;
    }

    while (homes.size() < players) {
        uint32_t best = count;
        int32_t bestDistance = -1;
        for (int pass = 0; pass < 2 && best == count; ++pass) {
            for (uint32_t candidate = 0; candidate < count; ++candidate) {
                if (taken[candidate]) continue;
                if (pass == 0 && galaxy.planetCount(candidate) < kMinHomePlanets) continue;

                int32_t nearest = 1 << 20;
                for (uint32_t home : homes) {
                    const int32_t hops = galaxy.hopDistance(candidate, home);
                    if (hops >= 0) nearest = std::min(nearest, hops);
                }
                if (nearest <= bestDistance) continue;
                bestDistance = nearest;
                best = candidate;
            }
        }
        if (best == count) break;
        homes.push_back(best);
        taken[best] = true;
    }
    while (homes.size() < players) homes.push_back(0);
    return homes;
}

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
    Season season;

    registerGalaxyComponents(world);
    registerFleetComponents(world);
    registerControlComponents(world);
    registerEconomyComponents(world);
    registerProductionComponents(world);
    registerBattleComponents(world);
    registerSeasonComponents(world);

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

    // Сезон растягивается под длину прогона. Прогон на три часа обязан
    // увидеть ВСЕ ЧЕТЫРЕ стадии, иначе Кризис и Финал не проверяются
    // вовсе — а именно они меняют правила сильнее всего.
    season.config.stretchTo(int64_t(hours * 3600));
    world.setResource(&season);

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
    //
    // По ГРАФУ, а не по номеру. Номера идут вдоль спиральных рукавов, и
    // «каждая четвёртая система» оказывалась пригоршней соседей: две
    // империи теряли флоты в первые полчаса и до конца прогона сидели
    // по одной системе. Заодно требуем от столицы трёх планет — старт
    // на одиноком мире это проигрыш до первого хода.
    const uint32_t count = galaxy.systemCount();
    const std::vector<uint32_t> homes = pickHomes(galaxy, bots.size());
    for (size_t i = 0; i < bots.size(); ++i) {
        bots[i].home = homes[i];
        bots[i].entity = world.create();
        world.add<Empire>(bots[i].entity,
                          Empire{fx::fromInt(500), fx::fromInt(200), fx::fromInt(300),
                                 fx::zero(), fx::zero(), bots[i].empire, bots[i].home});
        // Вооружение империи: его унаследуют все построенные корабли.
        world.add<FleetArmament>(bots[i].entity, bots[i].doctrine);
        // Столица — это ПЛАНЕТЫ родной системы, все до одной: владение
        // теперь лежит на них, и запись в системе сама по себе пуста.
        world.get<Owner>(galaxy.systemEntity(bots[i].home))->empire = bots[i].empire;
        for (uint32_t orbit = 0; orbit < galaxy.planetCount(bots[i].home); ++orbit) {
            const Entity planet = galaxy.planetEntity(bots[i].home, orbit);
            if (!planet.valid()) continue;
            world.get<Owner>(planet)->empire = bots[i].empire;
            world.get<PlanetDefense>(planet)->readiness = kReadinessMax;
        }

        // Стартовый флот.
        const Entity fleet = world.create();
        world.add<Fleet>(fleet, makeFleet({{Hull::Corvette, 8}, {Hull::Destroyer, 2}}));
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
    std::vector<uint32_t> unclaimed(count, 0);
    // Сражения считаются по СКАЧКУ перезарядки, каждый тик.
    //
    // Прежняя версия смотрела, сколько систем дерётся В МОМЕНТ ОТЧЁТА, то
    // есть раз в двадцать минут. Бой длится секунды, и такая выборка почти
    // всегда показывала ноль: прогон уверенно докладывал «за весь сезон
    // не случилось ни одного сражения» ровно тогда, когда флоты теряли
    // половину тоннажа в боях между отчётами.
    std::vector<uint32_t> battleCooldown(count, 0);
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

            // Что у бота болит прямо сейчас. Считается ОДИН РАЗ на цикл
            // и до обхода планет: иначе каждый бот пересчитывал бы это
            // на каждой своей планете.
            // Верфи считаются ПОШТУЧНО НА СИСТЕМУ, а не «есть или нет».
            //
            // Одна верфь осваивает 0.5 сплава в секунду, а развитая система
            // даёт около 0.75 — то есть одной верфи системе не хватает
            // по построению (см. kBuildRateShipyard). Бот, строивший одну,
            // копил сплавы и не воевал: к третьему часу на счетах лежало
            // по пятьдесят тысяч. Это было видно в отчёте как «сплавы 53666»
            // рядом с «флот 60 т», и это не жадность бота, а арифметика.
            std::vector<std::vector<uint32_t>> yardsBySystem(
                bots.size(), std::vector<uint32_t>(count, 0));
            std::vector<bool> starving(bots.size(), false);
            std::vector<bool> blackout(bots.size(), false);
            world.each<Planet, Owner, PlanetDevelopment, PlanetConstruction>(
                [&](Entity, Planet& planet, Owner& o, PlanetDevelopment& d,
                    PlanetConstruction& site) {
                    if (o.empire >= bots.size() || planet.system >= count) return;
                    yardsBySystem[o.empire][planet.system] +=
                        countBuildings(planet, d, Building::Shipyard);
                    // Верфь, которая СТРОИТСЯ, уже занята: без этого бот
                    // каждый цикл видел «верфей ноль» — она ведь ещё
                    // не достроена — и заказывал ещё одну. За два часа
                    // выходило по четыре верфи на систему вместо двух.
                    if (site.slot != PlanetConstruction::kNoSlot &&
                        site.building == uint8_t(Building::Shipyard)) {
                        ++yardsBySystem[o.empire][planet.system];
                    }
                });
            // Учётная книга заполняется системой экономики, а политика ботов
            // идёт ДО неё: на первом тике книга ещё пуста. Спрашивать
            // у пустой книги — это чтение за концом вектора и падение
            // на первом же запуске.
            for (size_t i = 0; i < bots.size() && uint32_t(i) < ledger.size(); ++i) {
                const Ledger::Flow& flow = ledger.at(uint32_t(i));
                blackout[i] = flow.energy < fx::zero();
                starving[i] = flow.foundryIdle > fx::zero();
            }

            for (const Bot& bot : bots) {
                // Сколько в каждой системе планет, ещё не принадлежащих
                // ЭТОМУ боту. Это и есть «есть ли здесь что захватывать»:
                // система-владелец больше не отвечает на этот вопрос,
                // потому что половина её планет может быть чужой.
                std::fill(unclaimed.begin(), unclaimed.end(), 0);
                world.each<Planet, Owner>([&](Entity, Planet& planet, Owner& planetOwner) {
                    if (planet.system >= count) return;
                    if (planetOwner.empire == bot.empire) return;
                    ++unclaimed[planet.system];
                });

                Empire* empire = world.get<Empire>(bot.entity);

                // 1. Заказать стройку на СВОИХ планетах.
                //
                // Раньше бот вписывал здание прямо в застройку, и оно
                // появлялось мгновенно. Теперь он ставит заказ, как игрок,
                // и ждёт: иначе прогон сезона проверял бы экономику, которой
                // в игре нет.
                // ЧТО СТРОИТЬ — РЕШАЕТСЯ ПО НУЖДЕ, А НЕ ПО НОМЕРУ СЛОТА.
                //
                // Прежняя версия брала здание из списка по индексу слота:
                // kBuildOrder[slot]. Верфь стояла шестой, а планеты в среднем
                // застраивались на четыре слота — и до верфи бот не доходил
                // НИКОГДА. Прогон показал ровно это: три империи из четырёх
                // с нулём верфей, шестьдесят тысяч нетронутых сплавов
                // и ни одного построенного корабля за три часа. Военная
                // половина игры не проверялась вовсе, и заметно это стало
                // только когда в отчёт добавили колонку «верфей».
                //
                // Теперь бот сначала закрывает дыры: нет верфи в системе —
                // строит верфь, энергия в минусе — электростанцию, заводы
                // простаивают без сырья — шахту. И только потом идёт
                // по списку приоритетов.
                constexpr uint32_t kYardsPerSystem = 2;
                world.each<Planet, Owner, PlanetDevelopment, PlanetConstruction>(
                    [&](Entity, Planet& planet, Owner& planetOwner,
                        PlanetDevelopment& development, PlanetConstruction& site) {
                        if (planet.system >= count) return;
                        if (planetOwner.empire != bot.empire) return;
                        if (site.slot != PlanetConstruction::kNoSlot) return;  // уже строит

                        const uint8_t limit = usableSlots(planet, development);
                        uint8_t free = kMaxSlots;
                        uint8_t filled = 0;
                        for (uint8_t slot = 0; slot < limit; ++slot) {
                            if (development.buildings[slot] == uint8_t(Building::None)) {
                                if (free == kMaxSlots) free = slot;
                            } else {
                                ++filled;
                            }
                        }
                        if (free == kMaxSlots) return;  // места нет

                        Building wanted = kBuildOrder[filled % (sizeof(kBuildOrder) /
                                                                sizeof(kBuildOrder[0]))];
                        const uint32_t yardsHere = yardsBySystem[bot.empire][planet.system];
                        if (yardsHere < kYardsPerSystem && filled >= 2) {
                            wanted = Building::Shipyard;
                        } else if (starving[bot.empire] && filled >= 1) {
                            wanted = Building::Mine;
                        } else if (blackout[bot.empire] && filled >= 1) {
                            wanted = Building::PowerPlant;
                        }
                        if (wanted == Building::Shipyard) {
                            // Засчитываем заказ СРАЗУ: иначе все планеты
                            // системы в одном цикле увидят «верфей мало»
                            // и закажут по верфи каждая.
                            ++yardsBySystem[bot.empire][planet.system];
                        }
                        enqueueConstruction(site, free, wanted);
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
                    // Заказываем ПАРТИЮ, а не по одному кораблю.
                    //
                    // По одному верфь простаивала между циклами политики,
                    // и к третьему часу на счетах лежало по пятьдесят тысяч
                    // сплавов — империя богатела и не воевала. Размер партии
                    // считается от казны: богатый строит крупнее, бедный
                    // по одному, и никто не заказывает больше, чем сможет
                    // оплатить.
                    const uint32_t batch = std::clamp(
                        uint32_t(empire->alloys.floorToInt() / int64_t(affordable * 4)),
                        1u, 8u);
                    world.each<StarSystem, Owner, BuildQueue>(
                        [&](Entity, StarSystem&, Owner& o, BuildQueue& queue) {
                            if (o.empire != bot.empire) return;
                            if (queue.remaining > 0) return;
                            enqueueBuild(queue, hull, batch);
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
                        // Пока в системе осталась хоть одна не своя планета,
                        // флот стоит и доделывает работу. Раньше признаком
                        // был владелец системы; с переносом владения на
                        // планеты флот улетал, взяв ОДНУ планету из четырёх,
                        // и половина системы оставалась чужой навсегда.
                        if (unclaimed[location.system] > 0) return;

                        int32_t best = -1;
                        int32_t bestHops = 1 << 20;
                        int32_t enemy = -1;
                        int32_t enemyHops = 1 << 20;
                        for (uint32_t target = 0; target < count; ++target) {
                            if (unclaimed[target] == 0) continue;  // там всё уже наше
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
        //
        // Производное владение системами — первым: и бой, и присутствие,
        // и поиск цели ботом смотрят на владельца системы и обязаны видеть
        // в одном тике одно и то же.
        systemSeason(world, context);
        if (season.stage != season.previousStage) {
            std::printf("[сезон t=%lld] стадия: %s\n", static_cast<long long>(tick),
                        stageName(season.stage));
        }
        systemControlRollup(world, context);
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

        // Перезарядка взлетает в момент боя и дальше только убывает,
        // поэтому скачок вверх — надёжный признак свежего сражения.
        world.each<StarSystem, BattleState>([&](Entity, StarSystem& system,
                                                BattleState& battle) {
            if (system.index >= count) return;
            if (battle.cooldown > battleCooldown[system.index]) ++battlesSeen;
            battleCooldown[system.index] = battle.cooldown;
        });

        if (check && totalTonnage(world) > beforeBattle) {
            std::printf("НАРУШЕНИЕ t=%lld: бой ДОБАВИЛ тоннаж (%u -> %u)\n",
                        static_cast<long long>(tick), beforeBattle, totalTonnage(world));
            ++violations;
        }

        systemPresence(world, context);
        systemSiege(world, context);
        systemEconomy(world, context);
        planetDefenceCap(world, context);
        planetConstructionTick(world, context);
        systemProduction(world, context);

        // Слияние не имеет права ни терять, ни удваивать корабли: оно только
        // переносит их из отряда в отряд.
        //
        // Срез берётся вплотную вокруг systemMergeFleets. Дальше в тике
        // применяется буфер команд, а там появляются построенные корабли —
        // и более широкая рамка ловила бы производство, а не слияние.
        const uint32_t beforeMerge = check ? totalTonnage(world) : 0;
        systemCrisis(world, context);
        systemPrestige(world, context);
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
            // Планеты считаются по СВОЕМУ владельцу, а не по владельцу
            // системы: наполовину взятая система даёт по половине каждому,
            // и это именно то, что теперь надо видеть в отчёте.
            world.each<Planet, Owner, PlanetDevelopment>(
                [&](Entity, Planet& planet, Owner& planetOwner, PlanetDevelopment& d) {
                    if (planet.system >= count) return;
                    const uint32_t who = planetOwner.empire;
                    if (who >= stats.size()) return;
                    ++stats[who].planets;
                    for (uint8_t slot = 0; slot < kMaxSlots; ++slot) {
                        if (d.buildings[slot] != uint8_t(Building::None)) ++stats[who].buildings;
                    }
                    stats[who].shipyards += countBuildings(planet, d, Building::Shipyard);
                });
            world.each<StarSystem, Owner, BuildQueue>(
                [&](Entity, StarSystem&, Owner& o, BuildQueue& queue) {
                    if (o.empire >= stats.size()) return;
                    if (queue.remaining > 0) ++stats[o.empire].queued;
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


            const int64_t minutes = (tick + 1) / (60 * kTicksPerSecond);
            std::printf("== %lld ч %02lld мин ==   идёт сражений: %u\n",
                        static_cast<long long>(minutes / 60),
                        static_cast<long long>(minutes % 60), fighting);
            for (size_t i = 0; i < bots.size(); ++i) {
                const Empire* empire = world.get<Empire>(bots[i].entity);
                std::printf("   %-11s систем %3u  планет %3u  зданий %3u  верфей %2u  "
                            "заказов %2u  флот %5u т в %2u отрядах  "
                            "сплавы %6lld  энергия %6lld\n",
                            bots[i].name, stats[i].systems, stats[i].planets,
                            stats[i].buildings, stats[i].shipyards, stats[i].queued,
                            stats[i].tonnage, stats[i].fleets,
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
