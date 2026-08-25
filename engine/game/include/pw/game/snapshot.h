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
struct SystemView {
    uint8_t owner = 0xFF;      // kNoEmpire укладывается в 0xFF
    uint8_t readiness = 0;     // готовность обороны, 0..100
    uint8_t siegeEmpire = 0xFF;
    uint8_t siegeProgress = 0; // 0..100

    bool operator==(const SystemView& o) const {
        return owner == o.owner && readiness == o.readiness &&
               siegeEmpire == o.siegeEmpire && siegeProgress == o.siegeProgress;
    }
    bool operator!=(const SystemView& o) const { return !(*this == o); }
};

/// Флот, каким его видит клиент.
struct FleetView {
    uint32_t id = 0;
    uint8_t empire = 0xFF;
    uint32_t system = 0;
    uint32_t nextSystem = 0;
    fx progress = fx::zero();
    sim::Fleet composition{};

    bool operator==(const FleetView& o) const {
        return empire == o.empire && system == o.system && nextSystem == o.nextSystem &&
               progress.raw() == o.progress.raw() &&
               composition.corvettes == o.composition.corvettes &&
               composition.destroyers == o.composition.destroyers &&
               composition.cruisers == o.composition.cruisers &&
               composition.battleships == o.composition.battleships;
    }
    bool operator!=(const FleetView& o) const { return !(*this == o); }
};

/// Застройка планеты — единственное, что в планете меняется.
///
/// Геометрия планет (класс, число слотов, орбита) выводится из сида, и
/// клиент строит её у себя той же функцией, что и сервер. По сети идёт
/// только то, чего из сида не вывести: что игрок построил и на чём
/// специализировал.
struct PlanetView {
    uint8_t specialization = 0;
    uint8_t buildings[sim::kMaxSlots] = {};

    bool operator==(const PlanetView& o) const {
        if (specialization != o.specialization) return false;
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
