// pw_bot — клиент без графики.
//
// Играет сам: подключается, застраивает столицу, заказывает корабли и
// расширяется на соседние системы. Нужен для трёх разных вещей:
//
//   1. ДОКАЗЫВАЕТ, ЧТО ИГРА РАБОТАЕТ, до всякого рендера. Если бот
//      захватывает планеты через настоящие сокеты — значит цикл
//      «подключился, увидел, приказал, получил» замкнут, и остаётся
//      только нарисовать его;
//   2. НАГРУЖАЕТ СЕРВЕР. Восемь ботов на одной машине проверяют то, что
//      живыми игроками проверить нельзя, пока их нет;
//   3. ЛОВИТ ОТЛИЧИЯ ЖИВОЙ СЕТИ ОТ СИМУЛЯТОРА. Тесты гоняют протокол
//      в памяти; здесь он идёт через ядро, MTU и настоящие задержки.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "pw/game/client.h"
#include "pw/net/socket.h"

using namespace pw;
using namespace pw::game;

namespace {

std::atomic<bool> running{true};
void onSignal(int) { running = false; }

int64_t nowMilliseconds() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - start).count();
}

/// Порядок застройки: сначала сырьё, потом переработка, потом верфь.
///
/// Тот же список, что у ботов ночного прогона: если он там даёт осмысленную
/// экономику, он и здесь её даст, а расхождение сразу будет заметно.
const sim::Building kBuildOrder[] = {
    sim::Building::Mine,       sim::Building::Mine,     sim::Building::Mine,
    sim::Building::Foundry,    sim::Building::Foundry,  sim::Building::Shipyard,
    sim::Building::PowerPlant, sim::Building::PowerPlant,
    sim::Building::Mine,       sim::Building::Foundry,
    sim::Building::Laboratory, sim::Building::TradeHub,
};

void printUsage() {
    std::printf(
        "pw_bot — клиент PlanetWar без графики\n\n"
        "  --host <адрес>   сервер (по умолчанию 127.0.0.1)\n"
        "  --port <n>       порт (по умолчанию 27015)\n"
        "  --name <имя>     имя игрока\n"
        "  --seconds <n>    сколько играть (0 — до Ctrl+C)\n"
        "  --quiet          не печатать ход игры\n");
}

/// Решения бота. Вызывается раз в секунду — чаще незачем, игра медленная.
class Brain {
public:
    void think(Client& client) {
        if (!client.ready()) return;
        ++ticks_;

        buildSomething(client);
        orderShips(client);
        expand(client);
    }

    uint32_t built() const { return built_; }
    uint32_t ordered() const { return ordered_; }
    uint32_t dispatched() const { return dispatched_; }

private:
    void buildSomething(Client& client) {
        // По одному зданию за раз: очередь на сервере всё равно обработает
        // их по порядку, а завалить канал сотней приказов в первый же тик
        // значит проверить не игру, а переполнение очереди.
        for (uint32_t system = 0; system < client.galaxy().systemCount(); ++system) {
            for (const auto& planet : client.planetsAt(system)) {
                // Владение проверяется у ПЛАНЕТЫ: захватывают их, а не
                // системы, и в чужой системе у бота вполне может стоять
                // собственный анклав.
                if (planet.owner != uint8_t(client.empire())) continue;

                // Пока идёт стройка, новый заказ на эту планету не нужен.
                //
                // Без этой проверки бот каждый цикл повторял заказ в тот же
                // слот: слот считается пустым, пока здание не достроено,
                // и очередь набивалась пятью копиями одного здания.
                // Поймали разбором того, куда уходят минералы.
                if (planet.building()) continue;

                for (uint8_t slot = 0; slot < planet.slots; ++slot) {
                    if (planet.buildings[slot] != uint8_t(sim::Building::None)) continue;
                    const size_t index = slot % (sizeof(kBuildOrder) / sizeof(kBuildOrder[0]));
                    if (client.orderBuildBuilding(planet.id, slot, kBuildOrder[index])) {
                        ++built_;
                    }
                    return;
                }
            }
        }
    }

    void orderShips(Client& client) {
        if (ticks_ % 10 != 0) return;
        if (client.view().empire.alloys < fx::fromInt(sim::kCostDestroyer * 2)) return;

        for (uint32_t system = 0; system < client.galaxy().systemCount(); ++system) {
            if (client.view().systems[system].owner != uint8_t(client.empire())) continue;
            if (client.orderBuildShip(system, sim::Hull::Destroyer, 1)) ++ordered_;
            return;
        }
    }

    /// Осталось ли в системе что захватывать.
    ///
    /// Владелец системы на этот вопрос больше не отвечает: захватывают
    /// планеты, и система считается вашей уже по большинству. Флот, который
    /// смотрел на владельца системы, улетал, взяв ОДНУ планету из четырёх,
    /// и половина системы оставалась чужой навсегда.
    static bool worthTaking(const Client& client, uint32_t system) {
        const auto& view = client.view().systems[system];
        if (view.totalPlanets == 0) return false;
        return view.owner != uint8_t(client.empire()) ||
               view.ownedPlanets < view.totalPlanets;
    }

    void expand(Client& client) {
        if (ticks_ % 5 != 0) return;

        for (uint32_t system = 0; system < client.galaxy().systemCount(); ++system) {
            const auto fleets = client.fleetsAt(system);
            if (fleets.empty()) continue;

            // Здесь ещё есть работа — флот остаётся и доделывает её.
            if (worthTaking(client, system)) continue;

            int32_t best = -1;
            int32_t bestHops = 1 << 20;
            for (uint32_t target = 0; target < client.galaxy().systemCount(); ++target) {
                if (target == system) continue;
                if (!worthTaking(client, target)) continue;
                // Своё сначала: доделать наполовину взятую систему дешевле,
                // чем начинать новую, а брошенный анклав кормит соседа.
                if (client.view().systems[target].owner != 0xFF &&
                    client.view().systems[target].owner != uint8_t(client.empire())) {
                    continue;
                }
                const int32_t hops = client.galaxy().hopDistance(system, target);
                if (hops < 0 || hops >= bestHops) continue;
                bestHops = hops;
                best = int32_t(target);
            }
            if (best < 0) continue;
            if (client.orderMove(fleets.front(), uint32_t(best))) ++dispatched_;
            return;
        }
    }

    uint64_t ticks_ = 0;
    uint32_t built_ = 0;
    uint32_t ordered_ = 0;
    uint32_t dispatched_ = 0;
};

const char* noticeText(NoticeKind kind) {
    switch (kind) {
        case NoticeKind::SystemCaptured: return "система захвачена";
        case NoticeKind::SystemLost:     return "система потеряна";
        case NoticeKind::BattleWon:      return "бой выигран";
        case NoticeKind::BattleLost:     return "бой проигран";
        case NoticeKind::BattleDraw:     return "бой без победителя";
        case NoticeKind::PlanetSieged:   return "ОСАДА ПЛАНЕТЫ";
        case NoticeKind::PlanetLost:     return "планета потеряна";
        case NoticeKind::PlanetCaptured: return "планета взята";
        case NoticeKind::FleetDestroyed: return "флот уничтожен";
        case NoticeKind::OrderRejected:  return "приказ отвергнут";
        case NoticeKind::None:
        case NoticeKind::Count:          return "";
    }
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 27015;
    std::string name = "бот";
    int64_t seconds = 0;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = uint16_t(std::atoi(argv[++i]));
        else if (arg == "--name" && i + 1 < argc) name = argv[++i];
        else if (arg == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (arg == "--quiet") quiet = true;
        else {
            printUsage();
            return arg == "--help" ? 0 : 2;
        }
    }

    const net::Address server = net::Address::parse(host, port);
    if (!server.valid()) {
        std::fprintf(stderr, "непонятный адрес: %s\n", host.c_str());
        return 2;
    }

    if (!net::initialiseSockets()) {
        std::fprintf(stderr, "не удалось поднять сетевую подсистему\n");
        return 1;
    }

    net::Socket socket;
    if (!socket.open(0)) {
        std::fprintf(stderr, "%s\n", socket.error().c_str());
        net::shutdownSockets();
        return 1;
    }

    Client client;
    client.connect(server, name, nowMilliseconds());

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!quiet) std::printf("подключаюсь к %s...\n", server.toString().c_str());

    Brain brain;
    bool announced = false;
    int64_t nextThink = 0;
    int64_t nextReport = 0;
    const int64_t deadline = seconds > 0 ? seconds * 1000 : 0;

    while (running) {
        const int64_t now = nowMilliseconds();
        if (deadline > 0 && now >= deadline) break;

        uint8_t buffer[net::kMaxPacketSize];
        net::Address from;
        for (int drained = 0; drained < 64; ++drained) {
            const size_t size = socket.receive(from, buffer, sizeof(buffer));
            if (size == 0) break;
            if (from != server) continue;   // чужой отправитель — не наш сервер
            client.receive(buffer, size, now);
        }

        if (client.state() == net::ConnectionState::Failed) {
            const char* why = client.reason() == net::DisconnectReason::Rejected
                                  ? "сервер отказал"
                                  : "сервер не отвечает";
            std::fprintf(stderr, "%s\n", why);
            break;
        }
        if (client.state() == net::ConnectionState::Disconnected && announced) {
            std::printf("сервер попрощался\n");
            break;
        }

        if (client.ready() && !announced) {
            announced = true;
            if (!quiet) {
                std::printf("подключился: империя %u, столица %u, галактика %u систем\n",
                            client.empire(), client.capital(),
                            client.galaxy().systemCount());
            }
        }

        if (client.ready() && now >= nextThink) {
            nextThink = now + 1000;
            brain.think(client);
        }

        for (const ClientEvent& event : client.takeEvents()) {
            if (quiet) continue;
            if (event.kind == NoticeKind::OrderRejected) continue;  // шумно и неинтересно
            std::printf("  · %s (система %u)\n", noticeText(event.kind), event.system);
        }

        const size_t size = client.update(now, buffer, sizeof(buffer));
        if (size > 0) socket.send(server, buffer, size);

        if (!quiet && client.ready() && now >= nextReport) {
            nextReport = now + 10000;

            uint32_t systems = 0;
            for (const auto& system : client.view().systems) {
                if (system.owner == uint8_t(client.empire())) ++systems;
            }
            uint32_t fleets = 0, tonnage = 0;
            for (const auto& [id, fleet] : client.view().fleets) {
                if (fleet.empire != uint8_t(client.empire())) continue;
                ++fleets;
                tonnage += sim::fleetTonnage(fleet.composition);
            }

            std::printf("[%lld с] систем %u, флот %u т в %u отрядах, "
                        "сплавы %lld, минералы %lld · задержка %lld мс, потери %u%%\n",
                        static_cast<long long>(now / 1000), systems, tonnage, fleets,
                        static_cast<long long>(client.view().empire.alloys.floorToInt()),
                        static_cast<long long>(client.view().empire.minerals.floorToInt()),
                        static_cast<long long>(client.roundTrip()), client.lossPercent());

            // Где именно стоят флоты. Без этого «систем 1» шесть минут
            // подряд не отличить от «приказ не дошёл» — а это разные беды.
            for (const auto& [id, fleet] : client.view().fleets) {
                if (fleet.empire != uint8_t(client.empire())) continue;
                const uint32_t owner = fleet.system < client.view().systems.size()
                                           ? client.view().systems[fleet.system].owner
                                           : 0xFFu;
                if (fleet.system == fleet.nextSystem) {
                    std::printf("      флот %u стоит в системе %u (владелец %s)\n", id,
                                fleet.system,
                                owner == 0xFF ? "ничья" : (owner == client.empire() ? "своя"
                                                                                    : "чужая"));
                } else {
                    std::printf("      флот %u идёт %u -> %u, пройдено %lld%%\n", id,
                                fleet.system, fleet.nextSystem,
                                static_cast<long long>(
                                    (fleet.progress * fx::fromInt(100)).floorToInt()));
                }
            }
            std::fflush(stdout);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!quiet) {
        std::printf("\nитог: построено зданий %u, заказано кораблей %u, "
                    "отправлено флотов %u\n",
                    brain.built(), brain.ordered(), brain.dispatched());
    }

    // Прощаемся явно: сервер освободит место сразу, а не через пять секунд.
    uint8_t farewell[64];
    const size_t size = client.update(nowMilliseconds(), farewell, sizeof(farewell));
    (void)size;
    net::Connection temporary;
    uint8_t bye[64];
    const size_t byeSize = temporary.buildDisconnect(bye, sizeof(bye));
    if (byeSize > 0) socket.send(server, bye, byeSize);

    socket.close();
    net::shutdownSockets();
    return 0;
}
