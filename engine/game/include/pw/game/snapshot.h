#pragma once

// Снапшот состояния мира.
//
// ПОЧЕМУ ДЕЛЬТЫ, А НЕ ПОЛНОЕ СОСТОЯНИЕ. Двести систем и полторы сотни
// флотов — это около двух с половиной килобайт, а датаграмма ограничена
// 1200 байтами. Полное состояние физически не влезает в пакет уже на
// стартовой галактике, не говоря о больших. Резать его пополам нельзя:
// клиент не должен видеть половину мира из прошлого кадра, а половину
// из этого.
//
// Поэтому схема Quake 3: сервер помнит, что клиент ТОЧНО получил, и шлёт
// разницу. Клиент подтверждает номер снапшота, сервер сдвигает базу.
// В спокойной игре разница почти пуста, и снапшот занимает десятки байт
// вместо тысяч — прямо в бюджет 20 КБ/с из docs/03.
//
// ЧТО ПРОИСХОДИТ ПРИ ПОТЕРЕ. Ничего: снапшот едет ненадёжной частью
// пакета и не пересылается. База не сдвинулась — значит следующий
// снапшот повторит то, что не дошло, и добавит новое. Потеря стоит
// одного кадра задержки, а не рассинхрона.

#include <cstdint>
#include <map>
#include <vector>

#include "pw/net/bitstream.h"
#include "pw/net/sequence.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/world.h"

namespace pw::game {

using namespace pw::net;

/// Состояние системы, каким его видит клиент.
///
/// Всё здесь ПРОИЗВОДНОЕ от планет: владелец — по большинству, готовность —
/// средняя по своим планетам, осада — та, что идёт прямо сейчас. Карта
/// галактики показывает сводку, подробности живут в виде системы.
struct SystemView {
    uint8_t owner = 0xFF;      // kNoEmpire укладывается в 0xFF
    uint8_t readiness = 0;     // средняя готовность своих планет, 0..100
    uint8_t siegeEmpire = 0xFF;
    uint8_t siegeProgress = 0; // 0..100
    /// Сколько планет системы у её владельца и сколько всего. Это то, чем
    /// «наполовину взятая система» отличается на карте от целой: без этих
    /// двух чисел частичный захват не виден вообще.
    uint8_t ownedPlanets = 0;
    uint8_t totalPlanets = 0;
    /// Сколько планет системы НИЧЬИ, то есть годятся под колонизацию.
    ///
    /// Главное число новой игры. Империя начинается с одной планеты
    /// и растёт только высадкой колонистов — значит вопрос «куда лететь»
    /// задаётся постоянно, и без этого числа ответ на него добывался
    /// щелчком по каждой звезде по очереди.
    ///
    /// Отдельно от `totalPlanets - ownedPlanets`: та разность включает
    /// и ЧУЖИЕ планеты, а чужие колонизатором не берут — их берут осадой.
    /// Смешать эти два случая значит послать колониста туда, где его
    /// встретят.
    uint8_t freePlanets = 0;

    bool operator==(const SystemView& o) const {
        return owner == o.owner && readiness == o.readiness &&
               siegeEmpire == o.siegeEmpire && siegeProgress == o.siegeProgress &&
               ownedPlanets == o.ownedPlanets && totalPlanets == o.totalPlanets &&
               freePlanets == o.freePlanets;
    }
    bool operator!=(const SystemView& o) const { return !(*this == o); }
};

/// Флот, каким его видит клиент.
struct FleetView {
    uint32_t id = 0;
    uint8_t empire = 0xFF;
    uint32_t system = 0;
    uint32_t nextSystem = 0;
    /// Возле какой планеты стоит. kNoOrbit — в пути или на общей стоянке.
    uint32_t orbit = sim::kNoOrbit;
    fx progress = fx::zero();
    sim::Fleet composition{};

    // --- приказы: ТОЛЬКО У СВОИХ ОТРЯДОВ ---
    //
    // Маршрут чужого флота — это разведданные, которых игрок не заслужил.
    // Видеть, куда идёт сосед, и встречать его заранее означало бы
    // выиграть войну, ни разу не выйдя в космос. Поэтому у чужих отрядов
    // всё, что ниже, остаётся пустым: сервер их просто не отправляет.
    //
    // Своё же нужно целиком: игрок обязан видеть маршрут, который сам
    // задал, — иначе он не знает, дошёл приказ или потерялся по дороге.
    uint16_t tag = 0;
    uint8_t stance = uint8_t(sim::Stance::Reserve);
    uint8_t evade = 0;
    uint32_t anchor = sim::kNoSystem;
    uint32_t anchorOrbit = sim::kNoOrbit;
    uint8_t routeStep = 0;
    uint8_t routeCount = 0;
    uint32_t route[sim::FleetOrders::kMaxRoute] = {};

    /// Куда отряд идёт прямо сейчас. kNoSystem — никуда.
    uint32_t routeTarget() const {
        return routeStep < routeCount ? route[routeStep] : sim::kNoSystem;
    }

    /// Сравнение идёт ПО ВСЕМУ СОСТАВУ, циклом.
    ///
    /// Раньше здесь были выписаны четыре класса корпусов — те, что
    /// существовали, когда это писалось. Классов стало восемь, и четыре
    /// из них — тендер, носитель, монитор, титан — не участвовали
    /// ни в сравнении, ни в записи в пакет. Клиент их просто НЕ ВИДЕЛ:
    /// флот из десяти титанов выглядел пустым, а построенный монитор
    /// не появлялся в списке вовсе.
    ///
    /// Найдено при добавлении колонизатора — он стал бы девятым
    /// невидимым. Цикл исключает этот класс ошибок целиком: новый
    /// корпус попадает в снапшот сам, без правки в трёх местах.
    bool operator==(const FleetView& o) const {
        if (empire != o.empire || system != o.system || nextSystem != o.nextSystem ||
            orbit != o.orbit || progress.raw() != o.progress.raw()) {
            return false;
        }
        for (size_t index = 0; index < sim::kHullClasses; ++index) {
            if (composition.ships[index] != o.composition.ships[index]) return false;
        }
        // Приказы тоже сравниваются, и это не мелочь: без них снапшот
        // считал бы отряд неизменившимся после смены маршрута, и игрок
        // видел бы старый план до первого боя. Тот же класс ошибки,
        // из-за которого четыре корпуса из восьми не ездили по сети.
        if (tag != o.tag || stance != o.stance || evade != o.evade) return false;
        if (anchor != o.anchor || anchorOrbit != o.anchorOrbit) return false;
        if (routeStep != o.routeStep || routeCount != o.routeCount) return false;
        for (uint8_t index = 0; index < routeCount; ++index) {
            if (route[index] != o.route[index]) return false;
        }
        return true;
    }
    bool operator!=(const FleetView& o) const { return !(*this == o); }
};

/// Изменяемое состояние планеты.
///
/// Геометрия планет (класс, число слотов, орбита) выводится из сида, и
/// клиент строит её у себя той же функцией, что и сервер. По сети идёт
/// только то, чего из сида не вывести.
///
/// С переносом владения на планеты сюда переехало главное: КТО ХОЗЯИН.
/// Раньше владение было полем системы, планета же несла одну застройку.
/// Теперь планета — единица захвата, и клиенту нужны и её владелец, и её
/// оборона, и осада, и стройка, иначе панель системы не может показать,
/// что именно в этой системе происходит.
struct PlanetView {
    uint8_t owner = 0xFF;          // kNoEmpire укладывается в 0xFF
    uint8_t specialization = 0;
    uint8_t readiness = 0;         // готовность обороны, 0..100
    uint8_t siegeEmpire = 0xFF;    // кто осаждает
    uint8_t siegeProgress = 0;     // 0..100, для ничьей планеты — занятие
    uint8_t buildSlot = 0xFF;      // слот текущей стройки, 0xFF — стройки нет
    uint8_t buildBuilding = 0;     // Building, который возводится
    uint8_t buildPercent = 0;      // 0..100
    uint8_t buildQueued = 0;       // сколько заказов ждёт очереди
    /// Стройка оплачена и идёт. Ноль при buildSlot != 0xFF означает
    /// «заказ принят, но минералов на него не хватило» — а это разные
    /// состояния, и игрок обязан их различать.
    uint8_t buildPaid = 0;
    uint8_t buildings[sim::kMaxSlots] = {};

    bool operator==(const PlanetView& o) const {
        if (owner != o.owner || specialization != o.specialization) return false;
        if (readiness != o.readiness) return false;
        if (siegeEmpire != o.siegeEmpire || siegeProgress != o.siegeProgress) return false;
        if (buildSlot != o.buildSlot || buildBuilding != o.buildBuilding) return false;
        if (buildPercent != o.buildPercent || buildQueued != o.buildQueued) return false;
        if (buildPaid != o.buildPaid) return false;
        for (uint8_t i = 0; i < sim::kMaxSlots; ++i) {
            if (buildings[i] != o.buildings[i]) return false;
        }
        return true;
    }
    bool operator!=(const PlanetView& o) const { return !(*this == o); }
};

/// Ресурсы своей империи.
struct EmpireView {
    fx energy = fx::zero();
    fx minerals = fx::zero();
    fx alloys = fx::zero();
    fx research = fx::zero();
    fx influence = fx::zero();

    /// Чистый приход В СЕКУНДУ по каждому ресурсу.
    ///
    /// ГЛАВНОЕ ЧИСЛО ВЕРХНЕЙ ПОЛОСЫ, важнее запаса. Запас отвечает на
    /// «сколько у меня сейчас», приход — на «что будет дальше»,
    /// а решения игрок принимает про дальше. Империя с двадцатью тысячами
    /// сплавов и минусом по энергии проигрывает; по одному запасу этого
    /// не видно вовсе.
    ///
    /// Едет с сервера, а не считается клиентом как разность запасов между
    /// снапшотами: разность включает разовые траты, и после заказа корабля
    /// доход показал бы обвал, которого нет. Сервер знает настоящую
    /// скорость, потому что сам её и считает.
    fx energyIncome = fx::zero();
    fx mineralsIncome = fx::zero();
    fx alloysIncome = fx::zero();
    fx researchIncome = fx::zero();
    fx influenceIncome = fx::zero();

    /// Сколько сплавов в секунду НЕ выплавлено из-за нехватки минералов.
    /// Главный сигнал «достройте шахты», и его место — рядом со сплавами.
    fx foundryIdle = fx::zero();

    /// Стадия сезона и сколько до следующей.
    ///
    /// Едет в снапшоте, а не считается клиентом из своего тика: клиент
    /// не обязан знать длительности стадий, а сервер обязан быть
    /// единственным источником правды о времени. Два байта на кадр —
    /// цена того, что «до Конфликта осталось 40 минут» у всех совпадает.
    uint8_t stage = 0;
    /// Секунд до следующей стадии. Ноль на Финале.
    uint32_t stageSecondsLeft = 0;

    /// Престиж: пять треков и сумма. Игрок обязан видеть, за что играет.
    uint32_t prestigeTerritory = 0;
    uint32_t prestigeEconomy = 0;
    uint32_t prestigeScience = 0;
    uint32_t prestigeWar = 0;
    uint32_t prestigeDiplomacy = 0;

    uint32_t prestigeTotal() const {
        return prestigeTerritory + prestigeEconomy + prestigeScience + prestigeWar +
               prestigeDiplomacy;
    }
};

/// Полное состояние мира глазами одного клиента.
struct WorldView {
    uint64_t tick = 0;
    EmpireView empire;
    std::vector<SystemView> systems;
    std::map<uint32_t, FleetView> fleets;
    std::map<uint32_t, PlanetView> planets;

    void resize(uint32_t systemCount) { systems.assign(systemCount, SystemView{}); }
};

// ---------------------------------------------------------------------------
// Сторона сервера
// ---------------------------------------------------------------------------

/// Считает и пакует дельту для ОДНОГО клиента.
///
/// Своя на каждого: у разных игроков разные подтверждения, а значит разные
/// базы. Общей быть не может по определению.
class SnapshotWriter {
public:
    void reset(uint32_t systemCount);

    /// Уложить разницу между `current` и подтверждённой базой.
    ///
    /// Пишет столько, сколько влезет в остаток пакета; недописанное уедет
    /// в следующем снапшоте, потому что база сдвинется только на то, что
    /// реально отправлено.
    void write(ByteWriter& writer, const WorldView& current);

    /// Клиент подтвердил снапшот: всё, что в нём было, стало базой.
    void acknowledge(uint16_t snapshotId);

    uint16_t lastSnapshotId() const { return uint16_t(nextId_ - 1); }
    /// Сколько снапшотов отправлено и ещё не подтверждено.
    uint32_t pending() const;

private:
    /// Что уехало в одном снапшоте — чтобы применить это к базе при
    /// подтверждении. Иначе база сдвинулась бы на то, чего клиент не видел.
    struct Pending {
        uint16_t id = 0;
        bool used = false;
        uint64_t tick = 0;
        EmpireView empire;
        bool empireSent = false;
        std::vector<std::pair<uint32_t, SystemView>> systems;
        std::vector<std::pair<uint32_t, FleetView>> fleets;
        std::vector<std::pair<uint32_t, PlanetView>> planets;
        std::vector<uint32_t> removed;
    };

    static constexpr uint32_t kPendingSlots = kAckWindow * 2;

    WorldView base_;
    bool baseEmpireValid_ = false;
    Pending pending_[kPendingSlots];
    uint16_t nextId_ = 1;
    /// С какой системы продолжать обход в следующий раз.
    ///
    /// Без этого при нехватке места мы каждый раз паковали бы одни и те же
    /// первые системы, а хвост карты не обновлялся бы никогда.
    uint32_t systemCursor_ = 0;
};

/// Собрать состояние мира глазами одной империи.
///
/// Пока без AOI: галактика на 200-500 систем целиком помещается в дельту.
/// Ограничение видимости появится вместе с туманом войны — это отдельное
/// игровое решение, а не оптимизация.
void collectView(sim::World& world, const sim::Galaxy& galaxy, uint32_t empire,
                 uint64_t tick, WorldView& out);

// ---------------------------------------------------------------------------
// Сторона клиента
// ---------------------------------------------------------------------------

/// Применяет приходящие дельты к своей копии мира.
class SnapshotReader {
public:
    void reset(uint32_t systemCount);

    /// Применить снапшот. false — пакет битый или база не та.
    bool apply(ByteReader& reader);

    /// Номер последнего применённого снапшота — его клиент шлёт обратно.
    uint16_t lastSnapshotId() const { return lastId_; }

    const WorldView& view() const { return view_; }
    WorldView& view() { return view_; }

private:
    WorldView view_;
    uint16_t lastId_ = 0;
};

}  // namespace pw::game
