#include "pw/sim/economy.h"

#include <algorithm>
#include <vector>

#include "pw/sim/control.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

namespace pw::sim {

void registerEconomyComponents(World& world) {
    world.registerComponent<PlanetDevelopment>("PlanetDevelopment");
}

void initialiseEconomy(World& world) {
    // Собираем сущности заранее: навешивание компонента переносит сущность
    // между таблицами, а менять мир прямо в обходе нельзя.
    std::vector<Entity> planets;
    world.each<Planet>([&](Entity entity, Planet&) { planets.push_back(entity); });
    for (const Entity planet : planets) {
        world.add<PlanetDevelopment>(planet, PlanetDevelopment{});
    }
}

void Ledger::reset(uint32_t empires) {
    flows_.assign(empires, Flow{});
}

// ---------------------------------------------------------------------------
// Справочник
// ---------------------------------------------------------------------------

bool matchesSpecialisation(Building building, Specialization specialisation) {
    switch (specialisation) {
        case Specialization::Mining:     return building == Building::Mine;
        case Specialization::Industrial: return building == Building::Foundry;
        case Specialization::Research:   return building == Building::Laboratory;
        case Specialization::Trade:      return building == Building::TradeHub;
        case Specialization::Fortress:   return building == Building::Fortress;
        case Specialization::Shipyard:   return building == Building::Shipyard;
        default:                         return false;
    }
}

bool matchesClass(Building building, uint8_t planetClass) {
    const auto klass = PlanetClass(planetClass);
    switch (building) {
        case Building::Mine:
            // Астероиды и вулканические миры — сырьё лежит на поверхности.
            return klass == PlanetClass::AsteroidBelt || klass == PlanetClass::Volcanic;
        case Building::PowerPlant:
            // Газовый гигант — топливо и перепад давлений.
            return klass == PlanetClass::GasGiant;
        case Building::Laboratory:
        case Building::TradeHub:
            // Океанические миры пригодны для жизни, туда едут люди.
            return klass == PlanetClass::Ocean;
        case Building::Foundry:
            return klass == PlanetClass::Desert || klass == PlanetClass::Barren;
        default:
            return false;
    }
}

uint32_t buildingCost(Building building) {
    switch (building) {
        case Building::Mine:       return 60;
        case Building::PowerPlant: return 60;
        case Building::Foundry:    return 90;
        case Building::Laboratory: return 90;
        case Building::TradeHub:   return 90;
        // Крепость и верфь дороже: это не производство, а право — держать
        // планету и строить флот. Право обязано стоить времени.
        case Building::Fortress:   return 150;
        case Building::Shipyard:   return 200;
        // Инфраструктура. Дороже производственных зданий намеренно: она
        // окупается не доходом, а тем, чего не случилось, и такую покупку
        // человек откладывает до последнего. Высокая цена делает решение
        // «строю снабжение вместо ещё одной шахты» осознанным, а не
        // машинальным — а именно осознанность здесь и нужна.
        case Building::SupplyDepot:     return 180;
        case Building::ShieldGenerator: return 220;
        case Building::Drydock:         return 260;
        // Хабитат дороже всего: он торгует за самый дефицитный ресурс
        // карты — за место. Дешёвый хабитат обесценил бы класс планеты,
        // а за просторные миры в этой игре воюют.
        case Building::Habitat:         return 320;
        case Building::Garrison:        return 120;
        default:                        return 0;
    }
}

uint8_t countBuildings(const Planet& planet, const PlanetDevelopment& development,
                       Building building) {
    // Обходим ВЕСЬ массив слотов, а не первые planet.slots: доступное
    // число слотов само зависит от числа хабитатов, и считать его здесь
    // значило бы получить рекурсию с неочевидным дном.
    (void)planet;
    uint8_t count = 0;
    for (uint8_t slot = 0; slot < kMaxSlots; ++slot) {
        if (development.buildings[slot] == uint8_t(building)) ++count;
    }
    return count;
}

uint8_t usableSlots(const Planet& planet, const PlanetDevelopment& development) {
    const int base = std::min<int>(planet.slots, kMaxSlots);
    const int extra = int(countBuildings(planet, development, Building::Habitat)) *
                      int(kHabitatSlots);
    return uint8_t(std::min(base + extra, kMaxSlots));
}

bool isInfrastructure(Building building) {
    switch (building) {
        case Building::SupplyDepot:
        case Building::ShieldGenerator:
        case Building::Drydock:
        case Building::Habitat:
        case Building::Garrison:
            return true;
        default:
            return false;
    }
}

std::vector<uint32_t> countBuildingsPerSystem(World& world, Building building,
                                             uint32_t systemCount) {
    std::vector<uint32_t> counts(systemCount, 0);
    world.each<Planet, PlanetDevelopment>(
        [&](Entity, Planet& planet, PlanetDevelopment& development) {
            if (planet.system >= counts.size()) return;
            const uint8_t limit = std::min<uint8_t>(planet.slots, kMaxSlots);
            for (uint8_t slot = 0; slot < limit; ++slot) {
                if (development.buildings[slot] == uint8_t(building)) {
                    ++counts[planet.system];
                }
            }
        });
    return counts;
}

namespace {

/// Множитель выработки: специализация и класс складываются мультипликативно.
/// Профильное здание на профильной планете даёт почти вдвое — этого хватает,
/// чтобы специализироваться было выгодно, но непрофильное строительство
/// оставалось осмысленным.
fx outputScale(Building building, const Planet& planet) {
    fx scale = fx::one();
    if (matchesSpecialisation(building, Specialization(planet.specialization))) {
        scale *= kSpecialisationBonus;
    }
    if (matchesClass(building, planet.planetClass)) {
        scale *= kClassAffinityBonus;
    }
    return scale;
}

/// Сколько места в учётной книге. Империи нумеруются с нуля и их немного.
uint32_t ledgerSize(World& world) {
    uint32_t highest = 0;
    world.each<Empire>([&](Entity, Empire& empire) {
        if (empire.id != kNoEmpire && empire.id + 1 > highest) highest = empire.id + 1;
    });
    return std::min(highest, 4096u);
}

}  // namespace

// ---------------------------------------------------------------------------
// Экономика
// ---------------------------------------------------------------------------

void systemEconomy(World& world, const TickContext& context) {
    Ledger* ledger = world.resource<Ledger>();
    const Galaxy* galaxy = world.resource<Galaxy>();
    if (ledger == nullptr || galaxy == nullptr) return;

    const uint32_t empires = ledgerSize(world);
    ledger->reset(empires);
    if (empires == 0) return;

    // --- проход 1: всё, кроме заводов ---
    //
    // Заводы откладываем: им нужны минералы, а сколько их будет, известно
    // только после того, как отработают все шахты империи.
    std::vector<uint32_t> foundries(empires, 0);
    // Узлы снабжения и число планет — для содержания флота. Оба считаются
    // здесь же, одним обходом: отдельный проход по планетам ради двух
    // счётчиков был бы вторым источником правды о том, чем владеет империя.
    std::vector<uint32_t> depots(empires, 0);
    std::vector<uint32_t> planets(empires, 0);

    // Выработка достаётся владельцу САМОЙ ПЛАНЕТЫ, а не её системы.
    //
    // Раньше считался владелец системы, и это было ровно то, от чего игра
    // ушла: захват системы отдавал всё её содержимое одним щелчком. Теперь
    // удержанная в чужом тылу планета продолжает кормить своего хозяина —
    // и осада её соседки не отбирает у него ни единицы дохода.
    world.each<Planet, PlanetDevelopment, Owner>(
        [&](Entity, Planet& planet, PlanetDevelopment& development, Owner& planetOwner) {
            const uint32_t empire = planetOwner.empire;
            if (empire == kNoEmpire || empire >= empires) return;

            Ledger::Flow& flow = ledger->at(empire);
            ++planets[empire];
            // Слоты СЧИТАЮТСЯ, а не берутся из планеты: хабитаты добавляют
            // места, и застройка в них обязана работать.
            const uint8_t limit = usableSlots(planet, development);

            for (uint8_t slot = 0; slot < limit; ++slot) {
                const auto building = Building(development.buildings[slot]);
                if (building == Building::None) continue;

                flow.energy -= kUpkeepBuilding;
                const fx scale = outputScale(building, planet);

                switch (building) {
                    case Building::Mine:
                        flow.minerals += kOutputMine * scale;
                        break;
                    case Building::PowerPlant:
                        flow.energy += kOutputPower * scale;
                        break;
                    case Building::Laboratory:
                        flow.research += kOutputLab * scale;
                        break;
                    case Building::TradeHub:
                        flow.energy += kOutputTradeEnergy * scale;
                        flow.influence += kOutputTradeInfluence * scale;
                        break;
                    case Building::Foundry:
                        ++foundries[empire];
                        break;
                    case Building::SupplyDepot:
                        ++depots[empire];
                        break;
                    default:
                        // Крепость, верфь, щит, док, хабитат и гарнизон
                        // ресурсов не производят: они покупают устойчивость,
                        // а платят за неё содержанием, как и все остальные.
                        break;
                }
            }
        });

    // --- содержание флота ---
    //
    // ЗДЕСЬ ЖИВЁТ ГЛАВНЫЙ ТОРМОЗ СНЕЖНОГО КОМА. Тонна флота у империи
    // из сорока планет обходится дороже, чем у империи из четырёх, и это
    // не штраф за успех, а логистика: чем дальше растянут фронт, тем
    // дороже держать на нём корабли.
    //
    // Почему именно так, а не «лимит планет»: лимит игрок читает как
    // запрет и обижается на него; растущее содержание он читает как задачу
    // и решает её — узлами снабжения, специализацией, отказом от лишнего
    // флота. Первое отнимает решение, второе его создаёт.
    //
    // Без тормоза сервер на тысячу игроков вырождается за неделю: первый,
    // кто вырвался вперёд, растёт быстрее всех просто потому, что уже
    // впереди, и к третьей неделе играть остаётся некому.
    std::vector<fx> upkeepScale(empires, fx::one());
    for (uint32_t empire = 0; empire < empires; ++empire) {
        const uint32_t owned = planets[empire];
        fx scale = fx::one();
        if (owned > kUpkeepFreePlanets) {
            scale += kUpkeepGrowthPerPlanet * fx::fromInt(int64_t(owned - kUpkeepFreePlanets));
        }
        // Узлы снабжения — единственный способ его ослабить. То есть рост
        // оплачивается инфраструктурой, а не запрещается.
        if (depots[empire] > 0) {
            fx relief = kDepotUpkeepRelief * fx::fromInt(int64_t(depots[empire]));
            relief = min(relief, kDepotReliefCap);
            scale *= (fx::one() - relief);
        }
        upkeepScale[empire] = max(fx::fromFraction(1, 4), scale);
    }

    world.each<Fleet, Owner>([&](Entity, Fleet& fleet, Owner& owner) {
        if (owner.empire == kNoEmpire || owner.empire >= empires) return;
        ledger->at(owner.empire).energy -= kUpkeepTonnage *
                                           fx::fromInt(fleetTonnage(fleet)) *
                                           upkeepScale[owner.empire];
    });

    // --- проход 2: заводы ---
    //
    // Цепочка работает здесь: завод плавит сплавы из минералов и встаёт,
    // если минералов нет. Индустриальный мир без шахтёрского бесполезен —
    // ровно то, ради чего цепочка и вводилась.
    world.each<Empire>([&](Entity, Empire& empire) {
        if (empire.id == kNoEmpire || empire.id >= empires) return;
        Ledger::Flow& flow = ledger->at(empire.id);
        const uint32_t count = foundries[empire.id];
        if (count == 0) return;

        const fx wanted = kFoundryMineralCost * fx::fromInt(count) * context.delta;
        const fx available = max(fx::zero(), empire.minerals + flow.minerals * context.delta);

        if (available >= wanted) {
            flow.minerals -= kFoundryMineralCost * fx::fromInt(count);
            flow.alloys += kOutputFoundry * fx::fromInt(count);
            return;
        }

        // Минералов на всех не хватило — работает столько заводов, на
        // сколько хватило сырья. Остальные простаивают, и это видно игроку.
        const fx share = wanted > fx::zero() ? available / wanted : fx::zero();
        flow.minerals -= kFoundryMineralCost * fx::fromInt(count) * share;
        flow.alloys += kOutputFoundry * fx::fromInt(count) * share;
        flow.foundryIdle = kOutputFoundry * fx::fromInt(count) * (fx::one() - share);
    });

    // --- применение ---
    //
    // Обход империй идёт по индексу сущности, порядок стабилен.
    world.each<Empire>([&](Entity, Empire& empire) {
        if (empire.id == kNoEmpire || empire.id >= empires) return;
        const Ledger::Flow& flow = ledger->at(empire.id);

        // Ресурсы не уходят в минус. Дефицит энергии — это сигнал, а не долг:
        // отключения и штрафы появятся отдельной механикой на Фазе 2.
        empire.energy = max(fx::zero(), empire.energy + flow.energy * context.delta);
        empire.minerals = max(fx::zero(), empire.minerals + flow.minerals * context.delta);
        empire.alloys = max(fx::zero(), empire.alloys + flow.alloys * context.delta);
        empire.research = max(fx::zero(), empire.research + flow.research * context.delta);
        empire.influence = max(fx::zero(), empire.influence + flow.influence * context.delta);
    });
}

// ---------------------------------------------------------------------------
// Крепости поднимают потолок обороны
// ---------------------------------------------------------------------------

void planetDefenceCap(World& world, const TickContext&) {
    // Крепость поднимает потолок ТОЙ ПЛАНЕТЫ, на которой стоит, а не всей
    // системы. Раньше считалось по системе, и крепость на пустом камне
    // укрепляла соседний мир-столицу — оборона строилась там, где дешевле,
    // а не там, где важно. Теперь укрепление — это выбор конкретной планеты.
    world.each<Planet, PlanetDefense>(
        [&](Entity entity, Planet& planet, PlanetDefense& defense) {
            uint32_t fortresses = 0;
            if (const PlanetDevelopment* development = world.get<PlanetDevelopment>(entity)) {
                fortresses = countBuildings(planet, *development, Building::Fortress);
            }

            const fx cap = kReadinessMax + kFortressReadiness * fx::fromInt(fortresses);
            defense.maxReadiness = cap;
            // Готовность выше нового потолка обрезается: снесли крепость —
            // оборона просела сразу, а не осталась висеть.
            if (defense.readiness > cap) defense.readiness = cap;
        });
}

}  // namespace pw::sim
