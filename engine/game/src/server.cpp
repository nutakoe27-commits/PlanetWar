#include "pw/game/server.h"

#include <algorithm>

#include "pw/sim/battle_system.h"
#include "pw/sim/combat.h"
#include "pw/sim/control.h"

namespace pw::game {

namespace {

/// Сколько миллисекунд длится один тик симуляции.
constexpr int64_t kTickMilliseconds = 1000 / sim::kTicksPerSecond;

/// Больше этого числа тиков за один вызов сервер не догоняет.
///
/// Если процесс встал на минуту (отладчик, своп, миграция), догонять
/// шестьсот тиков в одном вызове — значит встать ещё раз, уже надолго.
/// Лучше признать, что время потеряно, и идти дальше: это заметно игрокам
/// как один рывок, а не как повисший сервер.
constexpr int64_t kMaxCatchUpTicks = 20;

}  // namespace

// ---------------------------------------------------------------------------
// Запуск
// ---------------------------------------------------------------------------

void Server::start(const ServerConfig& config) {
    config_ = config;

    sim::registerGalaxyComponents(world_);
    sim::registerFleetComponents(world_);
    sim::registerControlComponents(world_);
    sim::registerEconomyComponents(world_);
    sim::registerProductionComponents(world_);
    sim::registerBattleComponents(world_);

    galaxy_.generate(world_, config_.galaxy);
    sim::initialiseControl(world_, galaxy_);
    sim::initialiseEconomy(world_);
    sim::initialiseProduction(world_, galaxy_);
    sim::initialiseBattles(world_, galaxy_);

    world_.setResource(&galaxy_);
    world_.setResource(&ledger_);
    world_.setResource(&commands_);
    world_.setResource(&presence_);

    homeTaken_.assign(galaxy_.systemCount(), false);
    tick_ = 0;
    running_ = true;
    startedAt_ = 0;
}

uint32_t Server::playerCount() const {
    uint32_t total = 0;
    for (const auto& [address, player] : players_) {
        if (player.connection.state() == net::ConnectionState::Connected) ++total;
    }
    return total;
}

uint32_t Server::pickHome() {
    const uint32_t count = galaxy_.systemCount();
    if (count == 0) return 0;

    // Первому игроку — любая система, следующим — та, что дальше всех от
    // уже занятых. Соседние старты означали бы первую встречу на второй
    // минуте: у проигравшего не было бы ни одного осмысленного хода.
    uint32_t best = 0;
    int32_t bestDistance = -1;
    bool anyTaken = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (homeTaken_[i]) { anyTaken = true; break; }
    }
    if (!anyTaken) {
        homeTaken_[0] = true;
        return 0;
    }

    for (uint32_t candidate = 0; candidate < count; ++candidate) {
        if (homeTaken_[candidate]) continue;

        int32_t nearest = 1 << 20;
        for (uint32_t other = 0; other < count; ++other) {
            if (!homeTaken_[other]) continue;
            const int32_t hops = galaxy_.hopDistance(candidate, other);
            if (hops >= 0) nearest = std::min(nearest, hops);
        }
        if (nearest > bestDistance) {
            bestDistance = nearest;
            best = candidate;
        }
    }
    homeTaken_[best] = true;
    return best;
}

// ---------------------------------------------------------------------------
// Приём
// ---------------------------------------------------------------------------

void Server::receive(const net::Address& from, const uint8_t* data, size_t size, int64_t now) {
    if (!running_) return;

    auto found = players_.find(from);
    if (found == players_.end()) {
        // Соединения ещё нет. Единственное, что мы согласны разобрать, —
        // запрос на подключение. Всё прочее молча выбрасывается: отвечать
        // на неопознанный пакет значит стать усилителем чужого трафика.
        uint16_t version = 0;
        if (!net::Connection::parseRequest(data, size, version)) return;

        if (version != net::kProtocolVersion) {
            Player stranger;
            stranger.connection.accept(from, 0, now);
            // Отказ уедет отдельным пакетом на следующем update; проще
            // отправить его сразу, поэтому держим отказ в отдельной ветке
            // без создания игрока.
            players_.erase(from);
            OutgoingPacket packet;
            packet.to = from;
            packet.data.resize(net::kMaxPacketSize);
            const size_t written = net::Connection::buildReject(
                packet.data.data(), packet.data.size(), net::RejectReason::VersionMismatch);
            if (written > 0) {
                packet.data.resize(written);
                pendingRejects_.push_back(std::move(packet));
            }
            return;
        }

        if (playerCount() >= config_.maxPlayers) {
            OutgoingPacket packet;
            packet.to = from;
            packet.data.resize(net::kMaxPacketSize);
            const size_t written = net::Connection::buildReject(
                packet.data.data(), packet.data.size(), net::RejectReason::ServerFull);
            if (written > 0) {
                packet.data.resize(written);
                pendingRejects_.push_back(std::move(packet));
            }
            return;
        }

        Player player;
        const uint32_t empire = uint32_t(players_.size());
        player.empire = empire;
        player.home = pickHome();
        player.connection.accept(from, empire, now);
        player.snapshots.reset(galaxy_.systemCount());

        // Империя появляется сразу: игрок владеет столицей с первой
        // секунды, даже если ещё не прислал имя.
        player.empireEntity = world_.create();
        world_.add<sim::Empire>(
            player.empireEntity,
            sim::Empire{fx::fromInt(config_.startingEnergy),
                        fx::fromInt(config_.startingMinerals),
                        fx::fromInt(config_.startingAlloys), fx::zero(), fx::zero(),
                        empire, player.home});

        if (sim::Owner* owner = world_.get<sim::Owner>(galaxy_.systemEntity(player.home))) {
            owner->empire = empire;
        }
        // Столица считается принадлежащей игроку С САМОГО НАЧАЛА, а не
        // «захваченной» им. Без этой строки первым, что видит вошедший,
        // становится «система захвачена» о его собственном доме.
        if (player.home < previousOwners_.size()) {
            previousOwners_[player.home] = uint8_t(empire & 0xFFu);
        }
        if (sim::SystemDefense* defense =
                world_.get<sim::SystemDefense>(galaxy_.systemEntity(player.home))) {
            defense->readiness = defense->maxReadiness;
        }

        const sim::Entity fleet = world_.create();
        world_.add<sim::Fleet>(fleet, config_.startingFleet);
        world_.add<sim::FleetLocation>(
            fleet, sim::FleetLocation{player.home, player.home, fx::zero()});
        world_.add<sim::MoveOrder>(fleet, sim::MoveOrder{sim::kNoSystem, 0});
        world_.add<sim::Owner>(fleet, sim::Owner{empire, 0});
        world_.add<sim::FleetArmament>(fleet, sim::balancedArmament());
        world_.add<sim::FleetArmament>(player.empireEntity, sim::balancedArmament());

        found = players_.emplace(from, std::move(player)).first;
        sendWelcome(found->second);
        return;
    }

    Player& player = found->second;
    net::ReceivedPacket packet;
    if (!player.connection.receive(data, size, now, packet)) return;

    // Подтверждение снапшота едет ненадёжной частью пакета клиента:
    // это просто «последнее, что я видел», и терять его не страшно.
    if (!packet.payload.empty()) {
        net::ByteReader reader(packet.payload.data(), packet.payload.size());
        const uint64_t acknowledged = reader.varint();
        if (!reader.failed() && acknowledged <= 0xFFFF) {
            player.acknowledged = uint16_t(acknowledged);
            player.snapshots.acknowledge(player.acknowledged);
        }
    }
}

void Server::sendWelcome(Player& player) {
    uint8_t buffer[net::kMaxMessageSize];
    net::ByteWriter writer(buffer, sizeof(buffer));

    WelcomeMessage message;
    message.params = config_.galaxy;
    message.empire = player.empire;
    message.capitalSystem = player.home;
    message.tick = tick_;
    writeWelcome(writer, message);

    if (!writer.overflowed()) player.connection.sendReliable(buffer, writer.size());
}

// ---------------------------------------------------------------------------
// Приказы
// ---------------------------------------------------------------------------

void Server::handleMessage(Player& player, const uint8_t* data, size_t size,
                           std::vector<OutgoingPacket>&) {
    net::ByteReader reader(data, size);
    MessageType type = MessageType::Join;
    if (!readMessageType(reader, type)) return;

    switch (type) {
        case MessageType::Join: {
            JoinMessage message;
            if (!readJoin(reader, message)) return;
            player.name = message.name;
            player.joined = true;
            return;
        }
        case MessageType::MoveFleet: {
            MoveFleetMessage message;
            if (!readMoveFleet(reader, message)) return;
            applyMove(player, message);
            return;
        }
        case MessageType::BuildShip: {
            BuildShipMessage message;
            if (!readBuildShip(reader, message)) return;
            applyBuildShip(player, message);
            return;
        }
        case MessageType::BuildBuilding: {
            BuildBuildingMessage message;
            if (!readBuildBuilding(reader, message)) return;
            applyBuildBuilding(player, message);
            return;
        }
        // Уведомления идут только от сервера. Пришедшее от клиента —
        // либо ошибка версии, либо попытка что-то подделать.
        case MessageType::Welcome:
        case MessageType::Notice:
            return;
    }
}

void Server::applyMove(Player& player, const MoveFleetMessage& message) {
    const auto reject = [&](NoticeKind kind, uint32_t system) {
        ++rejectedOrders_;
        uint8_t buffer[32];
        net::ByteWriter writer(buffer, sizeof(buffer));
        writeNotice(writer, NoticeMessage{kind, system});
        if (!writer.overflowed()) player.connection.sendReliable(buffer, writer.size());
    };

    if (message.target >= galaxy_.systemCount()) {
        reject(NoticeKind::OrderRejected, message.target);
        return;
    }

    // Номер сущности приходит от клиента, поэтому проверяется всё: жива ли
    // она, флот ли это вообще и ВАШ ли он. Без последней проверки любой
    // игрок водил бы чужие флоты — самый дешёвый чит из возможных.
    bool applied = false;
    world_.each<sim::Fleet, sim::FleetLocation, sim::MoveOrder, sim::Owner>(
        [&](sim::Entity entity, sim::Fleet&, sim::FleetLocation& location,
            sim::MoveOrder& order, sim::Owner& owner) {
            if (entity.index != message.fleet) return;
            if (owner.empire != player.empire) return;

            // Пути может не быть: граф связен не весь. Флот в пути между
            // узлами считается от СЛЕДУЮЩЕГО узла — он уже не может
            // развернуться на полдороге.
            if (location.nextSystem != message.target &&
                galaxy_.nextHop(location.nextSystem, message.target) < 0) {
                return;
            }
            order.target = message.target;
            applied = true;
        });

    if (!applied) reject(NoticeKind::OrderRejected, message.target);
}

void Server::applyBuildShip(Player& player, const BuildShipMessage& message) {
    if (message.system >= galaxy_.systemCount()) {
        ++rejectedOrders_;
        return;
    }
    const sim::Entity system = galaxy_.systemEntity(message.system);

    const sim::Owner* owner = world_.get<sim::Owner>(system);
    if (owner == nullptr || owner->empire != player.empire) {
        ++rejectedOrders_;
        return;
    }
    sim::BuildQueue* queue = world_.get<sim::BuildQueue>(system);
    if (queue == nullptr) {
        ++rejectedOrders_;
        return;
    }

    // Без верфи система не строит флот вовсе. Раньше заказ принимался
    // и молча не выполнялся: игрок жал клавишу, ничего не происходило,
    // и понять почему было неоткуда. Молчаливый отказ — худший вид
    // отказа, потому что человек повторяет одно и то же.
    const std::vector<uint32_t> shipyards = sim::countBuildingsPerSystem(
        world_, sim::Building::Shipyard, galaxy_.systemCount());
    if (message.system >= shipyards.size() || shipyards[message.system] == 0) {
        ++rejectedOrders_;
        uint8_t buffer[32];
        net::ByteWriter writer(buffer, sizeof(buffer));
        writeNotice(writer, NoticeMessage{NoticeKind::OrderRejected, message.system});
        if (!writer.overflowed()) player.connection.sendReliable(buffer, writer.size());
        return;
    }

    sim::enqueueBuild(*queue, sim::Hull(message.hull), message.count);
}

void Server::applyBuildBuilding(Player& player, const BuildBuildingMessage& message) {
    bool applied = false;
    world_.each<sim::Planet, sim::PlanetDevelopment>(
        [&](sim::Entity entity, sim::Planet& planet, sim::PlanetDevelopment& development) {
            if (entity.index != message.planet) return;
            if (message.slot >= planet.slots) return;

            const sim::Owner* owner = world_.get<sim::Owner>(galaxy_.systemEntity(planet.system));
            if (owner == nullptr || owner->empire != player.empire) return;

            development.buildings[message.slot] = message.building;
            applied = true;
        });
    if (!applied) ++rejectedOrders_;
}

// ---------------------------------------------------------------------------
// Тик
// ---------------------------------------------------------------------------

void Server::step() {
    sim::TickContext context;
    context.tick = tick_;

    sim::systemFleetMovement(world_, context);
    sim::systemBattles(world_, context);
    sim::systemPresence(world_, context);
    sim::systemSiege(world_, context);
    sim::systemEconomy(world_, context);
    sim::systemDefenceCap(world_, context);
    sim::systemProduction(world_, context);
    sim::systemMergeFleets(world_, context);
    sim::systemDisbandEmpty(world_, context);
    sim::systemApplyCommands(world_, context);

    ++tick_;
}

void Server::notify(uint32_t empire, NoticeKind kind, uint32_t system) {
    for (auto& [address, player] : players_) {
        if (player.empire != empire) continue;
        uint8_t buffer[32];
        net::ByteWriter writer(buffer, sizeof(buffer));
        writeNotice(writer, NoticeMessage{kind, system});
        if (!writer.overflowed()) player.connection.sendReliable(buffer, writer.size());
        return;
    }
}

void Server::notifyChanges() {
    const uint32_t systemCount = galaxy_.systemCount();
    if (previousOwners_.size() != systemCount) {
        previousOwners_.assign(systemCount, 0xFF);
        // Первый проход только запоминает: иначе игрок при подключении
        // получил бы уведомление о захвате собственной столицы.
        world_.each<sim::StarSystem, sim::Owner>(
            [&](sim::Entity, sim::StarSystem& system, sim::Owner& owner) {
                if (system.index < systemCount) {
                    previousOwners_[system.index] =
                        uint8_t(owner.empire == sim::kNoEmpire ? 0xFFu : owner.empire & 0xFFu);
                }
            });
        return;
    }

    // --- смена владельца ---
    world_.each<sim::StarSystem, sim::Owner>(
        [&](sim::Entity, sim::StarSystem& system, sim::Owner& owner) {
            if (system.index >= systemCount) return;
            const uint8_t now =
                uint8_t(owner.empire == sim::kNoEmpire ? 0xFFu : owner.empire & 0xFFu);
            const uint8_t before = previousOwners_[system.index];
            if (now == before) return;

            if (before != 0xFF) notify(before, NoticeKind::SystemLost, system.index);
            if (now != 0xFF) notify(now, NoticeKind::SystemCaptured, system.index);
            previousOwners_[system.index] = now;
        });

    // --- потери флота ---
    //
    // Считаем ТОННАЖ, а не число отрядов.
    //
    // Первая версия считала отряды и слала «флот уничтожен» при обычном
    // слиянии: два отряда становятся одним, количество падает, а не
    // потеряно ни одного корабля. Ложная тревога хуже пропущенной —
    // игрок бросает дела и летит спасать то, что цело.
    //
    // Тоннаж при слиянии сохраняется точно: это проверяется отдельным
    // инвариантом в ночном прогоне. Значит его падение — это ровно
    // потеря, и ничего больше.
    std::vector<std::pair<uint32_t, uint32_t>> alive;
    std::vector<uint32_t> tonnage(config_.maxPlayers + 1, 0);
    world_.each<sim::Fleet, sim::Owner>([&](sim::Entity entity, sim::Fleet& fleet,
                                            sim::Owner& owner) {
        if (sim::fleetEmpty(fleet)) return;
        alive.emplace_back(entity.index, owner.empire);
        if (owner.empire < tonnage.size()) tonnage[owner.empire] += sim::fleetTonnage(fleet);
    });

    if (previousTonnage_.size() == tonnage.size()) {
        for (size_t empire = 0; empire < tonnage.size(); ++empire) {
            if (tonnage[empire] >= previousTonnage_[empire]) continue;
            notify(uint32_t(empire), NoticeKind::FleetDestroyed, 0);
        }
    }
    previousTonnage_ = tonnage;
    previousFleets_.swap(alive);

    // --- сражения ---
    //
    // Перезарядка взлетает в момент боя и дальше только убывает, поэтому
    // скачок вверх — надёжный признак свежего сражения. Отдельного журнала
    // не нужно: событие живёт ровно до следующего боя в этой системе.
    if (previousCooldown_.size() != systemCount) previousCooldown_.assign(systemCount, 0);

    world_.each<sim::StarSystem, sim::BattleState>(
        [&](sim::Entity, sim::StarSystem& system, sim::BattleState& battle) {
            if (system.index >= systemCount) return;
            const uint32_t before = previousCooldown_[system.index];
            previousCooldown_[system.index] = battle.cooldown;
            if (battle.cooldown <= before) return;

            if (battle.lastWinner == sim::kBattleDraw) return;   // разошлись — не новость
            if (battle.lastWinner != sim::kBattleNobody) {
                notify(battle.lastWinner, NoticeKind::BattleWon, system.index);
            }
            if (battle.lastLoser != sim::kBattleNobody) {
                notify(battle.lastLoser, NoticeKind::BattleLost, system.index);
            }
        });
}

void Server::update(int64_t now, std::vector<OutgoingPacket>& outgoing) {
    if (!running_) return;
    if (startedAt_ == 0) startedAt_ = now;

    // Отказы, накопленные в receive: их некуда было отправить сразу.
    for (OutgoingPacket& packet : pendingRejects_) outgoing.push_back(std::move(packet));
    pendingRejects_.clear();

    // Сначала приказы: команда, пришедшая до тика, обязана попасть в него,
    // а не ждать следующего. Иначе отклик игрока плавал бы на сто
    // миллисекунд без всякой причины.
    for (auto& [address, player] : players_) {
        std::vector<uint8_t> message;
        while (player.connection.receiveReliable(message)) {
            handleMessage(player, message.data(), message.size(), outgoing);
        }
    }

    // Симуляция тикает по СВОИМ часам, а не по числу вызовов update:
    // иначе сервер на медленной машине играл бы в замедленную игру.
    const int64_t speed = int64_t(config_.speed == 0 ? 1u : config_.speed);
    const int64_t wanted = (now - startedAt_) * speed / kTickMilliseconds;
    int64_t behind = wanted - int64_t(tick_);
    // Догоняем не больше, чем позволяет скорость: при ускорении в двадцать
    // раз двадцать тиков за вызов — это норма, а не отставание.
    if (behind > kMaxCatchUpTicks * speed) {
        // Признаём потерянное время вместо того, чтобы догонять его в
        // одном вызове и повиснуть ещё раз.
        startedAt_ = now - int64_t(tick_) * kTickMilliseconds / speed;
        behind = kMaxCatchUpTicks * speed;
    }
    for (int64_t i = 0; i < behind; ++i) step();
    if (behind > 0) notifyChanges();

    // Снапшоты и исходящие пакеты.
    for (auto it = players_.begin(); it != players_.end();) {
        Player& player = it->second;
        player.connection.update(now);

        if (player.connection.state() == net::ConnectionState::Failed ||
            player.connection.state() == net::ConnectionState::Disconnected) {
            // Империя игрока остаётся в мире: её системы и флоты никуда
            // не деваются, и игрок может вернуться. Освобождать их сразу
            // значило бы наказывать за обрыв связи — ровно то, чего
            // обещание «уважение ко времени игрока» не допускает.
            it = players_.erase(it);
            continue;
        }

        if (player.connection.shouldSend(now)) {
            collectView(world_, galaxy_, player.empire, tick_, view_);

            uint8_t snapshot[net::kMaxPacketSize];
            net::ByteWriter snapshotWriter(snapshot, sizeof(snapshot));
            player.snapshots.write(snapshotWriter, view_);

            OutgoingPacket packet;
            packet.to = it->first;
            packet.data.resize(net::kMaxPacketSize);
            const size_t written = player.connection.build(
                packet.data.data(), packet.data.size(), now,
                snapshotWriter.overflowed() ? nullptr : snapshot,
                snapshotWriter.overflowed() ? 0 : snapshotWriter.size());
            if (written > 0) {
                packet.data.resize(written);
                outgoing.push_back(std::move(packet));
            }
        }
        ++it;
    }
}

void Server::shutdown(std::vector<OutgoingPacket>& outgoing) {
    for (auto& [address, player] : players_) {
        OutgoingPacket packet;
        packet.to = address;
        packet.data.resize(64);
        const size_t written =
            player.connection.buildDisconnect(packet.data.data(), packet.data.size());
        if (written > 0) {
            packet.data.resize(written);
            outgoing.push_back(std::move(packet));
        }
    }
    players_.clear();
    running_ = false;
}

}  // namespace pw::game
