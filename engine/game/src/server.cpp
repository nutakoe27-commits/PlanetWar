#include "pw/game/server.h"

#include <algorithm>

#include "pw/sim/battle_system.h"
#include "pw/sim/combat.h"
#include "pw/sim/colony.h"
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
    sim::registerSeasonComponents(world_);

    galaxy_.generate(world_, config_.galaxy);
    sim::initialiseControl(world_, galaxy_);
    sim::initialiseEconomy(world_);
    sim::initialiseProduction(world_, galaxy_);
    sim::initialiseBattles(world_, galaxy_);

    world_.setResource(&galaxy_);
    world_.setResource(&ledger_);
    world_.setResource(&commands_);
    world_.setResource(&presence_);

    season_ = sim::Season{};
    season_.config = config_.season;
    world_.setResource(&season_);

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
    // Столица обязана быть многопланетной. С переносом владения на планеты
    // «система» перестала быть единицей: старт на одиноком мире у чёрной
    // дыры — это три слота застройки и одна планета обороны против четырёх
    // у соседа. Не невезение, а проигрыш до первого хода.
    constexpr uint8_t kMinHomePlanets = 3;

    auto suitable = [&](uint32_t index) {
        return !homeTaken_[index] && galaxy_.planetCount(index) >= kMinHomePlanets;
    };

    bool anyTaken = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (homeTaken_[i]) { anyTaken = true; break; }
    }
    if (!anyTaken) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!suitable(i)) continue;
            homeTaken_[i] = true;
            return i;
        }
        homeTaken_[0] = true;
        return 0;
    }

    // Первому игроку — любая подходящая система, следующим — та, что дальше
    // всех от уже занятых. Соседние старты означали бы первую встречу на
    // второй минуте: у проигравшего не было бы ни одного осмысленного хода.
    uint32_t best = count;
    int32_t bestDistance = -1;
    for (int pass = 0; pass < 2 && best == count; ++pass) {
        // Второй проход снимает требование к числу планет: если тесная
        // галактика больше ничего не предлагает, пусть игрок сядет хоть
        // куда-нибудь, а не получит отказ в подключении.
        for (uint32_t candidate = 0; candidate < count; ++candidate) {
            if (homeTaken_[candidate]) continue;
            if (pass == 0 && !suitable(candidate)) continue;

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
    }
    if (best == count) best = 0;
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
        // Престиж заводится вместе с империей. Отдельным компонентом,
        // а не полями Empire: счёт за сезон читают и пишут раз в секунду,
        // а казну — каждый тик, и таскать одно через другое незачем.
        world_.add<sim::Prestige>(player.empireEntity, sim::Prestige{});

        // СТОЛИЦА — ОДНА ПЛАНЕТА, а не вся родная система.
        //
        // Раньше игрок получал всю систему целиком и первые полчаса просто
        // застраивал выданное. Решений в этом нет: что построить — вопрос,
        // но КУДА расти вопросом не был, потому что расти было некуда.
        // Империя начиналась готовой, и вся первая стадия сезона сводилась
        // к ожиданию, пока накопятся сплавы.
        //
        // Теперь у игрока один мир, а вокруг — ничьи планеты его же родной
        // системы. Первое настоящее решение принимается на второй минуте:
        // колонизатор стоит как два с половиной корвета, и это выбор между
        // «ещё одна планета» и «чем её защищать».
        //
        // Выбирается САМАЯ ВМЕСТИТЕЛЬНАЯ планета системы, а при равенстве —
        // ближняя к звезде. Не первая попавшаяся: стартовые условия обязаны
        // быть одинаковы у всех, а число слотов у планет разное, и выдать
        // одному игроку мир на двенадцать слотов, а другому на четыре
        // значило бы решить партию броском кубика.
        uint32_t capitalOrbit = 0;
        uint8_t capitalSlots = 0;
        for (uint32_t orbit = 0; orbit < galaxy_.planetCount(player.home); ++orbit) {
            const sim::Entity planet = galaxy_.planetEntity(player.home, orbit);
            if (!planet.valid()) continue;
            const sim::Planet* record = world_.get<sim::Planet>(planet);
            if (record == nullptr) continue;
            if (record->slots > capitalSlots) {
                capitalSlots = record->slots;
                capitalOrbit = orbit;
            }
        }

        const sim::Entity capital = galaxy_.planetEntity(player.home, capitalOrbit);
        if (capital.valid()) {
            if (sim::Owner* owner = world_.get<sim::Owner>(capital)) owner->empire = empire;
            if (sim::PlanetDefense* defense = world_.get<sim::PlanetDefense>(capital)) {
                // Столица начинается с ПОЛНОЙ обороны, в отличие от колоний.
                // Дом обязан быть крепостью с первой секунды: игрок, у
                // которого столицу берут, пока он ищет первую кнопку,
                // больше не вернётся.
                defense->readiness = defense->maxReadiness;
            }
        }
        player.capitalOrbit = capitalOrbit;

        // Владелец системы производный и пересчитается в ближайшем тике,
        // но проставляется и здесь: снапшот собирается раньше первого тика.
        if (sim::Owner* owner = world_.get<sim::Owner>(galaxy_.systemEntity(player.home))) {
            owner->empire = empire;
        }
        // Столица считается принадлежащей игроку С САМОГО НАЧАЛА, а не
        // «захваченной» им. Без этой строки первым, что видит вошедший,
        // становится «система захвачена» о его собственном доме.
        if (player.home < previousOwners_.size()) {
            previousOwners_[player.home] = uint8_t(empire & 0xFFu);
        }

        const sim::Entity fleet = world_.create();
        world_.add<sim::Fleet>(fleet, config_.startingFleet);
        world_.add<sim::FleetLocation>(
            fleet, sim::standingAt(player.home));
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
        case MessageType::Colonize: {
            ColonizeMessage message;
            if (!readColonize(reader, message)) return;
            applyColonize(player, message);
            return;
        }
        case MessageType::SplitFleet: {
            SplitFleetMessage message;
            if (!readSplitFleet(reader, message)) return;
            applySplitFleet(player, message);
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

void Server::applyColonize(Player& player, const ColonizeMessage& message) {
    const auto reject = [&](uint32_t system) {
        ++rejectedOrders_;
        uint8_t buffer[32];
        net::ByteWriter writer(buffer, sizeof(buffer));
        writeNotice(writer, NoticeMessage{NoticeKind::OrderRejected, system});
        if (!writer.overflowed()) player.connection.sendReliable(buffer, writer.size());
    };

    // Планета ищется ОБХОДОМ по номеру, а не собирается из номера обратно
    // в сущность.
    //
    // Собранная руками `Entity{index, 0}` не находит ничего: у живой
    // сущности поколение не ноль, и `get` честно отвечает «такой нет».
    // Приказ отвергался всегда, а выглядело это как «колонизация не
    // работает» — без единой подсказки, где именно она не работает.
    uint32_t planetSystem = sim::kNoSystem;
    uint32_t planetEmpire = sim::kNoEmpire;
    bool planetFound = false;
    world_.each<sim::Planet, sim::Owner>(
        [&](sim::Entity entity, sim::Planet& planet, sim::Owner& owner) {
            if (entity.index != message.planet) return;
            planetSystem = planet.system;
            planetEmpire = owner.empire;
            planetFound = true;
        });
    if (!planetFound) {
        reject(sim::kNoSystem);
        return;
    }

    // Правила живут в pw_sim и одни на всех: сервер, бот и тест зовут одну
    // и ту же функцию. Клиент зовёт её же, чтобы погасить кнопку заранее,
    // и потому отказ после нажатия становится невозможен.
    bool landed = false;
    world_.each<sim::Fleet, sim::FleetLocation, sim::Owner>(
        [&](sim::Entity entity, sim::Fleet& fleet, sim::FleetLocation& location,
            sim::Owner& owner) {
            if (entity.index != message.fleet) return;
            if (owner.empire != player.empire) return;
            if (sim::colonizeCheck(player.empire, fleet, location, planetEmpire,
                                   planetSystem) != sim::ColonyRefusal::Ok) {
                return;
            }
            // Колонизатор списывается здесь же: если бы списание жило
            // отдельно от проверки, между ними однажды вклинилось бы
            // условие, и корабль тратился бы впустую.
            --fleet[sim::Hull::Colonizer];
            landed = true;
        });

    if (!landed) {
        reject(planetSystem);
        return;
    }

    world_.each<sim::Planet, sim::Owner, sim::PlanetDefense>(
        [&](sim::Entity entity, sim::Planet&, sim::Owner& owner,
            sim::PlanetDefense& defense) {
            if (entity.index != message.planet) return;
            owner.empire = player.empire;
            defense.readiness = sim::kColonyStartReadiness;
        });

    // Свежая колония сразу считается «была нашей»: иначе первое, что
    // увидит игрок, — уведомление «система захвачена» о собственной
    // только что основанной колонии.
    if (planetSystem < previousOwners_.size()) {
        previousOwners_[planetSystem] = uint8_t(player.empire & 0xFFu);
    }
    notify(player.empire, NoticeKind::ColonyFounded, planetSystem);
}

void Server::applySplitFleet(Player& player, const SplitFleetMessage& message) {
    sim::Fleet taken{};
    uint32_t system = sim::kNoSystem;
    bool applied = false;
    const sim::FleetArmament* armament = nullptr;

    world_.each<sim::Fleet, sim::FleetLocation, sim::Owner>(
        [&](sim::Entity entity, sim::Fleet& fleet, sim::FleetLocation& location,
            sim::Owner& owner) {
            if (entity.index != message.fleet) return;
            if (owner.empire != player.empire) return;
            if (sim::splitCheck(fleet, location, sim::Hull(message.hull), message.count) !=
                sim::SplitRefusal::Ok) {
                return;
            }
            taken = sim::applySplit(fleet, sim::Hull(message.hull), message.count);
            system = location.system;
            armament = world_.get<sim::FleetArmament>(entity);
            applied = true;
        });

    if (!applied) {
        ++rejectedOrders_;
        uint8_t buffer[32];
        net::ByteWriter writer(buffer, sizeof(buffer));
        writeNotice(writer, NoticeMessage{NoticeKind::OrderRejected, system});
        if (!writer.overflowed()) player.connection.sendReliable(buffer, writer.size());
        return;
    }

    // Новый флот появляется через буфер команд: создавать сущности
    // в обходе нельзя, это правило самого World.
    //
    // Вооружение копируется с исходного: выделенный отряд — это часть
    // того же флота, а не свежая постройка. Иначе выделенные корветы
    // молча меняли бы оружие, и разделение флота стало бы способом
    // переоснастить его бесплатно.
    //
    // Орбиту новому отряду НЕ задаём: её назначит systemFleetStation
    // в ближайшем тике по общему правилу. Задать её здесь значило бы
    // завести второе место, где решается, у какой планеты стоит флот, —
    // и однажды эти два места разошлись бы.
    if (sim::Commands* commands = world_.resource<sim::Commands>()) {
        commands->spawnFleet(player.empire, system, taken, armament);
    }
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
    uint32_t shipyards = 0;
    uint32_t drydocks = 0;
    world_.each<sim::Planet, sim::Owner, sim::PlanetDevelopment>(
        [&](sim::Entity, sim::Planet& planet, sim::Owner& planetOwner,
            sim::PlanetDevelopment& development) {
            if (planet.system != message.system) return;
            // Верфь на чужом анклаве внутри вашей системы вам не служит.
            if (planetOwner.empire != player.empire) return;
            shipyards += sim::countBuildings(planet, development, sim::Building::Shipyard);
            drydocks += sim::countBuildings(planet, development, sim::Building::Drydock);
        });

    // Титан требует не только верфи, но и ремонтного дока.
    //
    // Смысл ворот не в том, чтобы усложнить, а в том, чтобы у титана была
    // ЦЕНА, отличная от денежной. Иначе он просто самый дорогой корабль,
    // который берут все, у кого хватило сплавов, и «венец сезона»
    // превращается в очередную покупку. С воротами империя сначала строит
    // инфраструктуру в конкретной системе — то есть заявляет, где у неё
    // главная верфь, и делает эту систему целью для соседей.
    const bool needsDrydock = sim::Hull(message.hull) == sim::Hull::Titan;
    if (shipyards == 0 || (needsDrydock && drydocks == 0)) {
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
    uint32_t system = 0;

    world_.each<sim::Planet, sim::Owner, sim::PlanetDevelopment, sim::PlanetConstruction>(
        [&](sim::Entity entity, sim::Planet& planet, sim::Owner& owner,
            sim::PlanetDevelopment& development, sim::PlanetConstruction& site) {
            if (entity.index != message.planet) return;
            // Слоты СЧИТАЮТСЯ: хабитаты добавляют места, и строить в них
            // должно быть можно.
            if (message.slot >= sim::usableSlots(planet, development)) return;

            // Владение проверяется у САМОЙ ПЛАНЕТЫ, а не у её системы:
            // теперь захватывают планеты, и удержанный в чужом тылу мир
            // строит своему хозяину, а не хозяину системы вокруг.
            if (owner.empire != player.empire) return;

            system = planet.system;

            // Снос мгновенный, стройка — нет. Сносить долго незачем, а вот
            // отменить начатую стройку игрок должен уметь одним действием,
            // и это то же самое действие.
            if (message.building == uint8_t(sim::Building::None)) {
                // Снос хабитата отбирает у планеты два слота. Если они
                // заняты, здания в них оказались бы «за краем» планеты:
                // они не производят, не сносятся и не видны. Отказываем
                // вслух, а не молча оставляем призраков.
                if (development.buildings[message.slot] == uint8_t(sim::Building::Habitat)) {
                    const uint8_t after = sim::usableSlots(planet, development) -
                                          sim::kHabitatSlots;
                    for (uint8_t slot = after; slot < sim::kMaxSlots; ++slot) {
                        if (slot == message.slot) continue;
                        if (development.buildings[slot] != uint8_t(sim::Building::None)) {
                            return;  // applied остаётся ложным — игрок получит отказ
                        }
                    }
                }
                development.buildings[message.slot] = uint8_t(sim::Building::None);
                if (site.slot == message.slot) {
                    sim::enqueueConstruction(site, sim::PlanetConstruction::kNoSlot,
                                             sim::Building::None);
                }
                applied = true;
                return;
            }

            if (message.building >= uint8_t(sim::Building::Count)) return;

            // Очередь может быть полна — тогда заказ не принят, и игрок
            // обязан об этом узнать.
            applied = sim::enqueueConstruction(site, message.slot,
                                               sim::Building(message.building));
        });

    if (applied) return;

    // Молчаливый отказ — худший вид отказа: человек жмёт клавишу, ничего
    // не происходит, и понять почему неоткуда. Поэтому отказ едет игроку.
    ++rejectedOrders_;
    uint8_t buffer[32];
    net::ByteWriter writer(buffer, sizeof(buffer));
    writeNotice(writer, NoticeMessage{NoticeKind::OrderRejected, system});
    if (!writer.overflowed()) player.connection.sendReliable(buffer, writer.size());
}

// ---------------------------------------------------------------------------
// Тик
// ---------------------------------------------------------------------------

void Server::step() {
    sim::TickContext context;
    context.tick = tick_;

    // Производное владение системами пересчитывается ПЕРВЫМ: и бой, и
    // присутствие, и поиск цели смотрят на владельца системы, и все они
    // обязаны в одном тике видеть одно и то же.
    // Стадия сезона считается ПЕРВОЙ: на неё смотрят и осада, и кризис,
    // и интерфейс, и все обязаны в одном тике видеть одну и ту же.
    sim::systemSeason(world_, context);
    sim::systemControlRollup(world_, context);
    sim::systemFleetMovement(world_, context);
    sim::systemFleetStation(world_, context);
    sim::systemBattles(world_, context);
    sim::systemPresence(world_, context);
    sim::systemSiege(world_, context);
    sim::systemEconomy(world_, context);
    sim::planetDefenceCap(world_, context);
    sim::planetConstructionTick(world_, context);
    sim::systemProduction(world_, context);
    sim::systemCrisis(world_, context);
    sim::systemPrestige(world_, context);
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

    // --- планеты: осада, потеря, захват ---
    //
    // Захватывают теперь планеты, и молчать о них нельзя. Заголовок
    // control.h обещает игроку, что владелец успеет получить уведомление,
    // а союзники — увидеть осаду и прийти; без этих новостей обещание
    // было пустым, и узнать об осаде можно было, только глядя в нужную
    // часть карты в нужную минуту.
    {
        const bool firstPass = previousPlanets_.empty();
        world_.each<sim::Planet, sim::Owner, sim::SiegeState>(
            [&](sim::Entity entity, sim::Planet& planet, sim::Owner& owner,
                sim::SiegeState& siege) {
                PlanetMemory now;
                now.owner =
                    uint8_t(owner.empire == sim::kNoEmpire ? 0xFFu : owner.empire & 0xFFu);
                now.besieger = uint8_t(siege.besieger == sim::kNoEmpire
                                           ? 0xFFu
                                           : siege.besieger & 0xFFu);
                now.system = planet.system;

                PlanetMemory& before = previousPlanets_[entity.index];
                if (firstPass) {
                    // Первый проход только запоминает: иначе игрок при
                    // подключении получил бы новость о захвате каждой
                    // планеты собственной столицы.
                    before = now;
                    return;
                }

                if (now.owner != before.owner) {
                    if (before.owner != 0xFF) {
                        notify(before.owner, NoticeKind::PlanetLost, now.system);
                    }
                    if (now.owner != 0xFF) {
                        notify(now.owner, NoticeKind::PlanetCaptured, now.system);
                    }
                }
                // Осада объявляется один раз, в момент начала. Повторять
                // её каждый тик значит превратить журнал в шум, а шум
                // игрок перестаёт читать целиком.
                if (now.besieger != before.besieger && now.besieger != 0xFF &&
                    now.owner != 0xFF) {
                    notify(now.owner, NoticeKind::PlanetSieged, now.system);
                }
                before = now;
            });
    }

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

            // Ничья — тоже новость, и приходит обеим сторонам. Молчать
            // о взаимном истреблении нельзя: игрок иначе узнаёт о гибели
            // флота только по его пропаже с карты.
            const NoticeKind forWinner =
                battle.drawn != 0 ? NoticeKind::BattleDraw : NoticeKind::BattleWon;
            const NoticeKind forLoser =
                battle.drawn != 0 ? NoticeKind::BattleDraw : NoticeKind::BattleLost;

            if (battle.lastWinner != sim::kBattleNobody) {
                notify(battle.lastWinner, forWinner, system.index);
            }
            if (battle.lastLoser != sim::kBattleNobody) {
                notify(battle.lastLoser, forLoser, system.index);
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
