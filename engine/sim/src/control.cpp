#include "pw/sim/control.h"

#include <algorithm>

#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/production.h"

namespace pw::sim {

void registerControlComponents(World& world) {
    world.registerComponent<PlanetDefense>("PlanetDefense");
    world.registerComponent<SiegeState>("SiegeState");
}

void initialiseControl(World& world, const Galaxy& galaxy) {
    // Владение — на планетах. Оборона и осада тоже: осаждают конкретное
    // небесное тело, а не абстрактную точку графа.
    //
    // Обход идёт по указателю галактики, а не по миру: навешивание
    // компонента переносит сущность между таблицами, и менять мир прямо
    // в его обходе нельзя — строки поедут под ногами.
    for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
        for (uint32_t orbit = 0; orbit < galaxy.planetCount(index); ++orbit) {
            const Entity planet = galaxy.planetEntity(index, orbit);
            if (!planet.valid()) continue;
            world.add<Owner>(planet, Owner{kNoEmpire, 0});
            // У ничьей планеты оборонять нечего: готовность нулевая,
            // потолок есть.
            world.add<PlanetDefense>(planet, PlanetDefense{fx::zero(), kReadinessMax});
            world.add<SiegeState>(planet, SiegeState{kNoEmpire, 0});
        }
    }

    // У системы владелец ПРОИЗВОДНЫЙ: пересчитывается каждый тик по
    // планетам. Компонент всё равно нужен — на него смотрят карта, поиск
    // пути, присутствие флотов и бой.
    for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
        world.add<Owner>(galaxy.systemEntity(index), Owner{kNoEmpire, 0});
    }
}

// ---------------------------------------------------------------------------
// Производное владение системами
// ---------------------------------------------------------------------------

void systemControlRollup(World& world, const TickContext&) {
    const Galaxy* galaxy = world.resource<Galaxy>();
    if (galaxy == nullptr) return;
    const uint32_t systemCount = galaxy->systemCount();
    if (systemCount == 0) return;

    // Считаем планеты по империям в каждой системе.
    //
    // Плоский массив вместо словаря: империй в сезоне единицы, а систем
    // сотни тысяч. Словарь на систему стоил бы выделения памяти на каждый
    // тик и не пережил бы безграничную галактику.
    constexpr uint32_t kMaxEmpires = 32;
    std::vector<uint16_t> counts(size_t(systemCount) * kMaxEmpires, 0);

    world.each<Planet, Owner>([&](Entity, Planet& planet, Owner& owner) {
        if (owner.empire == kNoEmpire || owner.empire >= kMaxEmpires) return;
        if (planet.system >= systemCount) return;
        ++counts[size_t(planet.system) * kMaxEmpires + owner.empire];
    });

    world.each<StarSystem, Owner>([&](Entity, StarSystem& system, Owner& owner) {
        if (system.index >= systemCount) return;
        const uint16_t* row = counts.data() + size_t(system.index) * kMaxEmpires;

        uint32_t best = kNoEmpire;
        uint16_t bestCount = 0;
        bool tie = false;
        for (uint32_t empire = 0; empire < kMaxEmpires; ++empire) {
            if (row[empire] == 0) continue;
            if (row[empire] > bestCount) {
                bestCount = row[empire];
                best = empire;
                tie = false;
            } else if (row[empire] == bestCount) {
                tie = true;
            }
        }

        // Ничья — значит система спорная, и владельца у неё нет. Это не
        // мелочь оформления: спорная система не даёт защитникам бонуса
        // обороны и не считается тылом ни для кого.
        owner.empire = (best == kNoEmpire || tie) ? kNoEmpire : best;
    });
}

// ---------------------------------------------------------------------------
// Присутствие
// ---------------------------------------------------------------------------

void Presence::Entry::add(uint32_t empire, uint32_t tonnage) {
    if (tonnage == 0 || empire == kNoEmpire) return;

    for (uint32_t i = 0; i < count; ++i) {
        if (empires[i] != empire) continue;
        tonnages[i] += tonnage;
        return;
    }
    if (count >= kMaxPresent) {
        // Больше восьми империй в одной системе на Фазе 1 невозможно:
        // столько всего мест в сезоне. Молча теряем, а не портим память.
        return;
    }
    empires[count] = empire;
    tonnages[count] = tonnage;
    ++count;
}

uint32_t Presence::Entry::tonnageOf(uint32_t empire) const {
    if (empire == kNoEmpire) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (empires[i] == empire) return tonnages[i];
    }
    return 0;
}

uint32_t Presence::Entry::strongestOther(uint32_t owner, uint32_t& tonnage) const {
    uint32_t best = kNoEmpire;
    tonnage = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (empires[i] == owner) continue;
        // При равенстве побеждает меньший номер империи: порядок обхода
        // не имеет права решать исход.
        const bool stronger = tonnages[i] > tonnage ||
                              (tonnages[i] == tonnage && empires[i] < best);
        if (!stronger) continue;
        tonnage = tonnages[i];
        best = empires[i];
    }
    return best;
}

uint32_t Presence::Entry::othersCount(uint32_t owner) const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (empires[i] != owner) ++total;
    }
    return total;
}

void Presence::rebuild(World& world, uint32_t systemCount) {
    entries_.assign(systemCount, Entry{});

    // Считаются только СТОЯЩИЕ флоты: флот в пути находится между
    // системами и не участвует ни в осаде, ни в обороне. Иначе флот
    // «защищал» бы систему, которую уже покинул.
    //
    // Владельцы систем здесь больше не нужны: кто кому враг, решается
    // на уровне планеты, а не системы.
    world.each<Fleet, FleetLocation, Owner>(
        [&](Entity, Fleet& fleet, FleetLocation& location, Owner& owner) {
            if (location.system != location.nextSystem) return;
            if (location.system >= systemCount) return;
            entries_[location.system].add(owner.empire, fleetTonnage(fleet));
        });
}

void systemPresence(World& world, const TickContext&) {
    Presence* presence = world.resource<Presence>();
    const Galaxy* galaxy = world.resource<Galaxy>();
    if (presence == nullptr || galaxy == nullptr) return;
    presence->rebuild(world, galaxy->systemCount());
}

// ---------------------------------------------------------------------------
// Осада
// ---------------------------------------------------------------------------

namespace {

/// Скорость осады растёт от тоннажа, но с затуханием.
///
/// Втрое больший флот не берёт планету втрое быстрее. Иначе выигрывал бы
/// просто тот, кто собрал всё в один кулак, и разделять силы никогда не имело
/// бы смысла — а вместе с этим исчезли бы и фронт, и рейды.
fx siegeRate(uint32_t tonnage) {
    const uint32_t capped = std::min(tonnage, kSiegeTonnageScale);
    const fx scale = fx::one() + fx::fromFraction(int64_t(capped), kSiegeTonnageScale);
    return kSiegeBaseRate * scale;
}

/// Можно ли сейчас что-то сделать с планетой этого владельца.
///
/// Этим же условием выбирается цель в системе. Отдельного правила «цель —
/// просто самая нижняя орбита» быть не должно: иначе защитник ставил бы
/// один корвет к внутренней планете и делал неуязвимой всю систему — ровно
/// та беда «одного щелчка», от которой мы и уходим.
bool actionable(const Presence::Entry& here, uint32_t owner) {
    uint32_t tonnage = 0;
    if (here.strongestOther(owner, tonnage) == kNoEmpire) return false;

    if (owner == kNoEmpire) {
        // Ничью планету занимает только ЕДИНСТВЕННЫЙ претендент: сошлись
        // двое — сначала разбираются между собой.
        return here.othersCount(kNoEmpire) == 1;
    }

    // Присутствие сил ВЛАДЕЛЬЦА ПЛАНЕТЫ снимает осаду немедленно, поэтому
    // деблокирующий удар работает: пришёл, встал — осада сорвана. Считается
    // владелец планеты, а не системы: держать планету в чужом тылу — законный
    // ход, и флот соседа её не обороняет.
    return here.tonnageOf(owner) == 0;
}

}  // namespace

void systemSiege(World& world, const TickContext& context) {
    const Presence* presence = world.resource<Presence>();
    if (presence == nullptr) return;
    const Galaxy* galaxy = world.resource<Galaxy>();
    if (galaxy == nullptr) return;

    const uint32_t systemCount = galaxy->systemCount();
    if (presence->size() < systemCount) return;

    // Флот работает по ОДНОЙ планете за раз, в порядке орбит.
    //
    // Иначе система с четырьмя планетами бралась бы так же быстро, как
    // с одной, и число планет перестало бы что-либо значить — а это главная
    // валюта игры. Заодно появляется глубина обороны: пока враг грызёт первую
    // орбиту, у защитника есть время привести флот.
    //
    // Цель выбирается ОТДЕЛЬНЫМ проходом по всем планетам, чтобы результат
    // не зависел от порядка сущностей в таблицах.
    std::vector<uint32_t> targetOrbit(systemCount, UINT32_MAX);
    std::vector<Entity> targetEntity(systemCount, kNoEntity);

    world.each<Planet, Owner>([&](Entity entity, Planet& planet, Owner& owner) {
        if (planet.system >= systemCount) return;
        if (!actionable(presence->at(planet.system), owner.empire)) return;
        if (uint32_t(planet.orbit) >= targetOrbit[planet.system]) return;
        targetOrbit[planet.system] = planet.orbit;
        targetEntity[planet.system] = entity;
    });

    world.each<Planet, Owner, PlanetDefense, SiegeState>(
        [&](Entity entity, Planet& planet, Owner& owner, PlanetDefense& defense,
            SiegeState& siege) {
            if (planet.system >= systemCount) return;
            const Presence::Entry& here = presence->at(planet.system);

            // --- не цель: осады нет, оборона отрастает ---
            if (targetEntity[planet.system] != entity) {
                siege.besieger = kNoEmpire;
                siege.ticks = 0;
                if (owner.empire != kNoEmpire && defense.readiness < defense.maxReadiness) {
                    defense.readiness =
                        min(defense.maxReadiness,
                            defense.readiness + kReadinessRegen * context.delta);
                }
                return;
            }

            uint32_t tonnage = 0;
            const uint32_t attacker = here.strongestOther(owner.empire, tonnage);

            if (siege.besieger != attacker) {
                siege.besieger = attacker;  // сменился осаждающий — счёт заново
                siege.ticks = 0;
            }
            ++siege.ticks;

            // --- ничья планета: занятие ---
            if (owner.empire == kNoEmpire) {
                if (int64_t(siege.ticks) < kClaimSeconds * kTicksPerSecond) return;
                owner.empire = attacker;
                // Занятая пустая планета встаёт с полной обороной: за неё
                // никто не дрался, и отбирать её должно быть так же трудно,
                // как любую другую — иначе выгодно было бы ждать соседа.
                defense.readiness = defense.maxReadiness;
                siege.besieger = kNoEmpire;
                siege.ticks = 0;
                return;
            }

            // --- осада ---
            defense.readiness -= siegeRate(tonnage) * context.delta;
            if (defense.readiness > fx::zero()) return;

            // Планета пала.
            owner.empire = attacker;
            // Стройка нового хозяина не наследует чужой котлован: вложенные
            // минералы принадлежали прежнему владельцу, и достраивать за
            // него — это подарок за штурм, которого никто не обещал.
            if (PlanetConstruction* site = world.get<PlanetConstruction>(entity)) {
                enqueueConstruction(*site, PlanetConstruction::kNoSlot, Building::None);
            }
            // Свежий захват слаб: отбить его обратно реально, и фронт может
            // ходить туда-обратно, а не застывать после первого удара.
            defense.readiness = defense.maxReadiness * kCaptureReadinessShare;
            siege.besieger = kNoEmpire;
            siege.ticks = 0;
        });
}

}  // namespace pw::sim
