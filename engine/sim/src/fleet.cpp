#include "pw/sim/fleet.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "pw/sim/combat.h"
#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/galaxy.h"

namespace pw::sim {

// ---------------------------------------------------------------------------
// Стойка: что отряд делает, когда делать нечего
// ---------------------------------------------------------------------------

void systemFleetStance(World& world, const TickContext&) {
    // Идёт ПОСЛЕ присутствия: уклонение смотрит, кто ещё стоит в системе,
    // а присутствие пересобирается каждый тик по итогам движения. Решение,
    // принятое здесь, вступает в силу движением следующего тика — то есть
    // через одну десятую игровой секунды. Ждать этого никто не заметит.
    const Presence* presence = world.resource<Presence>();

    world.each<Fleet, FleetLocation, FleetOrders, Owner>(
        [&](Entity, Fleet& fleet, FleetLocation& location, FleetOrders& orders,
            Owner& owner) {
            // В пути ничего не решаем: отряд между узлами не может ни
            // уклониться, ни вернуться — он уже в движении.
            if (location.system != location.nextSystem) return;
            if (fleetEmpty(fleet)) return;

            // --- уклонение ---
            //
            // Единственная механика во всей игре, которая работает, пока
            // игрок спит. Разведчик, конвой, эскорт колониста живут ровно
            // до первой встречи с линейным флотом; уклонение превращает
            // потерю в отход.
            if (orders.evade != 0 && presence != nullptr &&
                location.system < presence->size() &&
                location.system != orders.anchor) {
                const Presence::Entry& here = presence->at(location.system);
                uint32_t enemy = 0;
                const uint32_t attacker = here.strongestOther(owner.empire, enemy);
                const uint32_t mine = here.tonnageOf(owner.empire);
                // Свои считаются ВСЕ, кто в системе: союзный флот рядом —
                // это ровно та причина, по которой отходить не надо.
                if (attacker != kNoEmpire &&
                    uint64_t(enemy) * kEvadeDenominator >
                        uint64_t(mine) * kEvadeNumerator) {
                    // Уклонение НЕ меняет стойку и не трогает приписку:
                    // это исполнение решения, принятого игроком заранее,
                    // а не новое решение.
                    setRoute(orders, orders.anchor);
                    return;
                }
            }

            // --- возврат домой ---
            if (orders.routed()) return;
            if (orders.stance != uint8_t(Stance::Guard)) return;
            if (orders.anchor == kNoSystem || orders.anchor == location.system) return;
            setRoute(orders, orders.anchor);
        });
}

void registerFleetComponents(World& world) {
    world.registerComponent<Fleet>("Fleet");
    world.registerComponent<FleetLocation>("FleetLocation");
    world.registerComponent<FleetOrders>("FleetOrders");
}

namespace {

/// Всё, что отличает один корпус от другого, — одной таблицей.
///
/// Раньше цена жила в switch, скорость в цепочке if, тоннаж в формуле,
/// а осадная мощь не жила нигде. Добавить корпус означало вспомнить
/// про четыре места; забыть одно — получить корабль, который ничего
/// не весит или летает с нулевой скоростью. Таблица делает пропуск
/// невозможным: у строки либо есть все поля, либо она не компилируется.
struct HullSpec {
    uint32_t cost;      // сплавов
    fx speed;           // игровых единиц в секунду
    uint32_t tonnage;   // вес в очках престижа и в оценке сил
    uint32_t siege;     // вклад в штурм планеты
};

const HullSpec& specOf(Hull hull) {
    static const HullSpec table[kHullClasses] = {
        /* корвет  */ {kCostCorvette,   kSpeedCorvette,   1,  0},
        /* тендер  */ {kCostTender,     kSpeedTender,     2,  0},
        /* колониз.*/ {kCostColonizer,  kSpeedColonizer,  2,  0},
        /* эсминец */ {kCostDestroyer,  kSpeedDestroyer,  3,  1},
        /* носитель*/ {kCostCarrier,    kSpeedCarrier,    7,  2},
        /* крейсер */ {kCostCruiser,    kSpeedCruiser,    8,  3},
        /* монитор */ {kCostMonitor,    kSpeedMonitor,   10, 16},
        /* линкор  */ {kCostBattleship, kSpeedBattleship, 20, 6},
        /* титан   */ {kCostTitan,      kSpeedTitan,     70, 24},
    };
    static const HullSpec none{};
    if (hull == Hull::None || hull >= Hull::Count) return none;
    return table[size_t(hull) - 1];
}

}  // namespace

fx hullSpeed(Hull hull) { return specOf(hull).speed; }

fx fleetSpeed(const Fleet& fleet) {
    // Скорость флота задаёт самый медленный ПРИСУТСТВУЮЩИЙ корабль.
    // Перебираем все классы и берём минимум: цепочка if от медленного
    // к быстрому требовала помнить порядок скоростей, а он не совпадает
    // с порядком цен — монитор дешевле линкора и медленнее его.
    fx slowest = fx::zero();
    for (size_t index = 0; index < kHullClasses; ++index) {
        if (fleet.ships[index] == 0) continue;
        const fx speed = specOf(Hull(index + 1)).speed;
        if (slowest <= fx::zero() || speed < slowest) slowest = speed;
    }
    return slowest;
}

uint32_t fleetTonnage(const Fleet& fleet) {
    // Тоннаж растёт быстрее числа слотов: линкор дороже четырёх корветов
    // и в производстве, и в потерях.
    uint32_t total = 0;
    for (size_t index = 0; index < kHullClasses; ++index) {
        total += fleet.ships[index] * specOf(Hull(index + 1)).tonnage;
    }
    return total;
}

uint32_t fleetSiegePower(const Fleet& fleet) {
    uint32_t total = 0;
    for (size_t index = 0; index < kHullClasses; ++index) {
        total += fleet.ships[index] * specOf(Hull(index + 1)).siege;
    }
    return total;
}

fx fleetDamageControl(const Fleet& fleet) {
    uint32_t tenders = fleet[Hull::Tender];
    if (tenders == 0) return fx::zero();

    uint32_t ships = 0;
    for (size_t index = 0; index < kHullClasses; ++index) ships += fleet.ships[index];
    if (ships == 0) return fx::zero();

    // Один тендер на десять кораблей снимает десятую часть урона. Дальше
    // отдача падает: потолок в 35% выбран так, чтобы флот из одних тендеров
    // (которые не стреляют) всё равно проигрывал флоту с пушками.
    const fx share = fx::fromFraction(int64_t(tenders), int64_t(ships));
    return min(share, fx::fromFraction(7, 20));
}

uint32_t hullCost(Hull hull) { return specOf(hull).cost; }

void fleetAdd(Fleet& fleet, Hull hull, uint32_t count) {
    if (hull == Hull::None || hull >= Hull::Count) return;
    fleet[hull] += count;
}

uint32_t fleetCost(const Fleet& fleet) {
    uint32_t total = 0;
    for (size_t index = 0; index < kHullClasses; ++index) {
        total += fleet.ships[index] * specOf(Hull(index + 1)).cost;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Маршрут
// ---------------------------------------------------------------------------

const char* stanceName(Stance stance) {
    switch (stance) {
        case Stance::Reserve: return "резерв";
        case Stance::Hold:    return "стоять";
        case Stance::Guard:   return "охранять";
        case Stance::Patrol:  return "патруль";
        case Stance::Count:   break;
    }
    return "?";
}

const char* stanceHint(Stance stance) {
    switch (stance) {
        case Stance::Reserve:
            return "общий пул: сливается с другими нетронутыми отрядами "
                   "в той же системе · любой приказ выводит из резерва";
        case Stance::Hold:
            return "стоит там, где встал · сам никуда не уходит "
                   "и ни с кем не сливается";
        case Stance::Guard:
            return "когда маршрут кончился — возвращается к приписке "
                   "и стоит у неё · так гарнизон не уходит вслед за рейдером";
        case Stance::Patrol:
            return "идёт по маршруту по кругу без конца · маршрут "
                   "из одной точки патрулем не станет";
        case Stance::Count:
            break;
    }
    return "";
}

void clearRoute(FleetOrders& orders) {
    for (uint8_t i = 0; i < FleetOrders::kMaxRoute; ++i) orders.route[i] = kNoSystem;
    orders.count = 0;
    orders.step = 0;
}

void setRoute(FleetOrders& orders, uint32_t system) {
    clearRoute(orders);
    if (system == kNoSystem) return;
    orders.route[0] = system;
    orders.count = 1;
}

bool appendRoute(FleetOrders& orders, uint32_t system) {
    if (system == kNoSystem) return false;
    if (orders.count >= FleetOrders::kMaxRoute) return false;

    // Точки, стоящие подряд одинаково, не добавляются: два щелчка по одной
    // системе — это один и тот же приказ, и превращать их в две точки
    // маршрута значит наказывать за дрожащую руку.
    if (orders.count > 0 && orders.route[orders.count - 1] == system) return true;

    orders.route[orders.count] = system;
    ++orders.count;
    return true;
}

void commandGiven(FleetOrders& orders) {
    if (orders.stance == uint8_t(Stance::Reserve)) orders.stance = uint8_t(Stance::Hold);
}

void systemFleetMovement(World& world, const TickContext& context) {
    const Galaxy* galaxy = world.resource<Galaxy>();
    if (galaxy == nullptr) return;

    // Дойдя до конца маршрута, отряд либо встаёт, либо начинает его заново.
    //
    // Патруль — это ровно «маршрут не кончается», а не отдельная механика:
    // одна ветка вместо второго вида приказа. Из одной точки патруля
    // не выходит намеренно — ходить по кругу из одной системы в неё же
    // означало бы стоять, но с видом занятого.
    auto advance = [](FleetOrders& orders) {
        ++orders.step;
        if (orders.step < orders.count) return;
        if (orders.stance == uint8_t(Stance::Patrol) && orders.count > 1) {
            orders.step = 0;
            return;
        }
        clearRoute(orders);
    };

    world.each<Fleet, FleetLocation, FleetOrders>(
        [&](Entity, Fleet& fleet, FleetLocation& location, FleetOrders& order) {
            const bool moving = location.system != location.nextSystem;

            if (!moving) {
                // Стоим. Есть ли куда идти?
                if (!order.routed()) {
                    clearRoute(order);
                    return;
                }
                if (order.target() == location.system) {
                    // Уже здесь: точка маршрута пройдена, не сходя с места.
                    advance(order);
                    return;
                }
                const int32_t hop = galaxy->nextHop(location.system, order.target());
                if (hop < 0) {
                    // Пути нет — цель недостижима. Маршрут снимаем, а не
                    // оставляем висеть: иначе флот будет молча стоять,
                    // и игрок не поймёт, почему.
                    clearRoute(order);
                    return;
                }
                location.nextSystem = uint32_t(hop);
                location.progress = fx::zero();
                return;
            }

            // В пути. Доля линии за тик = скорость * шаг / длина линии.
            const fx length = galaxy->laneLength(location.system, location.nextSystem);
            if (length <= fx::zero()) {
                // Линии больше нет: карта изменилась под флотом. Возвращаем
                // его в исходную систему, оттуда он пересчитает маршрут.
                location.nextSystem = location.system;
                location.progress = fx::zero();
                return;
            }

            const fx speed = fleetSpeed(fleet);
            if (speed <= fx::zero()) return;  // пустой флот никуда не идёт

            location.progress += (speed * context.delta) / length;

            if (location.progress < fx::one()) return;

            // Прибыли. Дробный остаток не переносим на следующую линию:
            // узел — это точка принятия решения, и флот в ней всегда
            // оказывается ровно.
            location.system = location.nextSystem;
            location.progress = fx::zero();

            if (location.system == order.target()) {
                advance(order);           // точка взята
                if (!order.routed()) return;
            }
            const int32_t hop = galaxy->nextHop(location.system, order.target());
            if (hop < 0) {
                clearRoute(order);
            } else {
                location.nextSystem = uint32_t(hop);
            }
        });
}

const char* splitRefusalText(SplitRefusal refusal) {
    switch (refusal) {
        case SplitRefusal::Ok:         return "можно выделить";
        case SplitRefusal::NotYours:   return "это не ваш флот";
        case SplitRefusal::InTransit:  return "флот в пути — перестроиться нельзя";
        case SplitRefusal::NotEnough:  return "столько кораблей этого класса нет";
        case SplitRefusal::WholeFleet: return "это весь флот — выделять не из чего";
        case SplitRefusal::Count:      break;
    }
    return "нельзя";
}

SplitRefusal splitCheck(const Fleet& composition, const FleetLocation& location,
                        const Fleet& take) {
    if (location.nextSystem != location.system) return SplitRefusal::InTransit;

    uint32_t taking = 0;
    uint32_t total = 0;
    for (size_t index = 0; index < kHullClasses; ++index) {
        if (take.ships[index] > composition.ships[index]) return SplitRefusal::NotEnough;
        taking += take.ships[index];
        total += composition.ships[index];
    }
    if (taking == 0) return SplitRefusal::NotEnough;

    // Выделить всё до последнего корабля — это не выделение: исходный флот
    // опустеет и будет распущен, а новый займёт его место. Игрок получит
    // тот же флот с новым номером и решит, что игра его обманула.
    if (taking >= total) return SplitRefusal::WholeFleet;
    return SplitRefusal::Ok;
}

Fleet applySplit(Fleet& composition, const Fleet& take) {
    Fleet taken{};
    for (size_t index = 0; index < kHullClasses; ++index) {
        const uint32_t moved = std::min(take.ships[index], composition.ships[index]);
        composition.ships[index] -= moved;
        taken.ships[index] = moved;
    }
    return taken;
}

SplitRefusal splitCheck(const Fleet& composition, const FleetLocation& location,
                        Hull hull, uint32_t count) {
    if (hull == Hull::None || hull >= Hull::Count) return SplitRefusal::NotEnough;
    Fleet take{};
    take[hull] = count;
    return splitCheck(composition, location, take);
}

Fleet applySplit(Fleet& composition, Hull hull, uint32_t count) {
    if (hull == Hull::None || hull >= Hull::Count) return Fleet{};
    Fleet take{};
    take[hull] = count;
    return applySplit(composition, take);
}

Fleet fleetHalf(const Fleet& fleet) {
    Fleet half{};
    for (size_t index = 0; index < kHullClasses; ++index) {
        // Вниз, а не вверх: «половина» не должна уносить последний корабль
        // редкого класса — один титан из одного обязан остаться на месте.
        half.ships[index] = fleet.ships[index] / 2;
    }
    return half;
}

Fleet fleetOnly(const Fleet& fleet, Hull hull) {
    Fleet only{};
    if (hull == Hull::None || hull >= Hull::Count) return only;
    only[hull] = fleet[hull];
    return only;
}

// ---------------------------------------------------------------------------
// Слияние
// ---------------------------------------------------------------------------

const char* mergeRefusalText(MergeRefusal refusal) {
    switch (refusal) {
        case MergeRefusal::Ok:        return "можно слить";
        case MergeRefusal::NotYours:  return "это не ваш флот";
        case MergeRefusal::Apart:     return "отряды в разных системах";
        case MergeRefusal::InTransit: return "отряд в пути — слиться нельзя";
        case MergeRefusal::SameFleet: return "это один и тот же отряд";
        case MergeRefusal::Count:     break;
    }
    return "нельзя";
}

MergeRefusal mergeCheck(uint32_t intoEmpire, const FleetLocation& into,
                        uint32_t fromEmpire, const FleetLocation& from, bool sameEntity) {
    if (sameEntity) return MergeRefusal::SameFleet;
    if (intoEmpire != fromEmpire) return MergeRefusal::NotYours;
    if (into.system != into.nextSystem || from.system != from.nextSystem) {
        return MergeRefusal::InTransit;
    }
    if (into.system != from.system) return MergeRefusal::Apart;
    return MergeRefusal::Ok;
}

void applyMerge(Fleet& into, Fleet& from) {
    for (size_t index = 0; index < kHullClasses; ++index) {
        into.ships[index] += from.ships[index];
        from.ships[index] = 0;
    }
}

void systemFleetStation(World& world, const TickContext&) {
    // Стоящий флот встаёт на орбиту планеты, а не висит «в системе вообще».
    //
    // Правило выбора простое и оттого предсказуемое: САМАЯ БЛИЖНЯЯ К ЗВЕЗДЕ
    // СВОЯ планета, а если своих нет — первая попавшаяся. Предсказуемость
    // здесь важнее «умности»: игрок должен уметь сказать, где окажется
    // флот, ещё до того, как отдаст приказ. Умный выбор, который игрок
    // не может повторить в голове, ощущается как чужая воля.
    //
    // Раз выбранную орбиту флот не меняет, пока стоит: перепрыгивание
    // между планетами при каждом захвате соседа выглядело бы как дёрганье.
    const Galaxy* galaxy = world.resource<Galaxy>();
    if (galaxy == nullptr) return;

    world.each<FleetLocation, Owner>([&](Entity entity, FleetLocation& location,
                                         Owner& owner) {
        if (location.system != location.nextSystem) {
            // В пути орбиты нет. Флот между звёздами не стоит ни у чего,
            // и показывать его у планеты значило бы врать.
            location.orbit = kNoOrbit;
            return;
        }
        if (location.system >= galaxy->systemCount()) return;

        const uint32_t planets = galaxy->planetCount(location.system);
        if (planets == 0) {
            location.orbit = kNoOrbit;
            return;
        }
        // ПРИПИСКА СИЛЬНЕЕ ПРИВЫЧКИ. Отряд, приписанный к планете, стоя
        // в её системе занимает именно её орбиту — даже если уже стоял
        // у соседней. Иначе «привязать флот к планете» означало бы
        // «привязать, если он не встал раньше», а игрок читает приписку
        // как обещание.
        const FleetOrders* orders = world.get<FleetOrders>(entity);
        if (orders != nullptr && orders->anchor == location.system &&
            orders->anchorOrbit < planets) {
            location.orbit = orders->anchorOrbit;
            return;
        }

        // Уже стоит на существующей орбите — не трогаем.
        if (location.orbit < planets) return;

        uint32_t chosen = 0;
        for (uint32_t orbit = 0; orbit < planets; ++orbit) {
            const Entity planet = galaxy->planetEntity(location.system, orbit);
            if (!planet.valid()) continue;
            const Owner* planetOwner = world.get<Owner>(planet);
            if (planetOwner != nullptr && planetOwner->empire == owner.empire) {
                chosen = orbit;
                break;
            }
        }
        location.orbit = chosen;
    });
}

namespace {

/// Ключ слияния: одна империя, одна система, одинаковое вооружение.
struct MergeKey {
    uint32_t system;
    uint32_t empire;
    uint8_t armament[8];

    bool operator<(const MergeKey& other) const {
        if (system != other.system) return system < other.system;
        if (empire != other.empire) return empire < other.empire;
        return std::memcmp(armament, other.armament, sizeof(armament)) < 0;
    }
    bool operator==(const MergeKey& other) const {
        return system == other.system && empire == other.empire &&
               std::memcmp(armament, other.armament, sizeof(armament)) == 0;
    }
};

struct MergeCandidate {
    MergeKey key;
    uint32_t entityIndex;
    Entity entity;
    Fleet fleet;
};

}  // namespace

void systemMergeFleets(World& world, const TickContext&) {
    Commands* commands = world.resource<Commands>();
    if (commands == nullptr) return;

    std::vector<MergeCandidate> candidates;
    world.each<Fleet, FleetLocation, FleetOrders, Owner>(
        [&](Entity entity, Fleet& fleet, FleetLocation& location, FleetOrders& order,
            Owner& owner) {
            // Сливаем только стоящие, без маршрута И В РЕЗЕРВЕ.
            //
            // Резерв — это в точности «корабли, которых игрок не трогал».
            // Пока сливался любой стоящий отряд без приказа, два своих
            // отряда нельзя было держать раздельно дома: выделил рейдовую
            // группу — на следующем тике она снова в общей куче.
            if (location.system != location.nextSystem) return;
            if (order.routed()) return;
            if (order.stance != uint8_t(Stance::Reserve)) return;
            if (fleetEmpty(fleet)) return;

            const FleetArmament* armament = world.get<FleetArmament>(entity);
            if (armament == nullptr) return;

            MergeCandidate candidate{};
            candidate.key.system = location.system;
            candidate.key.empire = owner.empire;
            std::memcpy(candidate.key.armament, armament, sizeof(candidate.key.armament));
            candidate.entityIndex = entity.index;
            candidate.entity = entity;
            candidate.fleet = fleet;
            candidates.push_back(candidate);
        });

    // Порядок полный: ключ, затем номер сущности. Принимающим становится
    // флот с наименьшим номером — выбор воспроизводим.
    std::sort(candidates.begin(), candidates.end(),
              [](const MergeCandidate& a, const MergeCandidate& b) {
                  if (!(a.key == b.key)) return a.key < b.key;
                  return a.entityIndex < b.entityIndex;
              });

    size_t index = 0;
    while (index < candidates.size()) {
        size_t end = index;
        while (end + 1 < candidates.size() && candidates[end + 1].key == candidates[index].key) {
            ++end;
        }
        if (end > index) {
            Fleet* into = world.get<Fleet>(candidates[index].entity);
            if (into != nullptr) {
                for (size_t i = index + 1; i <= end; ++i) {
                    for (size_t hull = 0; hull < kHullClasses; ++hull) {
                        into->ships[hull] += candidates[i].fleet.ships[hull];
                    }
                    // Опустошаем сразу: до применения буфера мир не должен
                    // содержать удвоенных кораблей.
                    Fleet* from = world.get<Fleet>(candidates[i].entity);
                    if (from != nullptr) *from = Fleet{};
                    commands->destroy(candidates[i].entity);
                }
            }
        }
        index = end + 1;
    }
}

}  // namespace pw::sim
