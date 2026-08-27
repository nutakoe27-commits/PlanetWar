#include "pw/sim/season.h"

#include <algorithm>
#include <vector>

#include "pw/core/hash.h"
#include "pw/sim/combat.h"
#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

namespace pw::sim {

// ---------------------------------------------------------------------------
// Таймер
// ---------------------------------------------------------------------------

int64_t SeasonConfig::stageSeconds(SeasonStage stage) const {
    const int64_t factor = std::max<int64_t>(1, scale);
    switch (stage) {
        case SeasonStage::Expansion: return expansionSeconds * factor;
        case SeasonStage::Conflict:  return conflictSeconds * factor;
        case SeasonStage::Crisis:    return crisisSeconds * factor;
        case SeasonStage::Final:     return finalSeconds * factor;
        default:                     return 0;
    }
}

int64_t SeasonConfig::totalSeconds() const {
    int64_t total = 0;
    for (uint8_t stage = 0; stage < uint8_t(SeasonStage::Count); ++stage) {
        total += stageSeconds(SeasonStage(stage));
    }
    return total;
}

void SeasonConfig::stretchTo(int64_t seconds) {
    const int64_t base = totalSeconds();
    if (base <= 0 || seconds <= 0) return;

    // Каждая стадия масштабируется отдельно и с округлением, а остаток
    // отдаётся Конфликту: он самый длинный, и лишняя минута в нём
    // не меняет ничего, тогда как в Финале она заметна.
    const int64_t factor = std::max<int64_t>(1, scale);
    auto share = [&](int64_t value) {
        return std::max<int64_t>(1, value * factor * seconds / base);
    };
    expansionSeconds = share(expansionSeconds);
    crisisSeconds = share(crisisSeconds);
    finalSeconds = share(finalSeconds);
    conflictSeconds = std::max<int64_t>(
        1, seconds - expansionSeconds - crisisSeconds - finalSeconds);
    scale = 1;
}

SeasonStage stageAt(const SeasonConfig& config, uint64_t tick) {
    // ЧИСТАЯ ФУНКЦИЯ ОТ ТИКА. Не «сервер решил, что пора»: и сервер,
    // и клиент, и реплей считают стадию из одного числа одинаково,
    // и рассинхрону взяться неоткуда.
    const int64_t seconds = int64_t(tick) / kTicksPerSecond;
    int64_t edge = 0;
    for (uint8_t stage = 0; stage < uint8_t(SeasonStage::Count); ++stage) {
        edge += config.stageSeconds(SeasonStage(stage));
        if (seconds < edge) return SeasonStage(stage);
    }
    return SeasonStage::Final;
}

int64_t secondsLeftInStage(const SeasonConfig& config, uint64_t tick) {
    const int64_t seconds = int64_t(tick) / kTicksPerSecond;
    int64_t edge = 0;
    for (uint8_t stage = 0; stage < uint8_t(SeasonStage::Count); ++stage) {
        edge += config.stageSeconds(SeasonStage(stage));
        if (seconds < edge) return edge - seconds;
    }
    return 0;
}

const char* stageName(SeasonStage stage) {
    switch (stage) {
        case SeasonStage::Expansion: return "Расширение";
        case SeasonStage::Conflict:  return "Конфликт";
        case SeasonStage::Crisis:    return "Кризис";
        case SeasonStage::Final:     return "Финал";
        default:                     return "?";
    }
}

void systemSeason(World& world, const TickContext& context) {
    Season* season = world.resource<Season>();
    if (season == nullptr) return;

    season->previousStage = season->stage;
    season->stage = stageAt(season->config, context.tick);
    season->secondsLeft = secondsLeftInStage(season->config, context.tick);
    season->over = int64_t(context.tick) / kTicksPerSecond >= season->config.totalSeconds();
}

// ---------------------------------------------------------------------------
// Убежище
// ---------------------------------------------------------------------------

bool siegeAllowed(const World& world, const Galaxy& galaxy, uint32_t system,
                  uint32_t defender) {
    const Season* season = world.resource<Season>();
    if (season == nullptr) return true;

    // На Финале карта заморожена: очки уже считаются, и захват в последнюю
    // минуту не должен решать сезон. Иначе вся стратегия сводится к тому,
    // чтобы не показываться до последнего часа.
    if (season->stage == SeasonStage::Final) return false;

    if (season->stage != SeasonStage::Expansion) return true;

    // Ничьи планеты занимают всегда: Расширение для того и есть.
    if (defender == kNoEmpire) return true;
    // Кризис не считается игроком и убежищем не прикрыт — но его на этой
    // стадии ещё и не существует.
    if (defender == kCrisisEmpire) return true;

    // Три прыжка вокруг чужой столицы неприкосновенны. Именно ЧУЖОЙ:
    // проверка идёт по владельцу планеты, поэтому сидеть в своём убежище
    // и бить оттуда тоже нельзя.
    bool sheltered = false;
    const_cast<World&>(world).each<Empire>([&](Entity, Empire& empire) {
        if (sheltered || empire.id != defender) return;
        if (empire.capitalSystem >= galaxy.systemCount()) return;
        const int32_t hops = galaxy.hopDistance(empire.capitalSystem, system);
        if (hops >= 0 && uint32_t(hops) <= kSanctuaryJumps) sheltered = true;
    });
    return !sheltered;
}

// ---------------------------------------------------------------------------
// Кризис
// ---------------------------------------------------------------------------

namespace {

/// Системы ядра — оттуда и выходит кризис.
///
/// Ядро выбрано не случайно: по docs/01 это самая ценная и самая спорная
/// область карты. Угроза, выходящая из пустого угла, никого не касается;
/// угроза, выходящая из того места, за которое все дрались всю партию,
/// касается всех сразу.
std::vector<uint32_t> coreSystems(const Galaxy& galaxy) {
    std::vector<uint32_t> core;
    const fx extent = galaxy.extent();
    const fx limit = extent / fx::fromInt(4);
    for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
        const fx x = galaxy.positionX(index);
        const fx y = galaxy.positionY(index);
        if (x * x + y * y <= limit * limit) core.push_back(index);
    }
    if (core.empty() && galaxy.systemCount() > 0) core.push_back(0);
    return core;
}

}  // namespace

void systemCrisis(World& world, const TickContext& context) {
    const Season* season = world.resource<Season>();
    const Galaxy* galaxy = world.resource<Galaxy>();
    Commands* commands = world.resource<Commands>();
    if (season == nullptr || galaxy == nullptr || commands == nullptr) return;
    if (season->stage != SeasonStage::Crisis) return;

    const int64_t period = kCrisisWaveSeconds * kTicksPerSecond;
    if (context.tick % uint64_t(period) != 0) return;

    // Сколько планет у кого. Волна на империю растёт с её размером:
    // это не наказание лидера, а работа для него. У того, кто взял
    // полкарты, и врагов должно быть вдвое, а отстающему кризис даёт
    // время — то самое, которого ему не хватало всю партию.
    std::vector<uint32_t> planets(256, 0);
    world.each<Planet, Owner>([&](Entity, Planet&, Owner& owner) {
        if (owner.empire == kNoEmpire || owner.empire >= planets.size()) return;
        ++planets[owner.empire];
    });

    const std::vector<uint32_t> core = coreSystems(*galaxy);
    if (core.empty()) return;

    uint32_t waveIndex = 0;
    for (uint32_t empire = 0; empire < planets.size(); ++empire) {
        if (empire == kCrisisEmpire || planets[empire] == 0) continue;

        const uint32_t tonnage =
            kCrisisWaveBase + kCrisisWavePerPlanet * planets[empire];

        // Состав волны: эскорт плюс тяжёлое ядро. Мониторов нет намеренно —
        // кризис давит массой и не должен брать укреплённые планеты быстрее,
        // чем это делают игроки. Его задача — заставить их обороняться,
        // а не отобрать у них карту.
        Fleet wave{};
        fleetAdd(wave, Hull::Destroyer, tonnage / 3 + 1);
        fleetAdd(wave, Hull::Cruiser, tonnage / 8 + 1);
        if (tonnage > 30) fleetAdd(wave, Hull::Battleship, tonnage / 24);

        // Точка выхода перемешивается по номеру империи и волне: волны
        // не должны появляться в одной и той же системе, иначе первая же
        // построенная там крепость закрывает кризис навсегда.
        const uint64_t mixed =
            mixCoord(galaxy->seed(), int64_t(context.tick), int64_t(empire));
        const uint32_t system = core[size_t(mixed % core.size())];

        commands->spawnFleet(kCrisisEmpire, system, wave);
        ++waveIndex;
    }
    (void)waveIndex;
}

// ---------------------------------------------------------------------------
// Престиж
// ---------------------------------------------------------------------------

void systemPrestige(World& world, const TickContext& context) {
    // Раз в секунду, а не каждый тик: престиж — это счёт за сезон, и десять
    // начислений в секунду не дают ничего, кроме десятикратного округления
    // в пользу того, у кого дробная часть удачнее.
    if (context.tick % uint64_t(kTicksPerSecond) != 0) return;

    const Season* season = world.resource<Season>();
    if (season == nullptr) return;

    // Кто чем владеет. Территория — планеты, взвешенные по числу слотов:
    // просторный мир стоит дороже выжженного камня, и это ровно то, за что
    // на карте воюют.
    std::vector<uint32_t> territory(256, 0);
    world.each<Planet, Owner>([&](Entity, Planet& planet, Owner& owner) {
        if (owner.empire == kNoEmpire || owner.empire >= territory.size()) return;
        territory[owner.empire] += 1u + uint32_t(planet.slots) / 3u;
    });

    world.each<Empire, Prestige>([&](Entity, Empire& empire, Prestige& prestige) {
        if (empire.id == kNoEmpire || empire.id >= territory.size()) return;

        prestige.territory = territory[empire.id];
        // Экономика и наука КОПЯТСЯ: это счёт за весь сезон, а не срез.
        // Иначе выигрывал бы тот, кто удачно потратился к последней минуте.
        prestige.economy += uint32_t((empire.minerals + empire.alloys).floorToInt() / 1000);
        prestige.science += uint32_t(empire.research.floorToInt() / 500);
        prestige.diplomacy = uint32_t(empire.influence.floorToInt() / 10);
    });
    (void)season;
}

void registerSeasonComponents(World& world) { world.registerComponent<Prestige>("Prestige"); }

}  // namespace pw::sim
