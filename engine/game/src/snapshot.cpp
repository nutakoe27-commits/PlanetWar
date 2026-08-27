#include "pw/game/snapshot.h"

#include <algorithm>

#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/production.h"
#include "pw/sim/season.h"

namespace pw::game {

namespace {

/// Сколько байт занимает одна система в дельте: индекс плюс шесть полей.
constexpr size_t kSystemEntryBound = 5 + 6;
/// Верхняя оценка на флот: идентификатор, империя, две системы, прогресс
/// и четыре счётчика — всё varint'ами.
constexpr size_t kFleetEntryBound = 5 + 1 + 5 + 5 + 10 + 4 * 5;
/// Планета: номер, десять однобайтовых полей и слоты застройки.
constexpr size_t kPlanetEntryBound = 5 + 10 + sim::kMaxSlots;

void writeSystem(ByteWriter& writer, uint32_t index, const SystemView& system) {
    writer.varint(index);
    writer.u8(system.owner);
    writer.u8(system.readiness);
    writer.u8(system.siegeEmpire);
    writer.u8(system.siegeProgress);
    writer.u8(system.ownedPlanets);
    writer.u8(system.totalPlanets);
}

bool readSystem(ByteReader& reader, uint32_t& index, SystemView& system) {
    index = uint32_t(reader.varint());
    system.owner = reader.u8();
    system.readiness = reader.u8();
    system.siegeEmpire = reader.u8();
    system.siegeProgress = reader.u8();
    system.ownedPlanets = reader.u8();
    system.totalPlanets = reader.u8();
    return !reader.failed();
}

void writeFleet(ByteWriter& writer, const FleetView& fleet) {
    writer.varint(fleet.id);
    writer.u8(fleet.empire);
    writer.varint(fleet.system);
    writer.varint(fleet.nextSystem);
    writer.varint(fleet.orbit == sim::kNoOrbit ? 0u : fleet.orbit + 1u);
    writer.fixed(fleet.progress);
    // ВЕСЬ состав циклом. Перечисление классов руками пропустило четыре
    // из восьми — см. FleetView::operator==.
    for (size_t index = 0; index < sim::kHullClasses; ++index) {
        writer.varint(fleet.composition.ships[index]);
    }
}

bool readFleet(ByteReader& reader, FleetView& fleet) {
    fleet.id = uint32_t(reader.varint());
    fleet.empire = reader.u8();
    fleet.system = uint32_t(reader.varint());
    fleet.nextSystem = uint32_t(reader.varint());
    // Орбита едет со сдвигом на единицу: ноль означает «нет орбиты».
    // Так kNoOrbit (все единицы) не превращается в пятибайтовый varint
    // в каждом пакете про каждый идущий флот.
    const uint64_t orbit = reader.varint();
    fleet.orbit = orbit == 0 ? sim::kNoOrbit : uint32_t(orbit - 1);
    fleet.progress = reader.fixed();
    for (size_t index = 0; index < sim::kHullClasses; ++index) {
        fleet.composition.ships[index] = uint32_t(reader.varint());
    }
    return !reader.failed();
}

void writePlanet(ByteWriter& writer, uint32_t id, const PlanetView& planet) {
    writer.varint(id);
    writer.u8(planet.owner);
    writer.u8(planet.specialization);
    writer.u8(planet.readiness);
    writer.u8(planet.siegeEmpire);
    writer.u8(planet.siegeProgress);
    writer.u8(planet.buildSlot);
    writer.u8(planet.buildBuilding);
    writer.u8(planet.buildPercent);
    writer.u8(planet.buildQueued);
    writer.u8(planet.buildPaid);
    for (uint8_t i = 0; i < sim::kMaxSlots; ++i) writer.u8(planet.buildings[i]);
}

bool readPlanet(ByteReader& reader, uint32_t& id, PlanetView& planet) {
    id = uint32_t(reader.varint());
    planet.owner = reader.u8();
    planet.specialization = reader.u8();
    planet.readiness = reader.u8();
    planet.siegeEmpire = reader.u8();
    planet.siegeProgress = reader.u8();
    planet.buildSlot = reader.u8();
    planet.buildBuilding = reader.u8();
    planet.buildPercent = reader.u8();
    planet.buildQueued = reader.u8();
    planet.buildPaid = reader.u8();
    for (uint8_t i = 0; i < sim::kMaxSlots; ++i) planet.buildings[i] = reader.u8();
    return !reader.failed();
}

void writeEmpire(ByteWriter& writer, const EmpireView& empire) {
    writer.fixed(empire.energy);
    writer.fixed(empire.minerals);
    writer.fixed(empire.alloys);
    writer.fixed(empire.research);
    writer.fixed(empire.influence);
    writer.fixed(empire.energyIncome);
    writer.fixed(empire.mineralsIncome);
    writer.fixed(empire.alloysIncome);
    writer.fixed(empire.researchIncome);
    writer.fixed(empire.influenceIncome);
    writer.fixed(empire.foundryIdle);
    writer.u8(empire.stage);
    writer.varint(empire.stageSecondsLeft);
    writer.varint(empire.prestigeTerritory);
    writer.varint(empire.prestigeEconomy);
    writer.varint(empire.prestigeScience);
    writer.varint(empire.prestigeWar);
    writer.varint(empire.prestigeDiplomacy);
}

bool readEmpire(ByteReader& reader, EmpireView& empire) {
    empire.energy = reader.fixed();
    empire.minerals = reader.fixed();
    empire.alloys = reader.fixed();
    empire.research = reader.fixed();
    empire.influence = reader.fixed();
    empire.energyIncome = reader.fixed();
    empire.mineralsIncome = reader.fixed();
    empire.alloysIncome = reader.fixed();
    empire.researchIncome = reader.fixed();
    empire.influenceIncome = reader.fixed();
    empire.foundryIdle = reader.fixed();
    empire.stage = reader.u8();
    empire.stageSecondsLeft = uint32_t(reader.varint());
    empire.prestigeTerritory = uint32_t(reader.varint());
    empire.prestigeEconomy = uint32_t(reader.varint());
    empire.prestigeScience = uint32_t(reader.varint());
    empire.prestigeWar = uint32_t(reader.varint());
    empire.prestigeDiplomacy = uint32_t(reader.varint());
    return !reader.failed();
}

bool sameEmpire(const EmpireView& a, const EmpireView& b) {
    // Сравниваются ВСЕ отправляемые поля, а не только запасы.
    //
    // Раньше сравнивались одни запасы, и это работало по случайности:
    // запасы меняются почти каждый тик, поэтому блок и так уходил заново,
    // а стадия с престижем ехали пассажирами. Стоило запасам замереть —
    // упёрлись в ноль по энергии, встала экономика на Финале — и смена
    // стадии перестала бы доезжать до клиента вовсе.
    return a.energy.raw() == b.energy.raw() && a.minerals.raw() == b.minerals.raw() &&
           a.alloys.raw() == b.alloys.raw() && a.research.raw() == b.research.raw() &&
           a.influence.raw() == b.influence.raw() &&
           a.energyIncome.raw() == b.energyIncome.raw() &&
           a.mineralsIncome.raw() == b.mineralsIncome.raw() &&
           a.alloysIncome.raw() == b.alloysIncome.raw() &&
           a.researchIncome.raw() == b.researchIncome.raw() &&
           a.influenceIncome.raw() == b.influenceIncome.raw() &&
           a.foundryIdle.raw() == b.foundryIdle.raw() && a.stage == b.stage &&
           a.stageSecondsLeft == b.stageSecondsLeft &&
           a.prestigeTerritory == b.prestigeTerritory &&
           a.prestigeEconomy == b.prestigeEconomy &&
           a.prestigeScience == b.prestigeScience && a.prestigeWar == b.prestigeWar &&
           a.prestigeDiplomacy == b.prestigeDiplomacy;
}

}  // namespace

// ---------------------------------------------------------------------------
// Сбор состояния
// ---------------------------------------------------------------------------

void collectView(sim::World& world, const sim::Galaxy& galaxy, uint32_t empire,
                 uint64_t tick, WorldView& out) {
    const uint32_t systemCount = galaxy.systemCount();
    out.tick = tick;
    if (out.systems.size() != systemCount) out.resize(systemCount);
    else std::fill(out.systems.begin(), out.systems.end(), SystemView{});
    out.fleets.clear();
    out.planets.clear();

    // Сколько тиков осады нужно, чтобы застолбить ничью планету. По этому
    // же числу считается процент для полосы прогресса в интерфейсе.
    constexpr int64_t kClaimTicks = sim::kClaimSeconds * sim::kTicksPerSecond;

    // --- владельцы систем (производные) ---
    world.each<sim::StarSystem, sim::Owner>(
        [&](sim::Entity, sim::StarSystem& system, sim::Owner& owner) {
            if (system.index >= systemCount) return;
            out.systems[system.index].owner =
                uint8_t(owner.empire == sim::kNoEmpire ? 0xFFu : owner.empire & 0xFFu);
        });

    // --- планеты: владение, оборона, осада, стройка ---
    //
    // Сводка по системе набирается ЗДЕСЬ ЖЕ, одним проходом: карта галактики
    // показывает средние по своим планетам, а подробности игрок смотрит
    // в виде системы. Второй проход ради тех же данных был бы лишним обходом
    // самой многочисленной таблицы мира.
    std::vector<uint32_t> readinessSum(systemCount, 0);

    world.each<sim::Planet, sim::Owner, sim::PlanetDefense, sim::SiegeState>(
        [&](sim::Entity entity, sim::Planet& planet, sim::Owner& owner,
            sim::PlanetDefense& defense, sim::SiegeState& siege) {
            PlanetView view;
            view.owner =
                uint8_t(owner.empire == sim::kNoEmpire ? 0xFFu : owner.empire & 0xFFu);
            view.specialization = planet.specialization;

            // Готовность — доля от потолка ЭТОЙ планеты, а не абсолютное
            // число: потолок зависит от построенных на ней крепостей,
            // и без нормировки полоска в интерфейсе ничего не значила бы.
            int64_t readiness = 0;
            if (defense.maxReadiness > fx::zero()) {
                readiness = (defense.readiness * fx::fromInt(100) / defense.maxReadiness)
                                .floorToInt();
            }
            view.readiness = uint8_t(std::clamp<int64_t>(readiness, 0, 100));

            view.siegeEmpire =
                uint8_t(siege.besieger == sim::kNoEmpire ? 0xFFu : siege.besieger & 0xFFu);

            // Знаменатель разный: ничью планету занимают за kClaimSeconds,
            // чужую грызут до нуля обороны. Один процент на оба случая
            // означал бы полосу, которая для половины ситуаций врёт.
            int64_t progress = 0;
            if (owner.empire == sim::kNoEmpire) {
                progress = kClaimTicks > 0 ? int64_t(siege.ticks) * 100 / kClaimTicks : 0;
            } else if (siege.besieger != sim::kNoEmpire) {
                progress = 100 - int64_t(view.readiness);
            }
            view.siegeProgress = uint8_t(std::clamp<int64_t>(progress, 0, 100));

            if (const sim::PlanetConstruction* site =
                    world.get<sim::PlanetConstruction>(entity)) {
                view.buildSlot = site->slot;
                view.buildBuilding = site->building;
                view.buildPercent = uint8_t(sim::constructionPercent(*site));
                view.buildQueued = site->queued;
                view.buildPaid = site->paid;
            }

            if (const sim::PlanetDevelopment* development =
                    world.get<sim::PlanetDevelopment>(entity)) {
                for (uint8_t slot = 0; slot < sim::kMaxSlots; ++slot) {
                    view.buildings[slot] = development->buildings[slot];
                }
            }

            out.planets[entity.index] = view;

            if (planet.system >= systemCount) return;
            SystemView& summary = out.systems[planet.system];
            ++summary.totalPlanets;
            if (view.owner != 0xFF && view.owner == summary.owner) {
                ++summary.ownedPlanets;
                readinessSum[planet.system] += view.readiness;
            }
            // На карте видна ТА осада, что идёт прямо сейчас. Их в системе
            // может идти не больше одной: флот работает по планетам
            // по очереди.
            if (siege.besieger != sim::kNoEmpire) {
                summary.siegeEmpire = view.siegeEmpire;
                summary.siegeProgress = view.siegeProgress;
            }
        });

    for (uint32_t index = 0; index < systemCount; ++index) {
        SystemView& summary = out.systems[index];
        if (summary.ownedPlanets == 0) continue;
        summary.readiness = uint8_t(readinessSum[index] / summary.ownedPlanets);
    }

    world.each<sim::Fleet, sim::FleetLocation, sim::Owner>(
        [&](sim::Entity entity, sim::Fleet& fleet, sim::FleetLocation& location,
            sim::Owner& owner) {
            if (sim::fleetEmpty(fleet)) return;
            FleetView view;
            view.id = entity.index;
            view.empire = uint8_t(owner.empire == sim::kNoEmpire ? 0xFFu : owner.empire & 0xFFu);
            view.system = location.system;
            view.nextSystem = location.nextSystem;
            view.orbit = location.orbit;
            view.progress = location.progress;
            view.composition = fleet;
            out.fleets[view.id] = view;
        });

    out.empire = EmpireView{};
    world.each<sim::Empire>([&](sim::Entity entity, sim::Empire& record) {
        if (record.id != empire) return;
        out.empire.energy = record.energy;
        out.empire.minerals = record.minerals;
        out.empire.alloys = record.alloys;
        out.empire.research = record.research;
        out.empire.influence = record.influence;
        if (const sim::Prestige* prestige = world.get<sim::Prestige>(entity)) {
            out.empire.prestigeTerritory = prestige->territory;
            out.empire.prestigeEconomy = prestige->economy;
            out.empire.prestigeScience = prestige->science;
            out.empire.prestigeWar = prestige->war;
            out.empire.prestigeDiplomacy = prestige->diplomacy;
        }
    });

    // Приход берётся из бухгалтерии тика — той самой, по которой сервер
    // и начисляет ресурсы. Второго счёта нет, разойтись нечему.
    if (const sim::Ledger* ledger = world.resource<sim::Ledger>()) {
        if (empire < ledger->size()) {
            const sim::Ledger::Flow& flow = ledger->at(empire);
            out.empire.energyIncome = flow.energy;
            out.empire.mineralsIncome = flow.minerals;
            out.empire.alloysIncome = flow.alloys;
            out.empire.researchIncome = flow.research;
            out.empire.influenceIncome = flow.influence;
            out.empire.foundryIdle = flow.foundryIdle;
        }
    }

    if (const sim::Season* season = world.resource<sim::Season>()) {
        out.empire.stage = uint8_t(season->stage);
        out.empire.stageSecondsLeft = uint32_t(std::max<int64_t>(0, season->secondsLeft));
    }
}

// ---------------------------------------------------------------------------
// Сторона сервера
// ---------------------------------------------------------------------------

void SnapshotWriter::reset(uint32_t systemCount) {
    base_ = WorldView{};
    base_.resize(systemCount);
    baseEmpireValid_ = false;
    for (Pending& slot : pending_) slot = Pending{};
    nextId_ = 1;
    systemCursor_ = 0;
}

uint32_t SnapshotWriter::pending() const {
    uint32_t total = 0;
    for (const Pending& slot : pending_) {
        if (slot.used) ++total;
    }
    return total;
}

void SnapshotWriter::write(ByteWriter& writer, const WorldView& current) {
    const uint16_t id = nextId_++;
    Pending& record = pending_[id % kPendingSlots];
    record = Pending{};
    record.id = id;
    record.used = true;
    record.tick = current.tick;

    writer.varint(id);
    writer.varint(current.tick);

    // --- ресурсы своей империи ---
    const bool empireChanged = !baseEmpireValid_ || !sameEmpire(base_.empire, current.empire);
    writer.boolean(empireChanged);
    if (empireChanged) {
        writeEmpire(writer, current.empire);
        record.empire = current.empire;
        record.empireSent = true;
    }

    // --- системы ---
    //
    // Обход идёт по кругу от прошлой остановки. Иначе при нехватке места
    // мы бы каждый раз паковали одни и те же первые системы, а хвост карты
    // не обновлялся бы никогда.
    const uint32_t systemCount = uint32_t(current.systems.size());
    if (base_.systems.size() != systemCount) {
        base_.resize(systemCount);
        baseEmpireValid_ = false;
    }

    std::vector<std::pair<uint32_t, SystemView>> changedSystems;
    for (uint32_t step = 0; step < systemCount; ++step) {
        const uint32_t index = (systemCursor_ + step) % systemCount;
        if (current.systems[index] == base_.systems[index]) continue;
        changedSystems.emplace_back(index, current.systems[index]);
    }

    // --- флоты ---
    std::vector<std::pair<uint32_t, FleetView>> changedFleets;
    for (const auto& [id2, fleet] : current.fleets) {
        const auto found = base_.fleets.find(id2);
        if (found != base_.fleets.end() && found->second == fleet) continue;
        changedFleets.emplace_back(id2, fleet);
    }

    std::vector<uint32_t> removed;
    for (const auto& [id2, fleet] : base_.fleets) {
        if (current.fleets.count(id2) == 0) removed.push_back(id2);
    }

    // --- укладка по бюджету ---
    //
    // Порядок приоритетов не случаен: сначала исчезнувшие флоты (иначе
    // клиент рисует призраков), потом изменившиеся флоты (движение видно
    // глазом), потом системы (владение меняется редко и терпит кадр).
    const size_t room = writer.capacity() > writer.size() ? writer.capacity() - writer.size() : 0;
    size_t budget = room > 16 ? room - 16 : 0;   // запас на три счётчика

    size_t removedCount = 0;
    for (uint32_t fleetId : removed) {
        const size_t cost = varintSize(fleetId);
        if (cost > budget) break;
        budget -= cost;
        ++removedCount;
    }
    writer.varint(removedCount);
    for (size_t i = 0; i < removedCount; ++i) {
        writer.varint(removed[i]);
        record.removed.push_back(removed[i]);
    }

    size_t fleetCount = 0;
    while (fleetCount < changedFleets.size() && budget >= kFleetEntryBound) {
        budget -= kFleetEntryBound;
        ++fleetCount;
    }
    writer.varint(fleetCount);
    for (size_t i = 0; i < fleetCount; ++i) {
        writeFleet(writer, changedFleets[i].second);
        record.fleets.push_back(changedFleets[i]);
    }

    // Застройка планет. Меняется реже флотов, но игрок ждёт её сразу
    // после нажатия — поэтому раньше систем.
    std::vector<std::pair<uint32_t, PlanetView>> changedPlanets;
    for (const auto& [id2, planet] : current.planets) {
        const auto found = base_.planets.find(id2);
        if (found != base_.planets.end() && found->second == planet) continue;
        changedPlanets.emplace_back(id2, planet);
    }

    size_t planetCount = 0;
    while (planetCount < changedPlanets.size() && budget >= kPlanetEntryBound) {
        budget -= kPlanetEntryBound;
        ++planetCount;
    }
    writer.varint(planetCount);
    for (size_t i = 0; i < planetCount; ++i) {
        writePlanet(writer, changedPlanets[i].first, changedPlanets[i].second);
        record.planets.push_back(changedPlanets[i]);
    }

    size_t systemEntries = 0;
    while (systemEntries < changedSystems.size() && budget >= kSystemEntryBound) {
        budget -= kSystemEntryBound;
        ++systemEntries;
    }
    writer.varint(systemEntries);
    for (size_t i = 0; i < systemEntries; ++i) {
        writeSystem(writer, changedSystems[i].first, changedSystems[i].second);
        record.systems.push_back(changedSystems[i]);
    }

    // Двигаем курсор за последнюю уложенную систему.
    if (systemEntries > 0 && systemCount > 0) {
        systemCursor_ = (changedSystems[systemEntries - 1].first + 1) % systemCount;
    }
}

void SnapshotWriter::acknowledge(uint16_t snapshotId) {
    Pending& record = pending_[snapshotId % kPendingSlots];
    if (!record.used || record.id != snapshotId) return;

    // База сдвигается ровно на то, что клиент подтвердил. Всё, что не
    // влезло или потерялось, остаётся расхождением и уедет в следующий раз.
    base_.tick = record.tick;
    if (record.empireSent) {
        base_.empire = record.empire;
        baseEmpireValid_ = true;
    }
    for (const auto& [index, system] : record.systems) {
        if (index < base_.systems.size()) base_.systems[index] = system;
    }
    for (const auto& [id, fleet] : record.fleets) base_.fleets[id] = fleet;
    for (const auto& [id, planet] : record.planets) base_.planets[id] = planet;
    for (uint32_t id : record.removed) base_.fleets.erase(id);

    // Старые снапшоты закрываем: клиент подтвердил более новый, значит
    // они либо дошли, либо уже неактуальны.
    for (Pending& slot : pending_) {
        if (!slot.used) continue;
        if (slot.id == snapshotId || sequenceLessThan(slot.id, snapshotId)) slot.used = false;
    }
}

// ---------------------------------------------------------------------------
// Сторона клиента
// ---------------------------------------------------------------------------

void SnapshotReader::reset(uint32_t systemCount) {
    view_ = WorldView{};
    view_.resize(systemCount);
    lastId_ = 0;
}

bool SnapshotReader::apply(ByteReader& reader) {
    const uint64_t rawId = reader.varint();
    const uint64_t tick = reader.varint();
    if (reader.failed() || rawId > 0xFFFF) return false;

    const uint16_t id = uint16_t(rawId);
    // Устаревший снапшот применять нельзя: он вернул бы мир в прошлое.
    // Он не ошибка — просто пришёл не по порядку, что для ненадёжной
    // части пакета совершенно нормально.
    if (lastId_ != 0 && !sequenceGreaterThan(id, lastId_)) return true;

    const bool empireChanged = reader.boolean();
    if (reader.failed()) return false;
    EmpireView empire = view_.empire;
    if (empireChanged && !readEmpire(reader, empire)) return false;

    const uint64_t removedCount = reader.varint();
    if (reader.failed() || removedCount > 100000) return false;
    std::vector<uint32_t> removed;
    removed.reserve(size_t(std::min<uint64_t>(removedCount, 1024)));
    for (uint64_t i = 0; i < removedCount; ++i) {
        const uint64_t fleetId = reader.varint();
        if (reader.failed()) return false;
        removed.push_back(uint32_t(fleetId));
    }

    const uint64_t fleetCount = reader.varint();
    if (reader.failed() || fleetCount > 100000) return false;
    std::vector<FleetView> fleets;
    fleets.reserve(size_t(std::min<uint64_t>(fleetCount, 1024)));
    for (uint64_t i = 0; i < fleetCount; ++i) {
        FleetView fleet;
        if (!readFleet(reader, fleet)) return false;
        fleets.push_back(fleet);
    }

    const uint64_t planetCount = reader.varint();
    if (reader.failed() || planetCount > 1000000) return false;
    std::vector<std::pair<uint32_t, PlanetView>> planets;
    planets.reserve(size_t(std::min<uint64_t>(planetCount, 1024)));
    for (uint64_t i = 0; i < planetCount; ++i) {
        uint32_t planetId = 0;
        PlanetView planet;
        if (!readPlanet(reader, planetId, planet)) return false;
        planets.emplace_back(planetId, planet);
    }

    const uint64_t systemCount = reader.varint();
    if (reader.failed() || systemCount > 1000000) return false;
    std::vector<std::pair<uint32_t, SystemView>> systems;
    systems.reserve(size_t(std::min<uint64_t>(systemCount, 1024)));
    for (uint64_t i = 0; i < systemCount; ++i) {
        uint32_t index = 0;
        SystemView system;
        if (!readSystem(reader, index, system)) return false;
        if (index >= view_.systems.size()) return false;
        systems.emplace_back(index, system);
    }

    // Применяем только после того, как разобрали ВСЁ. Иначе битый пакет
    // оставил бы мир наполовину обновлённым — половина из этого кадра,
    // половина из прошлого.
    view_.tick = tick;
    view_.empire = empire;
    for (uint32_t fleetId : removed) view_.fleets.erase(fleetId);
    for (const FleetView& fleet : fleets) view_.fleets[fleet.id] = fleet;
    for (const auto& [planetId, planet] : planets) view_.planets[planetId] = planet;
    for (const auto& [index, system] : systems) view_.systems[index] = system;
    lastId_ = id;
    return true;
}

}  // namespace pw::game
