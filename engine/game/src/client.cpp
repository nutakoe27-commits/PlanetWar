#include "pw/game/client.h"

#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"

namespace pw::game {

void Client::connect(const net::Address& server, const std::string& name, int64_t now) {
    connection_.connect(server, now);
    snapshots_.reset(0);
    name_ = name;
    ready_ = false;
    joinSent_ = false;
    empire_ = 0;
    capital_ = 0;
    events_.clear();
}

void Client::disconnect() { connection_.disconnect(); }

bool Client::send(const uint8_t* data, size_t size) {
    return connection_.sendReliable(data, size);
}

void Client::receive(const uint8_t* data, size_t size, int64_t now) {
    net::ReceivedPacket packet;
    if (!connection_.receive(data, size, now, packet)) return;

    // Снапшот применяем только после Welcome: до него неизвестно даже,
    // сколько в галактике систем, и любая дельта была бы бессмысленна.
    if (ready_ && !packet.payload.empty()) {
        net::ByteReader reader(packet.payload.data(), packet.payload.size());
        snapshots_.apply(reader);
    }

    std::vector<uint8_t> message;
    while (connection_.receiveReliable(message)) {
        handleMessage(message.data(), message.size());
    }
}

void Client::handleMessage(const uint8_t* data, size_t size) {
    net::ByteReader reader(data, size);
    MessageType type = MessageType::Join;
    if (!readMessageType(reader, type)) return;

    switch (type) {
        case MessageType::Welcome: {
            WelcomeMessage message;
            if (!readWelcome(reader, message)) return;

            // Строим ту же галактику, что и сервер: та же функция, тот же
            // fixed-point, бит в бит та же карта. Именно это позволяет не
            // гонять по сети сотни килобайт геометрии.
            sim::registerGalaxyComponents(galaxyWorld_);
            galaxy_.generate(galaxyWorld_, message.params);

            empire_ = message.empire;
            capital_ = message.capitalSystem;
            snapshots_.reset(galaxy_.systemCount());
            ready_ = true;
            return;
        }
        case MessageType::Notice: {
            NoticeMessage message;
            if (!readNotice(reader, message)) return;
            events_.push_back(ClientEvent{message.kind, message.system});
            return;
        }
        // Приказы идут только от клиента. Пришедшие от сервера — либо
        // ошибка версии, либо кто-то подделывает пакеты.
        case MessageType::Join:
        case MessageType::MoveFleet:
        case MessageType::BuildShip:
        case MessageType::BuildBuilding:
        case MessageType::Colonize:
        case MessageType::SplitFleet:
            return;
    }
}

size_t Client::update(int64_t now, uint8_t* buffer, size_t capacity) {
    connection_.update(now);

    // Имя отправляется один раз, как только соединение установилось.
    if (connection_.state() == net::ConnectionState::Connected && !joinSent_) {
        uint8_t message[net::kMaxMessageSize];
        net::ByteWriter writer(message, sizeof(message));
        writeJoin(writer, JoinMessage{name_});
        if (!writer.overflowed() && send(message, writer.size())) joinSent_ = true;
    }

    if (!connection_.shouldSend(now)) return 0;

    // Подтверждение снапшота — единственное, что клиент шлёт ненадёжно.
    // Терять его не страшно: следующий пакет повторит номер.
    uint8_t acknowledgement[16];
    net::ByteWriter ack(acknowledgement, sizeof(acknowledgement));
    ack.varint(snapshots_.lastSnapshotId());

    return connection_.build(buffer, capacity, now, acknowledgement, ack.size());
}

// ---------------------------------------------------------------------------
// Намерения
// ---------------------------------------------------------------------------

bool Client::orderMove(uint32_t fleet, uint32_t target) {
    uint8_t buffer[64];
    net::ByteWriter writer(buffer, sizeof(buffer));
    writeMoveFleet(writer, MoveFleetMessage{fleet, target});
    return !writer.overflowed() && send(buffer, writer.size());
}

bool Client::orderColonize(uint32_t fleet, uint32_t planet) {
    uint8_t buffer[64];
    net::ByteWriter writer(buffer, sizeof(buffer));
    writeColonize(writer, ColonizeMessage{fleet, planet});
    return !writer.overflowed() && send(buffer, writer.size());
}

bool Client::orderSplitFleet(uint32_t fleet, sim::Hull hull, uint16_t count) {
    uint8_t buffer[64];
    net::ByteWriter writer(buffer, sizeof(buffer));
    writeSplitFleet(writer, SplitFleetMessage{fleet, uint8_t(hull), count});
    return !writer.overflowed() && send(buffer, writer.size());
}

bool Client::orderBuildShip(uint32_t system, sim::Hull hull, uint8_t count) {
    uint8_t buffer[64];
    net::ByteWriter writer(buffer, sizeof(buffer));
    writeBuildShip(writer, BuildShipMessage{system, uint8_t(hull), count});
    return !writer.overflowed() && send(buffer, writer.size());
}

bool Client::orderBuildBuilding(uint32_t planet, uint8_t slot, sim::Building building) {
    uint8_t buffer[64];
    net::ByteWriter writer(buffer, sizeof(buffer));
    writeBuildBuilding(writer, BuildBuildingMessage{planet, slot, uint8_t(building)});
    return !writer.overflowed() && send(buffer, writer.size());
}

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------

std::vector<ClientEvent> Client::takeEvents() {
    std::vector<ClientEvent> out;
    out.swap(events_);
    return out;
}

uint8_t Client::PlanetInfo::freeSlots() const {
    uint8_t free = 0;
    for (uint8_t i = 0; i < slots && i < sim::kMaxSlots; ++i) {
        if (buildings[i] != uint8_t(sim::Building::None)) continue;
        if (i == buildSlot) continue;  // слот занят стройкой
        ++free;
    }
    return free;
}

std::vector<Client::PlanetInfo> Client::planetsAt(uint32_t system) const {
    std::vector<PlanetInfo> out;
    if (!ready_) return out;

    // Планеты создаются генератором галактики, а он детерминирован —
    // поэтому номера сущностей у клиента и сервера совпадают, и снапшот
    // может ссылаться на планету одним числом.
    const_cast<sim::World&>(galaxyWorld_).each<sim::Planet>(
        [&](sim::Entity entity, sim::Planet& planet) {
            if (planet.system != system) return;

            PlanetInfo info;
            info.id = entity.index;
            info.system = planet.system;
            info.planetClass = planet.planetClass;
            info.slots = planet.slots;
            info.orbit = planet.orbit;
            info.specialization = planet.specialization;

            const auto found = view().planets.find(info.id);
            if (found != view().planets.end()) {
                const PlanetView& live = found->second;
                info.specialization = live.specialization;
                info.owner = live.owner;
                info.readiness = live.readiness;
                info.siegeEmpire = live.siegeEmpire;
                info.siegeProgress = live.siegeProgress;
                info.buildSlot = live.buildSlot;
                info.buildBuilding = live.buildBuilding;
                info.buildPercent = live.buildPercent;
                info.buildQueued = live.buildQueued;
                info.buildPaid = live.buildPaid;
                for (uint8_t i = 0; i < sim::kMaxSlots; ++i) {
                    info.buildings[i] = live.buildings[i];
                }
            }
            out.push_back(info);
        });
    return out;
}

std::vector<uint32_t> Client::fleetsAt(uint32_t system) const {
    std::vector<uint32_t> out;
    for (const auto& [id, fleet] : view().fleets) {
        if (fleet.empire != uint8_t(empire_ & 0xFF)) continue;
        // Стоящий флот — тот, у кого текущий и следующий узлы совпадают.
        if (fleet.system != system || fleet.nextSystem != system) continue;
        out.push_back(id);
    }
    return out;
}

}  // namespace pw::game
