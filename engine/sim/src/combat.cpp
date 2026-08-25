#include "pw/sim/combat.h"

#include <algorithm>

namespace pw::sim {
namespace {

// ---------------------------------------------------------------------------
// Характеристики корпусов
//
// Совпадают со спецификациями из assets/src/hulls.json, по которым Blender
// строит модели: у корвета с двумя оружейными слотами на модели ровно две
// башни. Числа боевой прочности выведены из стоимости — линкор в 24 раза
// дороже корвета и в 20 раз прочнее, поэтому дешёвый массовый флот остаётся
// осмысленным выбором, а не заведомо худшим.
// ---------------------------------------------------------------------------

struct HullStats {
    int64_t hitPoints;
    int64_t weaponSlots;
    int64_t defenseSlots;
    int64_t utilitySlots;
};

// Прочность выбрана по отношению к урону, а не сама по себе.
//
// Первая версия ставила её вчетверо ниже, и любой бой кончался взаимным
// уничтожением за три-четыре раунда. Последствия шли дальше, чем «бои
// слишком быстрые»:
//
//   — энергетическое оружие не успевало выстрелить НИ РАЗУ: сближение
//     наступает к пятому раунду, а к нему уже все мертвы. Треть
//     контр-системы просто не работала;
//   — ракеты выигрывали у всего, потому что стреляют с любой дистанции.
//     Камень-ножницы-бумага вырождались в один доминирующий билд;
//   — состав флота переставал что-либо решать: обе стороны гибли целиком
//     в любом случае, и разведка не окупалась.
//
// То есть неверное соотношение двух чисел обнуляло главную скилловую ось
// игры. Тесты на свойства контр-системы это поймали, тесты на отдельные
// формулы — не поймали бы.
constexpr HullStats kHulls[] = {
    {},                    // Hull::None
    {200, 2, 1, 1},        // корвет
    {600, 4, 2, 2},        // эсминец
    {1600, 6, 4, 3},       // крейсер
    {4000, 10, 6, 4},      // линкор
};

// --- урон за оружейный слот за раунд -------------------------------------
//
// Числа выровнены по СУММАРНОМУ урону за бой, а не по урону за раунд.
// Ракеты стреляют все четырнадцать раундов, кинетика двенадцать, энергия
// только девять — она вступает после сближения. Поэтому за раунд энергия
// обязана бить заметно больнее, иначе она проигрывает структурно.
//
// Ожидаемый суммарный урон за полный бой выровнен: ракеты 168, кинетика 252,
// энергия 252. У ракет он ниже намеренно — они единственные, кому не мешают
// щиты, и без этой скидки ракетный билд обыгрывал бы оба остальных.
//
// Первая версия ставила энергии 18 против 15 у ракет, и энергетический билд
// проигрывал ракетному в ста процентах боёв. Треть контр-системы была
// нежизнеспособна.
constexpr int64_t kDamageKinetic = 21;
constexpr int64_t kDamageEnergy = 28;
constexpr int64_t kDamageMissile = 12;

/// Снижение урона за один защитный слот.
///
/// Первая версия ставила 4%, и у корвета с одним защитным слотом выбор между
/// щитами и бронёй менял исход на четыре процента — то есть не менял ничего.
/// Контр-система работала только на линкорах с шестью слотами, а она обязана
/// работать на всех классах: иначе лёгкий флот перестаёт быть выбором.
inline const fx kMitigationPerSlot = fx::fromFraction(3, 20);   // 15%
/// Потолок снижения: полностью укрыться нельзя ни при каком билде.
inline const fx kMitigationCap = fx::fromFraction(18, 25);      // 72%
/// Сбитие ракет за один слот ПРО.
inline const fx kPointDefensePerSlot = fx::fromFraction(11, 50); // 22%
inline const fx kPointDefenseCap = fx::fromFraction(17, 20);     // 85%

/// Разброс урона за раунд, доля. Небольшой намеренно: он добавляет боям
/// характер, но не должен перебивать контр-систему — иначе разведка
/// перестанет окупаться, а вместе с ней исчезнет и скилл.
inline const fx kRoundVariance = fx::fromFraction(2, 25);        // ±8%

/// На сколько раундов доктрина откладывает сближение.
int64_t closingDelay(Doctrine doctrine) {
    switch (doctrine) {
        case Doctrine::Envelop: return 1;   // заходит по флангам
        case Doctrine::Evasive: return 3;   // держится на расстоянии
        default:                return 0;   // линия идёт прямо
    }
}

/// Множитель наносимого урона от доктрины.
fx damageScale(Doctrine doctrine) {
    switch (doctrine) {
        case Doctrine::Line:    return fx::fromFraction(23, 20);  // 1.15
        case Doctrine::Envelop: return fx::fromFraction(11, 10);  // 1.10
        default:                return fx::fromFraction(17, 20);  // 0.85
    }
}

/// Множитель получаемого урона от доктрины.
fx incomingScale(Doctrine doctrine) {
    return doctrine == Doctrine::Evasive ? fx::fromFraction(3, 4) : fx::one();
}

/// Насколько защита данного типа помогает против данного оружия.
/// Здесь и живёт контр-система, всё остальное — обвязка.
struct Counters {
    fx shields;
    fx armour;
};

// Разброс между «своей» и «чужой» защитой намеренно большой: попасть
// под свою контру должно быть больно, иначе разведка не окупается.
Counters countersKinetic()  { return {fx::fromInt(2), fx::fromFraction(3, 10)}; }
Counters countersEnergy()   { return {fx::fromFraction(3, 10), fx::fromInt(2)}; }
/// Ракеты игнорируют щиты полностью: против них помогает только броня и ПРО.
///
/// Броня против ракет работает СИЛЬНЕЕ, чем против кинетики, и это не
/// произвол. Щиты есть почти в любом билде, а против ракет они бесполезны —
/// значит ракеты по умолчанию проходили бы куда лучше остальных типов
/// оружия и били бы всех. Первая версия ставила коэффициент 1.2, и ракетный
/// билд выигрывал и у кинетического, и у энергетического: камень-ножницы-
/// бумага вырождались в один доминирующий выбор, а с ним пропадал смысл
/// разведки.
Counters countersMissile()  { return {fx::zero(), fx::fromFraction(9, 5)}; }

fx share(uint8_t percent) { return fx::fromFraction(int64_t(percent), 100); }

/// Нормировать доли: игрок или бот может задать сумму, не равную сотне.
void normalise(fx& a, fx& b, fx& c) {
    const fx total = a + b + c;
    if (total <= fx::zero()) {
        a = b = c = fx::fromFraction(1, 3);
        return;
    }
    a = a / total;
    b = b / total;
    c = c / total;
}

void normalise(fx& a, fx& b) {
    const fx total = a + b;
    if (total <= fx::zero()) {
        a = b = fx::fromFraction(1, 2);
        return;
    }
    a = a / total;
    b = b / total;
}

/// Свёрнутые характеристики стороны.
struct Aggregate {
    fx hitPoints;
    fx weaponSlots;
    fx defensePerShip;
    fx utilityPerShip;
    fx kinetic, energy, missile;
    fx shields, armour;
    fx pointDefense;
};

Aggregate summarise(const BattleSide& side) {
    Aggregate out{};
    int64_t ships = 0, defense = 0, utility = 0;

    const uint32_t counts[] = {0, side.fleet.corvettes, side.fleet.destroyers,
                               side.fleet.cruisers, side.fleet.battleships};
    for (int hull = 1; hull < int(Hull::Count); ++hull) {
        const int64_t n = int64_t(counts[hull]);
        if (n == 0) continue;
        out.hitPoints += fx::fromInt(n * kHulls[hull].hitPoints);
        out.weaponSlots += fx::fromInt(n * kHulls[hull].weaponSlots);
        defense += n * kHulls[hull].defenseSlots;
        utility += n * kHulls[hull].utilitySlots;
        ships += n;
    }
    if (ships > 0) {
        // Защита — свойство корабля, а не размера флота: тысяча корветов
        // не становится прочнее одного корвета в пересчёте на корабль.
        out.defensePerShip = fx::fromFraction(defense, ships);
        out.utilityPerShip = fx::fromFraction(utility, ships);
    }

    out.kinetic = share(side.armament.kinetic);
    out.energy = share(side.armament.energy);
    out.missile = share(side.armament.missile);
    normalise(out.kinetic, out.energy, out.missile);

    out.shields = share(side.armament.shields);
    out.armour = share(side.armament.armour);
    normalise(out.shields, out.armour);

    out.pointDefense = share(side.armament.pointDefense);
    return out;
}

/// Доля урона, доходящая до цели после защиты.
fx throughput(const Aggregate& target, const Counters& counters) {
    fx mitigation = target.defensePerShip * kMitigationPerSlot *
                    (target.shields * counters.shields + target.armour * counters.armour);
    mitigation = clamp(mitigation, fx::zero(), kMitigationCap);
    return fx::one() - mitigation;
}

/// Урон стороны за раунд с учётом дистанции, защиты цели и доктрин.
fx roundDamage(const Aggregate& attacker, const Aggregate& target,
               Doctrine attackerDoctrine, Doctrine targetDoctrine,
               int band, fx attackerStrength, Rng& rng) {
    // band: 0 — дальняя (только ракеты), 1 — средняя (плюс кинетика),
    //       2 — ближняя (всё оружие).
    fx damage = fx::zero();

    // Ракеты стреляют с любой дистанции — тем и ценны.
    {
        const fx pd = clamp(target.utilityPerShip * target.pointDefense * kPointDefensePerSlot,
                            fx::zero(), kPointDefenseCap);
        damage += attacker.weaponSlots * attacker.missile *
                  fx::fromInt(kDamageMissile) *
                  throughput(target, countersMissile()) * (fx::one() - pd);
    }
    if (band >= 1) {
        damage += attacker.weaponSlots * attacker.kinetic *
                  fx::fromInt(kDamageKinetic) * throughput(target, countersKinetic());
    }
    if (band >= 2) {
        damage += attacker.weaponSlots * attacker.energy *
                  fx::fromInt(kDamageEnergy) * throughput(target, countersEnergy());
    }

    // Огневая мощь падает вместе с флотом: подбитые корабли не стреляют.
    //
    // Без этого флот с десятью процентами прочности бил бы так же, как целый,
    // и бой превращался бы в обмен уроном по начальным составам. С этим
    // появляется обратная связь: кто лучше держит удар, тот дольше стреляет
    // и выигрывает с большим отрывом — то есть контр-система начинает решать
    // не только «кто победит», но и «сколько от него останется».
    damage *= attackerStrength;

    damage *= damageScale(attackerDoctrine);
    damage *= incomingScale(targetDoctrine);

    // Разброс раунда. Свой поток случайности, чтобы порядок вызовов
    // в других подсистемах не влиял на исход боя.
    const fx roll = rng.unit() * (kRoundVariance * fx::fromInt(2)) - kRoundVariance;
    damage *= (fx::one() + roll);
    return max(fx::zero(), damage);
}

Fleet difference(const Fleet& before, const Fleet& after) {
    return Fleet{before.corvettes - after.corvettes, before.destroyers - after.destroyers,
                 before.cruisers - after.cruisers, before.battleships - after.battleships};
}

}  // namespace

FleetArmament balancedArmament() {
    FleetArmament armament{};
    armament.kinetic = 34;
    armament.energy = 33;
    armament.missile = 33;
    armament.pointDefense = 50;
    armament.shields = 50;
    armament.armour = 50;
    armament.doctrine = uint8_t(Doctrine::Line);
    return armament;
}

int64_t fleetHitPoints(const Fleet& fleet) {
    const uint32_t counts[] = {0, fleet.corvettes, fleet.destroyers,
                               fleet.cruisers, fleet.battleships};
    int64_t total = 0;
    for (int hull = 1; hull < int(Hull::Count); ++hull) {
        total += int64_t(counts[hull]) * kHulls[hull].hitPoints;
    }
    return total;
}

/// Пересчитать выживших: потерянная прочность тратится на корабли, начиная
/// с самых дешёвых.
///
/// Первая версия умножала на долю КОЛИЧЕСТВО каждого класса и округляла вниз.
/// Это выглядело безобидно и ломало игру:
///
///   — единственный корабль класса погибал от любого урона. Флот 4/3/1/0
///     получил 136 урона из 4200 (3%) — и потерял корвет, эсминец И
///     единственный крейсер, то есть 57% тоннажа. Крупные корпуса в
///     смешанном флоте становились бессмысленной тратой сплавов;
///   — потери не сходились с уроном: снятый тоннаж не имел отношения к тому,
///     сколько прочности выбил противник. Обмен переставал быть честным
///     в обе стороны — сильный флот терял слишком много, слабый слишком мало.
///
/// Теперь потери — это ровно выбитая прочность, и тратится она на эскорт
/// раньше, чем на крупные корпуса: лёгкие корабли для того во флоте и стоят.
/// Ошибка округления снимается по правилу «половина корпуса — уже потеря»,
/// иначе одинокий линкор пережил бы что угодно, кроме полного уничтожения.
Fleet survivors(const Fleet& initial, fx fraction) {
    fraction = clamp(fraction, fx::zero(), fx::one());

    Fleet out = initial;
    uint32_t* counts[] = {&out.corvettes, &out.destroyers, &out.cruisers, &out.battleships};

    int64_t total = 0;
    for (int hull = 1; hull < int(Hull::Count); ++hull) {
        total += int64_t(*counts[hull - 1]) * kHulls[hull].hitPoints;
    }
    if (total == 0) return out;

    // Выбитая прочность. Считаем через долю выживших, чтобы ноль означал
    // именно полное уничтожение, без ошибок округления в последнем корабле.
    const fx survivingPoints = fx::fromInt(total) * fraction;
    int64_t lost = total - survivingPoints.floorToInt();
    if (lost <= 0) return out;

    for (int hull = 1; hull < int(Hull::Count); ++hull) {
        const int64_t hp = kHulls[hull].hitPoints;
        uint32_t& count = *counts[hull - 1];
        while (count > 0 && lost >= hp) {
            lost -= hp;
            --count;
        }
        // Остаток меньше корпуса: добираем ближайшим округлением, но только
        // если в этом классе ещё есть кого снимать.
        if (count > 0 && lost > 0 && lost * 2 >= hp) {
            lost -= hp;
            --count;
        }
        if (lost <= 0) break;
    }
    return out;
}


uint32_t battleStrength(const Fleet& fleet, const FleetArmament&) {
    const uint32_t counts[] = {0, fleet.corvettes, fleet.destroyers,
                               fleet.cruisers, fleet.battleships};
    int64_t total = 0;
    for (int hull = 1; hull < int(Hull::Count); ++hull) {
        total += int64_t(counts[hull]) * (kHulls[hull].hitPoints + kHulls[hull].weaponSlots * 40);
    }
    return uint32_t(total / 100);
}

BattleResult resolveBattle(const BattleSide& a, const BattleSide& b, Rng& rng) {
    BattleResult result{};
    if (fleetEmpty(a.fleet) && fleetEmpty(b.fleet)) return result;
    if (fleetEmpty(a.fleet)) {
        result.outcome = 1;
        return result;
    }
    if (fleetEmpty(b.fleet)) {
        result.outcome = 0;
        return result;
    }

    const Aggregate summaryA = summarise(a);
    const Aggregate summaryB = summarise(b);

    fx healthA = summaryA.hitPoints;
    fx healthB = summaryB.hitPoints;

    // Сближение задаёт та сторона, которая тянет дольше: уклоняющийся флот
    // дольше держит дистанцию, и его противник дольше остаётся под ракетами.
    const int64_t delay = std::max(closingDelay(a.doctrine), closingDelay(b.doctrine));
    const int64_t mediumAt = 2 + delay;
    const int64_t closeAt = 5 + delay;

    for (uint32_t round = 0; round < kMaxBattleRounds; ++round) {
        ++result.rounds;

        const int band = int64_t(round) >= closeAt ? 2 : (int64_t(round) >= mediumAt ? 1 : 0);

        // Обе стороны стреляют по состоянию НА НАЧАЛО раунда. Иначе тот, чей
        // залп считается первым, получал бы преимущество из ниоткуда —
        // и исход зависел бы от порядка аргументов функции.
        const fx strengthA = clamp(healthA / summaryA.hitPoints, fx::zero(), fx::one());
        const fx strengthB = clamp(healthB / summaryB.hitPoints, fx::zero(), fx::one());

        const fx damageToB =
            roundDamage(summaryA, summaryB, a.doctrine, b.doctrine, band, strengthA, rng);
        const fx damageToA =
            roundDamage(summaryB, summaryA, b.doctrine, a.doctrine, band, strengthB, rng);

        healthA -= damageToA;
        healthB -= damageToB;

        if (healthA <= fx::zero() || healthB <= fx::zero()) break;
    }

    const fx fractionA = healthA <= fx::zero() ? fx::zero() : healthA / summaryA.hitPoints;
    const fx fractionB = healthB <= fx::zero() ? fx::zero() : healthB / summaryB.hitPoints;

    const Fleet leftA = survivors(a.fleet, fractionA);
    const Fleet leftB = survivors(b.fleet, fractionB);
    result.lossesA = difference(a.fleet, leftA);
    result.lossesB = difference(b.fleet, leftB);

    const bool deadA = fleetEmpty(leftA);
    const bool deadB = fleetEmpty(leftB);

    if (deadA && deadB) {
        result.outcome = 2;
    } else if (deadB) {
        result.outcome = 0;
    } else if (deadA) {
        result.outcome = 1;
    } else {
        // Победа по ОТНОСИТЕЛЬНЫМ ПОТЕРЯМ, а не по полному истреблению.
        //
        // Флоты не дерутся до последнего корабля: командир, потерявший
        // заметно больше противника, выходит из боя. Без этого правила бой
        // двух крупных флотов заканчивался бы ничьёй всегда — за отведённые
        // раунды никто никого не добивает, — и состав флота переставал бы
        // влиять на исход вообще.
        const fx lossA = fx::one() - fractionA;
        const fx lossB = fx::one() - fractionB;
        const fx margin = fx::fromFraction(1, 20);  // 5 процентов
        if (lossA > lossB + margin)      result.outcome = 1;
        else if (lossB > lossA + margin) result.outcome = 0;
        else                             result.outcome = 2;  // разошлись вничью
    }

    return result;
}

}  // namespace pw::sim
