#include "doctest.h"

#include "pw/sim/combat.h"

using namespace pw;
using namespace pw::sim;

namespace {

FleetArmament armed(uint8_t kinetic, uint8_t energy, uint8_t missile,
                    uint8_t shields, uint8_t armour, uint8_t pointDefense,
                    Doctrine doctrine = Doctrine::Line) {
    FleetArmament armament{};
    armament.kinetic = kinetic;
    armament.energy = energy;
    armament.missile = missile;
    armament.shields = shields;
    armament.armour = armour;
    armament.pointDefense = pointDefense;
    armament.doctrine = uint8_t(doctrine);
    return armament;
}

BattleSide side(const Fleet& fleet, const FleetArmament& armament) {
    BattleSide out;
    out.fleet = fleet;
    out.armament = armament;
    out.doctrine = Doctrine(armament.doctrine);
    return out;
}

/// Сколько раз из ста прогонов побеждает сторона A.
///
/// Один бой ничего не доказывает: в каждом раунде есть разброс. Свойства
/// контр-системы проверяются на выборке, а не на единичном исходе.
int winRate(const BattleSide& a, const BattleSide& b) {
    int wins = 0;
    for (uint64_t seed = 0; seed < 100; ++seed) {
        Rng rng(seed, /*stream=*/9);
        const BattleResult result = resolveBattle(a, b, rng);
        if (result.outcome == 0) ++wins;
    }
    return wins;
}

/// Средний тоннаж, выбитый из стороны B.
double averageDamageTo(const BattleSide& a, const BattleSide& b) {
    int64_t total = 0;
    for (uint64_t seed = 0; seed < 100; ++seed) {
        Rng rng(seed, 9);
        total += fleetTonnage(resolveBattle(a, b, rng).lossesB);
    }
    return double(total) / 100.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Основы
// ---------------------------------------------------------------------------

TEST_CASE("бой: пустой флот проигрывает не сражаясь") {
    Rng rng(1, 9);
    const auto empty = side(Fleet{0, 0, 0, 0}, balancedArmament());
    const auto real = side(Fleet{10, 0, 0, 0}, balancedArmament());

    CHECK(resolveBattle(empty, real, rng).outcome == 1);
    CHECK(resolveBattle(real, empty, rng).outcome == 0);
    CHECK(resolveBattle(empty, empty, rng).outcome == 2);
}

TEST_CASE("бой: завершается за конечное число раундов") {
    Rng rng(2, 9);
    // Две стены брони с ПРО: самый затяжной случай, какой можно собрать.
    const auto tank = side(Fleet{0, 0, 0, 40}, armed(0, 0, 100, 0, 100, 100));
    const BattleResult result = resolveBattle(tank, tank, rng);
    CHECK(result.rounds <= kMaxBattleRounds);
    CHECK(result.rounds > 0);
}

TEST_CASE("бой: воспроизводим до последнего корабля") {
    const auto a = side(Fleet{30, 10, 4, 1}, armed(50, 30, 20, 60, 40, 30));
    const auto b = side(Fleet{20, 12, 5, 2}, armed(20, 50, 30, 30, 70, 50));

    Rng first(0xBEEF, 9), second(0xBEEF, 9);
    const BattleResult one = resolveBattle(a, b, first);
    const BattleResult two = resolveBattle(a, b, second);

    CHECK(one.outcome == two.outcome);
    CHECK(one.rounds == two.rounds);
    CHECK(one.lossesA.corvettes == two.lossesA.corvettes);
    CHECK(one.lossesB.battleships == two.lossesB.battleships);
}

TEST_CASE("бой: при прочих равных крупный флот побеждает") {
    const auto small = side(Fleet{20, 0, 0, 0}, balancedArmament());
    const auto large = side(Fleet{40, 0, 0, 0}, balancedArmament());
    CHECK(winRate(large, small) > 90);
    CHECK(winRate(small, large) < 10);
}

// ---------------------------------------------------------------------------
// Контр-система — сердце скилловой части игры
// ---------------------------------------------------------------------------

TEST_CASE("контры: против кинетики спасают щиты, а не броня") {
    // Обе стороны одинаковы во всём, кроме защиты. Тогда исход говорит
    // ровно об одном — о работе контры, и ни о чём больше.
    const Fleet same{100, 0, 0, 0};
    const auto shielded = side(same, armed(100, 0, 0, 100, 0, 0));
    const auto armoured = side(same, armed(100, 0, 0, 0, 100, 0));

    // Кинетика вязнет в щитах и режет броню.
    CHECK(winRate(shielded, armoured) > 85);
}

TEST_CASE("контры: против энергии спасает броня, а не щиты") {
    const Fleet same{100, 0, 0, 0};
    const auto shielded = side(same, armed(0, 100, 0, 100, 0, 0));
    const auto armoured = side(same, armed(0, 100, 0, 0, 100, 0));

    CHECK(winRate(armoured, shielded) > 85);
}

TEST_CASE("контры: против ракет щиты бесполезны") {
    const Fleet same{100, 0, 0, 0};
    const auto shielded = side(same, armed(0, 0, 100, 100, 0, 0));
    const auto armoured = side(same, armed(0, 0, 100, 0, 100, 0));

    // Ракеты проходят щиты насквозь, поэтому броня выигрывает вчистую.
    CHECK(winRate(armoured, shielded) > 85);
}

TEST_CASE("контры: ПРО сбивает ракеты") {
    const Fleet same{0, 0, 0, 20};
    const auto naked = side(same, armed(0, 0, 100, 50, 50, 0));
    const auto defended = side(same, armed(0, 0, 100, 50, 50, 100));

    // Линкор с четырьмя слотами ПРО перехватывает почти весь залп.
    CHECK(winRate(defended, naked) > 90);
}

TEST_CASE("контры: ни один билд не бьёт всех") {
    // Камень-ножницы-бумага обязаны замкнуться. Если бы существовал билд,
    // выигрывающий у всех, разведка перестала бы окупаться, а вместе с ней
    // исчезла бы вся скилловая часть игры.
    const Fleet same{0, 0, 10, 0};

    const auto kinetic = side(same, armed(100, 0, 0, 60, 40, 0));
    const auto energy = side(same, armed(0, 100, 0, 40, 60, 0));
    const auto missile = side(same, armed(0, 0, 100, 50, 50, 0));

    struct Build { const char* name; BattleSide build; };
    const Build builds[] = {{"кинетика", kinetic}, {"энергия", energy}, {"ракеты", missile}};

    for (const Build& candidate : builds) {
        int beaten = 0;
        for (const Build& other : builds) {
            if (&candidate == &other) continue;
            if (winRate(candidate.build, other.build) > 50) ++beaten;
        }
        CAPTURE(candidate.name);
        // Ни один билд не должен обыгрывать оба остальных.
        CHECK(beaten < 2);
    }
}

TEST_CASE("контры: знание состава противника перевешивает численность") {
    // ГЛАВНОЕ ОБЕЩАНИЕ ДИЗАЙНА. Меньший флот, собранный под конкретного
    // врага, обязан побеждать больший флот, собранный наугад. Иначе
    // побеждает просто тот, кто накопил больше, и разведка не нужна.
    //
    // Разведка донесла: враг защищён щитами, а бьёт кинетикой и ракетами.
    // Правильный ответ читается однозначно:
    //   его щиты      -> бить ЭНЕРГИЕЙ, она их режет;
    //   его кинетика  -> прикрыться ЩИТАМИ, броня от кинетики не спасает;
    //   его ракеты    -> поставить ПРО, плюс немного брони.
    const auto shieldedHorde = side(Fleet{0, 0, 0, 26}, armed(60, 0, 40, 100, 0, 0));
    const auto counterBuild = side(Fleet{0, 0, 0, 20}, armed(0, 100, 0, 60, 40, 60));

    const int rate = winRate(counterBuild, shieldedHorde);
    CAPTURE(rate);
    // Флот на четверть меньше, но собранный под врага, выигрывает
    // подавляющее большинство боёв.
    CHECK(rate > 80);
}

// ---------------------------------------------------------------------------
// Доктрины
// ---------------------------------------------------------------------------

TEST_CASE("доктрины: уклонение снижает получаемый урон") {
    const auto attacker = side(Fleet{30, 0, 0, 0}, armed(50, 50, 0, 50, 50, 0));
    const auto line = side(Fleet{30, 0, 0, 0},
                           armed(50, 50, 0, 50, 50, 0, Doctrine::Line));
    const auto evasive = side(Fleet{30, 0, 0, 0},
                              armed(50, 50, 0, 50, 50, 0, Doctrine::Evasive));

    CHECK(averageDamageTo(attacker, evasive) < averageDamageTo(attacker, line));
}

TEST_CASE("доктрины: линия бьёт больнее уклонения") {
    const auto target = side(Fleet{30, 0, 0, 0}, armed(50, 50, 0, 50, 50, 0));
    const auto line = side(Fleet{30, 0, 0, 0},
                           armed(50, 50, 0, 50, 50, 0, Doctrine::Line));
    const auto evasive = side(Fleet{30, 0, 0, 0},
                              armed(50, 50, 0, 50, 50, 0, Doctrine::Evasive));

    CHECK(averageDamageTo(line, target) > averageDamageTo(evasive, target));
}

TEST_CASE("доктрины: уклонение противника мешает ближнему бою") {
    // Энергия стреляет только вплотную. Уклоняющийся противник сходится
    // на три раунда дольше, и энергетический флот теряет треть своих залпов.
    //
    // Это плата за выбор доктрины: уклонение защищает, но и лишает
    // собственный ближний бой времени. Ни одна доктрина не бесплатна.
    const auto energy = side(Fleet{40, 0, 0, 0}, armed(0, 100, 0, 50, 50, 0));
    const auto lineTarget = side(Fleet{40, 0, 0, 0},
                                 armed(0, 0, 100, 50, 50, 0, Doctrine::Line));
    const auto evasiveTarget = side(Fleet{40, 0, 0, 0},
                                    armed(0, 0, 100, 50, 50, 0, Doctrine::Evasive));

    // Меряем нанесённый урон, а не победы: против ракетчика энергетический
    // флот в этом раскладе проигрывает в любом случае, но насколько дорого
    // обойдётся его смерть — зависит от того, дали ли ему сойтись.
    CHECK(averageDamageTo(energy, lineTarget) > averageDamageTo(energy, evasiveTarget) * 1.3);
}

// ---------------------------------------------------------------------------
// Оценка сил
// ---------------------------------------------------------------------------

TEST_CASE("оценка: боевая сила растёт с флотом и с классом корпуса") {
    const FleetArmament any = balancedArmament();
    CHECK(battleStrength(Fleet{10, 0, 0, 0}, any) < battleStrength(Fleet{20, 0, 0, 0}, any));
    CHECK(battleStrength(Fleet{20, 0, 0, 0}, any) < battleStrength(Fleet{0, 0, 0, 20}, any));
    CHECK(battleStrength(Fleet{0, 0, 0, 0}, any) == 0u);
}
