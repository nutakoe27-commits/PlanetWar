#include "pw/sim/control.h"

#include <algorithm>

#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

namespace pw::sim {

void registerControlComponents(World& world) {
    world.registerComponent<SystemDefense>("SystemDefense");
    world.registerComponent<SiegeState>("SiegeState");
}

void initialiseControl(World& world, const Galaxy& galaxy) {
    for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
        const Entity entity = galaxy.systemEntity(index);
        world.add<Owner>(entity, Owner{kNoEmpire, 0});
        // У ничьей системы оборонять нечего: готовность нулевая, потолок есть.
        world.add<SystemDefense>(entity, SystemDefense{fx::zero(), kReadinessMax});
        world.add<SiegeState>(entity, SiegeState{kNoEmpire, 0});
    }
}

// ---------------------------------------------------------------------------
// Присутствие
// ---------------------------------------------------------------------------

void Presence::rebuild(World& world, uint32_t systemCount) {
    entries_.assign(systemCount, Entry{});

    // Сначала владельцы: без них не отличить свои силы от чужих.
    std::vector<uint32_t> owners(systemCount, kNoEmpire);
    world.each<StarSystem, Owner>([&](Entity, StarSystem& system, Owner& owner) {
        if (system.index < systemCount) owners[system.index] = owner.empire;
    });

    // Затем флоты. Считаются только СТОЯЩИЕ: флот в пути находится между
    // системами и не участвует ни в осаде, ни в обороне. Иначе флот
    // «защищал» бы систему, которую уже покинул.
    world.each<Fleet, FleetLocation, Owner>(
        [&](Entity, Fleet& fleet, FleetLocation& location, Owner& owner) {
            if (location.system != location.nextSystem) return;
            if (location.system >= systemCount) return;

            Entry& entry = entries_[location.system];
            const uint32_t tonnage = fleetTonnage(fleet);
            if (tonnage == 0) return;

            if (owner.empire == owners[location.system]) {
                entry.ownerTonnage += tonnage;
                return;
            }

            // Сильнейший чужой. При равенстве побеждает меньший номер
            // империи — порядок обхода не имеет права решать исход.
            const bool stronger =
                tonnage > entry.hostileTonnage ||
                (tonnage == entry.hostileTonnage && owner.empire < entry.hostileEmpire);

            // Считаем РАЗНЫЕ империи. Держим две: этого хватает, чтобы
            // отличить «единственный претендент» от «их несколько», а полный
            // список рос бы как число систем на число империй.
            if (entry.hostileCount == 0) {
                entry.hostileCount = 1;
                entry.hostileEmpire = owner.empire;
                entry.hostileTonnage = tonnage;
                return;
            }
            if (owner.empire != entry.hostileEmpire && owner.empire != entry.secondEmpire) {
                if (entry.hostileCount == 1) entry.secondEmpire = owner.empire;
                entry.hostileCount = 2;
            }
            if (stronger) {
                if (entry.hostileEmpire != owner.empire) entry.secondEmpire = entry.hostileEmpire;
                entry.hostileTonnage = tonnage;
                entry.hostileEmpire = owner.empire;
            }
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
/// Втрое больший флот не берёт систему втрое быстрее. Иначе выигрывал бы
/// просто тот, кто собрал всё в один кулак, и разделять силы никогда не имело
/// бы смысла — а вместе с этим исчезли бы и фронт, и рейды.
fx siegeRate(uint32_t tonnage) {
    const uint32_t capped = std::min(tonnage, kSiegeTonnageScale);
    const fx scale = fx::one() + fx::fromFraction(int64_t(capped), kSiegeTonnageScale);
    return kSiegeBaseRate * scale;
}

}  // namespace

void systemSiege(World& world, const TickContext& context) {
    const Presence* presence = world.resource<Presence>();
    if (presence == nullptr) return;

    world.each<StarSystem, Owner, SystemDefense, SiegeState>(
        [&](Entity, StarSystem& system, Owner& owner, SystemDefense& defense,
            SiegeState& siege) {
            if (system.index >= presence->size()) return;
            const Presence::Entry& here = presence->at(system.index);

            // --- ничья система: занятие ---
            if (owner.empire == kNoEmpire) {
                // Занимать можно только когда претендент один. Если сошлись
                // двое, никто не занимает: сначала надо разобраться между собой.
                if (here.hostileCount != 1) {
                    siege.besieger = kNoEmpire;
                    siege.ticks = 0;
                    return;
                }
                if (siege.besieger != here.hostileEmpire) {
                    siege.besieger = here.hostileEmpire;
                    siege.ticks = 0;
                }
                ++siege.ticks;
                if (int64_t(siege.ticks) >= kClaimSeconds * kTicksPerSecond) {
                    owner.empire = siege.besieger;
                    defense.readiness = defense.maxReadiness;
                    siege.besieger = kNoEmpire;
                    siege.ticks = 0;
                }
                return;
            }

            // --- своя система под защитой: осады нет ---
            //
            // Присутствие защитников снимает осаду немедленно. Поэтому
            // деблокирующий удар работает: пришёл, встал — осада сорвана.
            const bool defended = here.ownerTonnage > 0;
            const bool besieged = here.hostileTonnage > 0 && !defended;

            if (!besieged) {
                siege.besieger = kNoEmpire;
                siege.ticks = 0;
                if (defense.readiness < defense.maxReadiness) {
                    defense.readiness = min(defense.maxReadiness,
                                            defense.readiness + kReadinessRegen * context.delta);
                }
                return;
            }

            // --- осада ---
            if (siege.besieger != here.hostileEmpire) {
                siege.besieger = here.hostileEmpire;
                siege.ticks = 0;  // сменился осаждающий — счёт заново
            }
            ++siege.ticks;

            defense.readiness -= siegeRate(here.hostileTonnage) * context.delta;
            if (defense.readiness > fx::zero()) return;

            // Система пала.
            owner.empire = siege.besieger;
            // Свежий захват слаб: отбить его обратно реально, и фронт
            // может ходить туда-обратно, а не застывать после первого удара.
            defense.readiness = defense.maxReadiness * kCaptureReadinessShare;
            siege.besieger = kNoEmpire;
            siege.ticks = 0;
        });
}

}  // namespace pw::sim
