// pw_server — headless-сервер сезона.
//
// Ни графики, ни SDL, ни Vulkan: собирается пресетом server-linux-x64-release
// и запускается там, где видеокарты нет и не будет. Это не «режим без
// картинки», а основной способ существования игры — клиент лишь окно в неё.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "pw/game/server.h"
#include "pw/net/socket.h"

using namespace pw;
using namespace pw::game;

namespace {

std::atomic<bool> running{true};

void onSignal(int) { running = false; }

/// Монотонные миллисекунды. Именно монотонные: перевод системных часов
/// (или синхронизация по NTP) не должен ни останавливать сезон, ни
/// прокручивать его вперёд.
int64_t nowMilliseconds() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - start).count();
}

void printUsage() {
    std::printf(
        "pw_server — сервер PlanetWar\n\n"
        "  --port <n>       порт (по умолчанию 27015)\n"
        "  --systems <n>    размер галактики (по умолчанию 200)\n"
        "  --seed <n>       сид сезона\n"
        "  --players <n>    сколько игроков принимать (по умолчанию 8)\n"
        "  --report <сек>   как часто печатать состояние (0 — не печатать)\n"
        "  --weeks <n>      длина сезона в неделях (по умолчанию 11)\n"
        "  --speed <n>      во сколько раз ускорить игровое время\n"
        "                   (для оценки и отладки; сезон рассчитан на 1)\n");
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 27015;
    ServerConfig config;
    config.galaxy.systemCount = 200;
    config.galaxy.seed = 0x50414E4554574152ull;
    config.maxPlayers = 8;
    int64_t reportSeconds = 10;
    int64_t weeks = 0;   // 0 — оставить умолчание SeasonConfig

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = uint16_t(std::atoi(argv[++i]));
        } else if (arg == "--systems" && i + 1 < argc) {
            config.galaxy.systemCount = uint32_t(std::atoi(argv[++i]));
        } else if (arg == "--seed" && i + 1 < argc) {
            config.galaxy.seed = std::strtoull(argv[++i], nullptr, 0);
        } else if (arg == "--players" && i + 1 < argc) {
            config.maxPlayers = uint32_t(std::atoi(argv[++i]));
        } else if (arg == "--report" && i + 1 < argc) {
            reportSeconds = std::atoi(argv[++i]);
        } else if (arg == "--weeks" && i + 1 < argc) {
            weeks = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--speed" && i + 1 < argc) {
            config.speed = uint32_t(std::max(1, std::atoi(argv[++i])));
        } else {
            printUsage();
            return arg == "--help" ? 0 : 2;
        }
    }

    // Длина сезона задаётся ПЕРЕСЧЁТОМ длительностей, а не целым масштабом:
    // масштаб врёт на всём, что не делится нацело, и сезон на девять недель
    // при базе в одиннадцать получил бы масштаб 1 и остался бы прежним.
    if (weeks > 0) config.season.stretchTo(weeks * 7 * sim::SeasonConfig::kDay);

    if (!net::initialiseSockets()) {
        std::fprintf(stderr, "не удалось поднять сетевую подсистему\n");
        return 1;
    }

    net::Socket socket;
    if (!socket.open(port)) {
        std::fprintf(stderr, "%s\n", socket.error().c_str());
        net::shutdownSockets();
        return 1;
    }

    Server server;
    server.start(config);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::printf("PlanetWar: сезон поднят\n");
    std::printf("  порт        %u\n", socket.port());
    std::printf("  галактика   %u систем, сид 0x%llX\n", server.galaxy().systemCount(),
                static_cast<unsigned long long>(config.galaxy.seed));
    std::printf("  мест        %u\n", config.maxPlayers);
    {
        // Длину сезона печатаем всегда: это первое, о чём спрашивает
        // игрок, и первое, что забывает поставить администратор.
        const int64_t total = config.season.totalSeconds();
        std::printf("  сезон       %lld нед (расширение %lld сут, конфликт %lld сут, "
                    "кризис %lld сут, финал %lld сут)\n",
                    static_cast<long long>(total / (7 * sim::SeasonConfig::kDay)),
                    static_cast<long long>(config.season.expansionSeconds /
                                           sim::SeasonConfig::kDay),
                    static_cast<long long>(config.season.conflictSeconds /
                                           sim::SeasonConfig::kDay),
                    static_cast<long long>(config.season.crisisSeconds /
                                           sim::SeasonConfig::kDay),
                    static_cast<long long>(config.season.finalSeconds /
                                           sim::SeasonConfig::kDay));
    }
    if (config.speed > 1) {
        std::printf("  скорость    x%u — игровое время ускорено\n", config.speed);
    }
    std::printf("  остановка   Ctrl+C\n\n");

    int64_t nextReport = reportSeconds * 1000;
    std::vector<OutgoingPacket> outgoing;

    while (running) {
        const int64_t now = nowMilliseconds();

        // Разгребаем ВСЮ очередь приёма, а не один пакет: при восьми
        // игроках на 10 Гц за тик приходит десяток датаграмм, и разбирать
        // их по одной значило бы отставать всё сильнее.
        uint8_t buffer[net::kMaxPacketSize];
        net::Address from;
        for (int drained = 0; drained < 256; ++drained) {
            const size_t size = socket.receive(from, buffer, sizeof(buffer));
            if (size == 0) break;
            server.receive(from, buffer, size, now);
        }

        outgoing.clear();
        server.update(now, outgoing);
        for (const OutgoingPacket& packet : outgoing) {
            socket.send(packet.to, packet.data.data(), packet.data.size());
        }

        if (reportSeconds > 0 && now >= nextReport) {
            nextReport = now + reportSeconds * 1000;
            std::printf("[%lld с] игроков %u, тик %llu",
                        static_cast<long long>(now / 1000), server.playerCount(),
                        static_cast<unsigned long long>(server.tick()));
            for (const auto& [address, player] : server.players()) {
                std::printf("\n    %-16s империя %u, задержка %lld мс, потери %u%%",
                            player.name.empty() ? "(без имени)" : player.name.c_str(),
                            player.empire,
                            static_cast<long long>(player.connection.roundTrip()),
                            player.connection.lossPercent());
            }
            std::printf("\n");
            std::fflush(stdout);
        }

        // Спим до следующего тика. Без сна процесс сожрал бы ядро целиком
        // ради работы, которой на самом деле на единицы процентов.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::printf("\nостановка: прощаемся с игроками\n");
    outgoing.clear();
    server.shutdown(outgoing);
    for (const OutgoingPacket& packet : outgoing) {
        socket.send(packet.to, packet.data.data(), packet.data.size());
    }

    socket.close();
    net::shutdownSockets();
    return 0;
}
