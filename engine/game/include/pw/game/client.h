#pragma once

// Клиент: зеркало мира и отправка намерений.
//
// Клиент НЕ ПРИМЕНЯЕТ ИГРОВЫХ ПРАВИЛ. Ни одного. Он рисует то, что прислал
// сервер, и отправляет намерения — «хочу вести этот флот туда». Захват
// системы, исход боя, списание ресурсов происходят только на сервере
// и доезжают снапшотом.
//
// Это стоит немного отзывчивости и покупает полную невозможность
// клиентских читов (docs/03): подделанный пакет ничего не даёт, потому
// что решает не он.
//
// Галактику клиент строит сам — из сида, пришедшего в Welcome. Та же
// функция, тот же fixed-point, бит в бит та же карта. По сети идёт три
// десятка байт вместо сотен килобайт.

#include <cstdint>
#include <string>
#include <vector>

#include "pw/game/protocol.h"
#include "pw/game/snapshot.h"
#include "pw/net/connection.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/world.h"

namespace pw::game {

/// Что произошло с точки зрения интерфейса.
struct ClientEvent {
    NoticeKind kind = NoticeKind::None;
    uint32_t system = 0;
};

class Client {
public:
    /// Начать подключение.
    void connect(const net::Address& server, const std::string& name, int64_t now);
    void disconnect();

    /// Принять датаграмму от сервера.
    void receive(const uint8_t* data, size_t size, int64_t now);

    /// Собрать очередной исходящий пакет. 0 — сейчас слать нечего.
    size_t update(int64_t now, uint8_t* buffer, size_t capacity);

    // --- намерения игрока ---

    bool orderMove(uint32_t fleet, uint32_t target);
    bool orderBuildShip(uint32_t system, sim::Hull hull, uint8_t count = 1);
    bool orderBuildBuilding(uint32_t planet, uint8_t slot, sim::Building building);
    /// Высадить колонию: флот с колонизатором занимает ничью планету.
    bool orderColonize(uint32_t fleet, uint32_t planet);
    /// Выделить корабли одного класса в отдельный флот.
    bool orderSplitFleet(uint32_t fleet, sim::Hull hull, uint16_t count);

    // --- состояние ---

    net::ConnectionState state() const { return connection_.state(); }
    net::DisconnectReason reason() const { return connection_.reason(); }
    net::RejectReason rejectReason() const { return connection_.rejectReason(); }
    /// Галактика построена и снапшоты применяются.
    bool ready() const { return ready_; }

    uint32_t empire() const { return empire_; }
    uint32_t capital() const { return capital_; }
    const sim::Galaxy& galaxy() const { return galaxy_; }
    const WorldView& view() const { return snapshots_.view(); }
    int64_t roundTrip() const { return connection_.roundTrip(); }
    uint32_t lossPercent() const { return connection_.lossPercent(); }

    /// Забрать накопившиеся события. Список очищается.
    std::vector<ClientEvent> takeEvents();

    /// Свои флоты, стоящие в системе.
    std::vector<uint32_t> fleetsAt(uint32_t system) const;

    /// Планета глазами игрока: геометрия своя, застройка от сервера.
    struct PlanetInfo {
        uint32_t id = 0;
        uint32_t system = 0;
        uint8_t planetClass = 0;
        uint8_t slots = 0;
        uint8_t orbit = 0;
        uint8_t specialization = 0;
        /// Владелец ПЛАНЕТЫ. Захватывают их, а не системы, поэтому
        /// в одной системе владельцы соседних орбит могут быть разными.
        uint8_t owner = 0xFF;
        uint8_t readiness = 0;      // готовность обороны, 0..100
        uint8_t siegeEmpire = 0xFF; // кто осаждает
        uint8_t siegeProgress = 0;  // 0..100
        uint8_t buildSlot = 0xFF;   // слот текущей стройки
        uint8_t buildBuilding = 0;  // Building, который возводится
        uint8_t buildPercent = 0;   // 0..100
        uint8_t buildQueued = 0;    // сколько заказов ждёт очереди
        uint8_t buildPaid = 0;      // оплачена ли стройка
        uint8_t buildings[sim::kMaxSlots] = {};
        /// Свободных слотов под застройку. Слот со стройкой уже занят:
        /// иначе игрок заказал бы в него второе здание, увидев его пустым.
        uint8_t freeSlots() const;
        /// Идёт ли стройка прямо сейчас.
        bool building() const { return buildSlot != 0xFF; }
    };

    /// Планеты системы. Геометрия берётся из СВОЕЙ галактики — она
    /// выводится из сида и по сети не ездит; застройка приходит снапшотом.
    std::vector<PlanetInfo> planetsAt(uint32_t system) const;

    /// Имя, под которым игрок вошёл. Экран показывает его рядом с гербом:
    /// в MMO игрок обязан видеть, за кого играет, — на сервере тысяча
    /// таких же гербов, и цвет отличает его только от соседей.
    const std::string& name() const { return name_; }

private:
    net::Connection connection_;
    SnapshotReader snapshots_;
    sim::World galaxyWorld_;   // держит сущности систем построенной галактики
    sim::Galaxy galaxy_;

    std::string name_;
    uint32_t empire_ = 0;
    uint32_t capital_ = 0;
    bool ready_ = false;
    bool joinSent_ = false;

    std::vector<ClientEvent> events_;

    bool send(const uint8_t* data, size_t size);
    void handleMessage(const uint8_t* data, size_t size);
};

}  // namespace pw::game
