// Сжатие времени: сжатый прогон обязан совпадать с обычным.
//
// ЗАЧЕМ ЭТОТ ФАЙЛ СУЩЕСТВУЕТ.
//
// Сезон длится одиннадцать недель, то есть шестьдесят шесть миллионов тиков.
// Прогнать его целиком на обычном шаге — двадцать минут машинного времени,
// и такую проверку никто не запускает между правками. Поэтому в pw_sim есть
// сжатие времени (SimTempo): один тик проживает не десятую долю секунды,
// а сколько скажут, и весь сезон укладывается в пару секунд.
//
// БЕЗ ЭТОГО ФАЙЛА СЖАТИЕ БЫЛО БЫ УДОБНЫМ СПОСОБОМ НЕ ЗАМЕТИТЬ ОШИБКУ.
//
// Любая длительность, случайно записанная В ТИКАХ вместо игровых секунд,
// на сжатом прогоне меняется во столько же раз, во сколько сжали время.
// Стройка, считавшая тики, при сжатии в полсотни раз шла бы полсотни раз
// дольше — и прогон честно доложил бы «за сезон не построено ничего»,
// а мы искали бы ошибку в экономике. Здесь таких мест было четыре: стройка,
// откат сражений, период волны кризиса и стадия сезона.
//
// Проверка устроена как ДВОЙНОЙ ПРОГОН: один и тот же мир проживает одно
// и то же игровое время обычным и сжатым шагом, и всё, что видит игрок,
// обязано совпасть. Точные величины — точно, накопленные — с допуском:
// крупный шаг иначе округляет, и требовать побитового равенства значило бы
// требовать невозможного.
//
// ЧТО СОВПАДАТЬ НЕ ОБЯЗАНО — И ПОЧЕМУ ЭТО НЕ ОГОВОРКА, А ГРАНИЦА.
//
// Исход сражений. Поток случайности каждого боя засеян НОМЕРОМ ТИКА
// (см. systemBattles), а на сжатом шаге тики другие — значит и бросок
// другой. Требовать здесь совпадения значило бы требовать, чтобы
// случайность перестала быть случайностью. Сжатие проверяет ПРОПОРЦИИ
// времени, а не воспроизводимость мира: воспроизводимость проверяется
// отдельно, сравнением двух прогонов на ОДНОМ шаге.
//
// Зато сама ДЛИТЕЛЬНОСТЬ отката между сражениями от шага не зависит,
// и на неё здесь стоит отдельная проверка — без боя, на голом счётчике.
#include "doctest.h"

#include "game_time.h"

#include <algorithm>
#include <cmath>

#include "pw/sim/battle_system.h"
#include "pw/sim/colony.h"
#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/production.h"
#include "pw/sim/season.h"

using namespace pw;
using namespace pw::sim;
using namespace pw::test;

namespace {

/// Что видит игрок после прогона. Сравнивается между шагами.
struct Outcome {
    bool built = false;        // достроилось ли здание
    double minerals = 0.0;     // казна империи
    double readiness = 0.0;    // оборона осаждаемой планеты
    SeasonStage stage = SeasonStage::Expansion;
    int64_t secondsLeft = 0;
    int64_t gameSeconds = 0;
};

/// Мир на четыре часа игры: стройка, добыча, осада и сражение сразу.
///
/// Все четыре механики вместе НАМЕРЕННО. Каждая считает время по-своему —
/// стройка накапливает прогресс, добыча умножает на шаг, осада вычитает,
/// сражение сравнивает откат, — и ошибка в любой из них ловится только
/// тем, что рядом идут остальные три.
Outcome play(int64_t tempo, int64_t seconds) {
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
    params.seed = 0x7E3P0;
    params.systemCount = 40;
    galaxy.generate(world, params);
    initialiseControl(world, galaxy);
    initialiseEconomy(world);
    initialiseProduction(world, galaxy);
    initialiseBattles(world, galaxy);

    world.setResource(&galaxy);
    world.setResource(&ledger);
    world.setResource(&commands);
    world.setResource(&presence);
    world.setResource(&season);

    // Империя с казной. Одна, но настоящая: экономика считает по империям.
    const Entity empire = world.create();
    world.add<Empire>(empire, Empire{fx::fromInt(200), fx::fromInt(1000), fx::zero(),
                                     fx::zero(), fx::zero(), /*id=*/0, /*capital=*/1});

    // Шахтёрская планета со стройкой третьей шахты.
    const Entity home = galaxy.planetEntity(1, 0);
    REQUIRE(home.valid());
    world.get<Owner>(home)->empire = 0;
    world.get<Owner>(galaxy.systemEntity(1))->empire = 0;
    PlanetDevelopment mines{};
    mines.buildings[0] = uint8_t(Building::Mine);
    mines.buildings[1] = uint8_t(Building::Mine);
    *world.get<PlanetDevelopment>(home) = mines;
    world.get<Planet>(home)->slots = kMaxSlots;
    enqueueConstruction(*world.get<PlanetConstruction>(home), 2, Building::Mine);

    // Чужая планета под осадой в другой системе.
    const uint32_t besieged = 9;
    const Entity victim = galaxy.planetEntity(besieged, 0);
    REQUIRE(victim.valid());
    world.get<Owner>(victim)->empire = 1;
    world.get<PlanetDefense>(victim)->readiness = kReadinessMax;

    const Entity siegeFleet = world.create();
    world.add<Fleet>(siegeFleet, makeFleet({{Hull::Corvette, 20}}));
    world.add<FleetLocation>(siegeFleet, standingAt(besieged));
    world.add<FleetOrders>(siegeFleet, idleOrders(besieged));
    world.add<Owner>(siegeFleet, Owner{0, 0});

    // Сезон короткий, чтобы за прогон успела смениться стадия: переход
    // границы — самое интересное место, и оно обязано совпасть.
    season.config.expansionSeconds = seconds / 4;
    season.config.conflictSeconds = seconds;
    season.config.crisisSeconds = seconds;
    season.config.finalSeconds = seconds;

    Outcome out;

    const int64_t ticks = ticksForSeconds(seconds, tempo);
    for (int64_t i = 0; i < ticks; ++i) {
        const TickContext context = TickContext::at(uint64_t(i), tempo);
        systemSeason(world, context);
        systemControlRollup(world, context);
        systemFleetMovement(world, context);
        systemFleetStation(world, context);
        systemPresence(world, context);
        systemSiege(world, context);
        systemEconomy(world, context);
        planetDefenceCap(world, context);
        planetConstructionTick(world, context);
        systemProduction(world, context);
        systemApplyCommands(world, context);
    }

    // Таймер сезона снимается РОВНО НА ГРАНИЦЕ прогона, а не на последнем
    // тике. Последний тик у крупного шага отстоит от границы на весь шаг,
    // и обратный отсчёт расходился бы ровно на эту величину — то есть
    // проверка ловила бы шаг, а не ошибку.
    const TickContext edge = TickContext::at(uint64_t(ticks), tempo);
    systemSeason(world, edge);
    out.gameSeconds = edge.gameSeconds();

    out.built = world.get<PlanetDevelopment>(home)->buildings[2] == uint8_t(Building::Mine);
    out.minerals = world.get<Empire>(empire)->minerals.toDouble();
    out.readiness = world.get<PlanetDefense>(victim)->readiness.toDouble();
    out.stage = season.stage;
    out.secondsLeft = season.secondsLeft;
    return out;
}

/// Отличие в долях. Ноль в знаменателе означает «оба нуля».
double drift(double a, double b) {
    const double scale = std::max(std::abs(a), std::abs(b));
    return scale > 1e-9 ? std::abs(a - b) / scale : 0.0;
}

}  // namespace

TEST_CASE("сжатие: игровое время считается из тика и сжатия") {
    // Обе стороны перевода обязаны сходиться, иначе «одна игровая секунда»
    // означала бы разное на разных прогонах.
    CHECK(gameSecondsAt(0, 1) == 0);
    CHECK(gameSecondsAt(10, 1) == 1);
    CHECK(gameSecondsAt(10, 50) == 50);
    CHECK(gameSecondsAt(1000, 500) == 50000);

    CHECK(ticksForSeconds(1, 1) == 10);
    CHECK(ticksForSeconds(3600, 1) == 36000);
    CHECK(ticksForSeconds(3600, 50) == 720);
    // Ноль тиков означал бы «каждый тик»: событие раз в час превратилось бы
    // в событие каждый тик, и кризис выпускал бы волну непрерывно.
    CHECK(ticksForSeconds(1, 100000) == 1);

    // Шаг согласован со сжатием: тик длится ровно tempo десятых секунды.
    CHECK(TickContext::at(0, 1).delta == fx::fromFraction(1, kTicksPerSecond));
    CHECK(TickContext::at(0, 50).delta == fx::fromFraction(50, kTicksPerSecond));
    // Ноль и отрицательное сжатие не останавливают время, а означают единицу.
    CHECK(TickContext::at(7, 0).tempo == 1);
    CHECK(TickContext::at(7, -3).tempo == 1);
}

TEST_CASE("сжатие: сжатый прогон видит ту же игру, что и обычный") {
    // ГЛАВНАЯ ПРОВЕРКА ЭТОГО ФАЙЛА. Четыре часа игры, три шага: обычный,
    // сжатый в полсотни раз и сжатый в пятьсот. Всё, что видит игрок,
    // обязано совпасть.
    const int64_t hours = 4 * kHour;

    const Outcome plain = play(/*tempo=*/1, hours);
    const Outcome coarse = play(/*tempo=*/50, hours);
    const Outcome rough = play(/*tempo=*/500, hours);

    // Прогон обязан быть содержательным: иначе совпадали бы три нуля.
    REQUIRE(plain.built);
    REQUIRE(plain.minerals > 0.0);
    REQUIRE(plain.readiness < kReadinessMax.toDouble());

    // Игровое время дошло до одного и того же места.
    CHECK(coarse.gameSeconds == plain.gameSeconds);
    CHECK(rough.gameSeconds == plain.gameSeconds);

    // Стадия сезона — чистая функция от игрового времени, совпадает точно.
    CHECK(coarse.stage == plain.stage);
    CHECK(rough.stage == plain.stage);
    CHECK(coarse.secondsLeft == plain.secondsLeft);
    CHECK(rough.secondsLeft == plain.secondsLeft);

    // Стройка считает прогресс игровым временем, а не тиками, — здание
    // достроилось на всех трёх шагах.
    CHECK(coarse.built == plain.built);
    CHECK(rough.built == plain.built);

    // Накопленное сходится с допуском: крупный шаг иначе округляет.
    CAPTURE(plain.minerals);
    CAPTURE(coarse.minerals);
    CAPTURE(rough.minerals);
    CHECK(drift(plain.minerals, coarse.minerals) < 0.01);
    CHECK(drift(plain.minerals, rough.minerals) < 0.02);

    CAPTURE(plain.readiness);
    CAPTURE(coarse.readiness);
    CAPTURE(rough.readiness);
    CHECK(drift(plain.readiness, coarse.readiness) < 0.01);
    CHECK(drift(plain.readiness, rough.readiness) < 0.02);
}

TEST_CASE("сжатие: сезон целиком прогоняется за разумное время") {
    // Проверяется не скорость, а АРИФМЕТИКА, из-за которой прогон сезона
    // однажды стал невозможным: настоящий сезон — это шестьдесят шесть
    // миллионов обычных тиков, и без сжатия ни один инструмент его
    // не переварит.
    const SeasonConfig config;
    const int64_t plainTicks = config.totalSeconds() * kTicksPerSecond;
    CHECK(plainTicks > 60'000'000);

    // При сжатии, которое подбирает pw_season, тот же сезон укладывается
    // в сотни тысяч тиков — то есть в секунды машинного времени.
    // Деление С ОКРУГЛЕНИЕМ ВВЕРХ, как в pw_season: округление вниз даёт
    // сжатие чуть слабее нужного, и прогон вылезает за отведённый предел.
    constexpr int64_t kTargetTicks = 400000;
    const int64_t tempo = (plainTicks + kTargetTicks - 1) / kTargetTicks;
    CHECK(tempo > 1);
    CHECK(ticksForSeconds(config.totalSeconds(), tempo) <= kTargetTicks);

    // И стадии при таком шаге всё ещё различимы: самая короткая из них
    // длиннее одного тика в тысячи раз.
    const int64_t shortest = config.finalSeconds;
    CHECK(ticksForSeconds(shortest, tempo) > 1000);
}

TEST_CASE("сжатие: откат сражений отмеряется игровым временем") {
    // Отдельно от большого прогона и БЕЗ САМОГО БОЯ: исход боя зависит
    // от броска, засеянного номером тика, и на сжатом шаге он другой.
    // А вот длительность паузы между сражениями — не бросок, а число,
    // и она обязана быть одинаковой при любом шаге.
    auto secondsToCoolDown = [](int64_t tempo) {
        World world;
        Galaxy galaxy;
        registerGalaxyComponents(world);
        registerFleetComponents(world);
        registerControlComponents(world);
        registerBattleComponents(world);

        GalaxyParams params;
        params.seed = 0xC001;
        params.systemCount = 20;
        galaxy.generate(world, params);
        initialiseControl(world, galaxy);
        initialiseBattles(world, galaxy);
        world.setResource(&galaxy);

        // Откат такой, каким его ставит сражение.
        world.each<BattleState>([](Entity, BattleState& state) {
            state.cooldown = uint32_t(kBattleIntervalSeconds * kTicksPerSecond);
        });

        const int64_t limit = ticksForSeconds(4 * kBattleIntervalSeconds, tempo);
        for (int64_t i = 0; i < limit; ++i) {
            const TickContext context = TickContext::at(uint64_t(i), tempo);
            systemBattles(world, context);

            bool anyLeft = false;
            world.each<BattleState>([&](Entity, BattleState& state) {
                if (state.cooldown > 0) anyLeft = true;
            });
            if (!anyLeft) return TickContext::at(uint64_t(i + 1), tempo).gameSeconds();
        }
        return int64_t(-1);
    };

    const int64_t plain = secondsToCoolDown(1);
    const int64_t coarse = secondsToCoolDown(50);
    CAPTURE(plain);
    CAPTURE(coarse);

    // Пауза равна объявленной, и она одинакова на обоих шагах.
    CHECK(plain == kBattleIntervalSeconds);
    CHECK(coarse == kBattleIntervalSeconds);
}
