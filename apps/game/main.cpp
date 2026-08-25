// pw_game_client — играбельный клиент PlanetWar.
//
// Окно, карта галактики, выделение системы, приказы флоту и застройка
// планет. Ровно то, что дорожная карта называет играбельным ядром:
// «подключиться, увидеть галактику, послать флот, захватить планету».
//
// Клиент НЕ ПРИМЕНЯЕТ ИГРОВЫХ ПРАВИЛ — ни одного. Он рисует присланное
// сервером и отправляет намерения. Единственное, что он «предсказывает», —
// линию от выделенного флота к точке под курсором, и это не игровое
// правило, а показ собственного намерения игрока.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pw/core/log.h"
#include "pw/game/client.h"
#include "pw/net/socket.h"
#include "pw/platform/input.h"
#include "pw/platform/platform.h"
#include "pw/platform/window.h"
#include "pw/render/atlas.h"
#include "pw/render/font.h"
#include "pw/render/hud.h"
#include "pw/render/map_view.h"
#include "pw/rhi/rhi.h"

using namespace pw;

namespace {

// SPIR-V вкомпилирован в бинарь: на Android ассеты лежат внутри apk,
// на iOS в бандле, на десктопе рядом с программой. Массив одинаков везде.
#include "pw/rhi/shaders.inc"

int64_t nowMilliseconds() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - start).count();
}

std::vector<uint8_t> spirv(const unsigned char* data, size_t size) {
    return std::vector<uint8_t>(data, data + size);
}

/// Камера: куда смотрим и с каким увеличением.
///
/// Зум ограничен с обеих сторон намеренно. Слишком близко — игрок теряет
/// карту из виду и не может принимать решения; слишком далеко — звёзды
/// сливаются в кашу, и по ней нельзя кликнуть.
struct CameraControl {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float worldHeight = 2000.0f;
    float minHeight = 200.0f;
    float maxHeight = 6000.0f;

    void zoom(float steps) {
        // Умножением, а не сложением: шаг колеса должен менять масштаб
        // одинаково и вблизи, и издалека.
        worldHeight *= std::pow(0.85f, steps);
        worldHeight = std::clamp(worldHeight, minHeight, maxHeight);
    }

    /// Перевод пикселей экрана в мировые координаты.
    void toWorld(float pixelX, float pixelY, int width, int height, float& worldX,
                 float& worldY) const {
        const float halfHeight = worldHeight * 0.5f;
        const float aspect = height > 0 ? float(width) / float(height) : 1.0f;
        const float nx = width > 0 ? (pixelX / float(width)) * 2.0f - 1.0f : 0.0f;
        const float ny = height > 0 ? (pixelY / float(height)) * 2.0f - 1.0f : 0.0f;
        worldX = centerX + nx * halfHeight * aspect;
        worldY = centerY - ny * halfHeight;
    }

    rhi::Camera toRhi() const {
        rhi::Camera camera;
        camera.centerX = centerX;
        camera.centerY = centerY;
        camera.worldHeight = worldHeight;
        return camera;
    }
};

const char* hullName(sim::Hull hull) {
    switch (hull) {
        case sim::Hull::Corvette:   return "корвет";
        case sim::Hull::Destroyer:  return "эсминец";
        case sim::Hull::Cruiser:    return "крейсер";
        case sim::Hull::Battleship: return "линкор";
        default:                    return "?";
    }
}

const char* buildingName(uint8_t building) {
    switch (sim::Building(building)) {
        case sim::Building::None:       return "пусто";
        case sim::Building::Mine:       return "шахта";
        case sim::Building::PowerPlant: return "электростанция";
        case sim::Building::Foundry:    return "литейная";
        case sim::Building::Laboratory: return "лаборатория";
        case sim::Building::TradeHub:   return "торговый узел";
        case sim::Building::Fortress:   return "крепость";
        case sim::Building::Shipyard:   return "верфь";
        default:                        return "?";
    }
}

const char* noticeText(game::NoticeKind kind) {
    switch (kind) {
        case game::NoticeKind::SystemCaptured: return "система захвачена";
        case game::NoticeKind::SystemLost:     return "система потеряна";
        case game::NoticeKind::BattleWon:      return "бой выигран";
        case game::NoticeKind::BattleLost:     return "бой проигран";
        case game::NoticeKind::FleetDestroyed: return "флот уничтожен";
        case game::NoticeKind::OrderRejected:  return "приказ отвергнут";
        case game::NoticeKind::None:           return "";
    }
    return "";
}

void printUsage() {
    std::printf(
        "pw_game_client — клиент PlanetWar\n\n"
        "  --host <адрес>   сервер (по умолчанию 127.0.0.1)\n"
        "  --port <n>       порт (по умолчанию 27015)\n"
        "  --name <имя>     имя игрока\n"
        "  --width <n>      ширина окна\n"
        "  --height <n>     высота окна\n"
        "  --validation     слои проверки Vulkan\n"
        "  --shot <файл>    без окна: подключиться, отрисовать карту в PNG и выйти\n"
        "  --shot-after <с> сколько секунд поиграть до снимка (по умолчанию 3)\n"
        "  --shot-zoom <k>  приблизить перед снимком к своей столице (1 — вся карта)\n"
        "\nУправление:\n"
        "  колесо           зум\n"
        "  правая кнопка    панорамирование\n"
        "  левая кнопка     выделить систему\n"
        "  левая по цели    отправить выделенный флот\n"
        "  1..8             построить здание в первом свободном слоте\n"
        "  Q W E R          заказать корвет/эсминец/крейсер/линкор\n"
        "  Пробел           показать всю галактику\n"
        "  Escape           выход\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 27015;
    std::string name = "игрок";
    int windowWidth = 1440, windowHeight = 900;
    bool validation = false;
    std::string shotPath;
    int shotAfterSeconds = 3;
    float shotZoom = 1.0f;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = uint16_t(std::atoi(argv[++i]));
        else if (arg == "--name" && i + 1 < argc) name = argv[++i];
        else if (arg == "--width" && i + 1 < argc) windowWidth = std::atoi(argv[++i]);
        else if (arg == "--height" && i + 1 < argc) windowHeight = std::atoi(argv[++i]);
        else if (arg == "--validation") validation = true;
        else if (arg == "--shot" && i + 1 < argc) shotPath = argv[++i];
        else if (arg == "--shot-after" && i + 1 < argc) shotAfterSeconds = std::atoi(argv[++i]);
        else if (arg == "--shot-zoom" && i + 1 < argc) shotZoom = float(std::atof(argv[++i]));
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

    // Безголовый режим существует не для галочки. Он позволяет проверять
    // карту автоматически: отрисовать кадр в CI, посчитать пиксели и
    // сравнить с ожидаемым. Иначе игровой интерфейс проверяется только
    // глазами и только тогда, когда кто-то удосужился посмотреть.
    const bool headless = !shotPath.empty();

    if (!initPlatform(headless)) {
        std::fprintf(stderr, "не удалось поднять платформу\n");
        return 1;
    }

    WindowDesc windowDesc;
    windowDesc.title = "PlanetWar";
    windowDesc.width = windowWidth;
    windowDesc.height = windowHeight;
    windowDesc.headless = headless;
    Window window(windowDesc);
    if (!window.valid()) {
        std::fprintf(stderr, "не удалось создать окно\n");
        return 1;
    }

    rhi::DeviceDesc deviceDesc;
    deviceDesc.window = headless ? nullptr : &window;
    deviceDesc.validation = validation;
    deviceDesc.width = windowWidth;
    deviceDesc.height = windowHeight;
    if (!headless) window.framebufferSize(deviceDesc.width, deviceDesc.height);

    rhi::Device device;
    if (!device.init(deviceDesc)) {
        std::fprintf(stderr, "Vulkan: %s\n", device.lastError().c_str());
        return 1;
    }
    if (!device.createSpritePipeline(spirv(kSpriteVert, sizeof(kSpriteVert)),
                                     spirv(kSpriteFrag, sizeof(kSpriteFrag))) ||
        !device.createLinePipeline(spirv(kLineVert, sizeof(kLineVert)),
                                   spirv(kLineFrag, sizeof(kLineFrag)))) {
        std::fprintf(stderr, "не удалось собрать конвейеры: %s\n", device.lastError().c_str());
        return 1;
    }

    // Атлас. Ищем рядом с программой и в дереве исходников: при разработке
    // запускают из корня репозитория, в собранной игре — из папки рядом.
    render::Atlas atlas;
    const char* atlasCandidates[] = {"assets/build/ships.json", "../assets/build/ships.json",
                                     "../../assets/build/ships.json"};
    bool atlasReady = false;
    for (const char* path : atlasCandidates) {
        if (atlas.load(path)) { atlasReady = true; break; }
    }
    if (!atlasReady) {
        std::fprintf(stderr,
                     "атлас не найден: %s\n"
                     "соберите ассеты: tools/blender/build_assets.py --quality preview\n",
                     atlas.error().c_str());
        return 1;
    }

    const std::vector<Rgba8> composite = atlas.composite();
    const rhi::TextureHandle texture =
        device.createTexture(atlas.textureWidth(), atlas.textureHeight(), composite.data());
    if (texture == rhi::kInvalidTexture) {
        std::fprintf(stderr, "не удалось загрузить атлас: %s\n", device.lastError().c_str());
        return 1;
    }

    // Шрифт. Отдельная текстура: у глифов своя сетка и свой размер клетки,
    // и паковать их вместе с кораблями значило бы усложнить жизнь обоим.
    render::Font font;
    const char* fontCandidates[] = {"assets/build/font.json", "../assets/build/font.json",
                                    "../../assets/build/font.json"};
    for (const char* path : fontCandidates) {
        if (font.load(path)) break;
    }
    rhi::TextureHandle fontTexture = rhi::kInvalidTexture;
    if (font.valid()) {
        fontTexture = device.createTexture(font.textureWidth(), font.textureHeight(),
                                           font.pixels().data());
    }
    if (fontTexture == rhi::kInvalidTexture) {
        // Игра без надписей работает, но играть в неё нельзя: игрок не
        // видит своих ресурсов. Это не предупреждение, а отказ.
        std::fprintf(stderr,
                     "шрифт не загружен: %s\n"
                     "соберите ассеты: tools/blender/build_assets.py --quality preview\n",
                     font.error().c_str());
        return 1;
    }

    if (!net::initialiseSockets()) {
        std::fprintf(stderr, "не удалось поднять сетевую подсистему\n");
        return 1;
    }
    net::Socket socket;
    if (!socket.open(0)) {
        std::fprintf(stderr, "%s\n", socket.error().c_str());
        return 1;
    }

    game::Client client;
    client.connect(server, name, nowMilliseconds());

    render::MapView mapView;
    mapView.setAtlas(&atlas);
    render::MapFrame frame;
    render::Selection selection;

    render::Hud hud;
    hud.setFont(&font);
    render::HudFrame hudFrame;

    CameraControl camera;
    Input input;

    bool framed = false;   // подогнали ли камеру под галактику
    int64_t nextStatus = 0;
    uint32_t selectedFleet = 0xFFFFFFFFu;

    std::printf("подключаюсь к %s...\n", server.toString().c_str());
    printUsage();

    const int64_t shotAt = int64_t(shotAfterSeconds) * 1000;
    bool shotTaken = false;

    while (window.pumpEvents(input)) {
        const int64_t now = nowMilliseconds();
        if (headless && shotTaken) break;

        // --- сеть ---
        uint8_t packet[net::kMaxPacketSize];
        net::Address from;
        for (int drained = 0; drained < 64; ++drained) {
            const size_t size = socket.receive(from, packet, sizeof(packet));
            if (size == 0) break;
            if (from != server) continue;
            client.receive(packet, size, now);
        }

        if (client.state() == net::ConnectionState::Failed) {
            std::fprintf(stderr, "%s\n",
                         client.reason() == net::DisconnectReason::Rejected
                             ? "сервер отказал в подключении"
                             : "сервер не отвечает");
            break;
        }

        for (const game::ClientEvent& event : client.takeEvents()) {
            std::printf("· %s (система %u)\n", noticeText(event.kind), event.system);
            std::fflush(stdout);
        }

        // --- камера ---
        //
        // Подгоняем один раз, как только узнали размер галактики: игрок
        // должен увидеть карту целиком, а не кусок пустоты.
        if (client.ready() && !framed) {
            framed = true;
            const float extent = float(client.galaxy().extent().toDouble());
            camera.worldHeight = extent * 2.0f;
            camera.maxHeight = extent * 3.0f;
            camera.minHeight = std::max(60.0f, extent * 0.05f);
            camera.centerX = 0.0f;
            camera.centerY = 0.0f;
            if (headless) {
                // В снимке выделяем столицу: иначе панель сведений
                // о системе не рисуется, и проверка в CI не увидела бы
                // половину интерфейса.
                selection.system = client.capital();
                const auto own = client.fleetsAt(client.capital());
                if (!own.empty()) selectedFleet = own.front();
            }
            if (headless && shotZoom > 1.0f) {
                // Снимок крупным планом: смотрим на свою столицу. Нужен,
                // чтобы разглядеть то, что на общем плане не видно, —
                // класс светила, состав флота, полосу осады.
                camera.worldHeight /= shotZoom;
                camera.centerX = float(client.galaxy().positionX(client.capital()).toDouble());
                camera.centerY = float(client.galaxy().positionY(client.capital()).toDouble());
            }
            window.setTitle(("PlanetWar — " + name).c_str());
        }

        int width = device.targetWidth(), height = device.targetHeight();
        if (!headless) window.framebufferSize(width, height);

        if (input.wasPressed(Key::Escape)) break;
        if (input.wheel() != 0.0f) camera.zoom(input.wheel());
        if (input.wasPressed(Key::Space) && client.ready()) {
            camera.worldHeight = float(client.galaxy().extent().toDouble()) * 2.0f;
            camera.centerX = 0.0f;
            camera.centerY = 0.0f;
        }

        // Панорамирование правой кнопкой: перемещение в пикселях
        // переводится в мировые единицы через текущий зум, иначе на
        // дальнем плане карта ползала бы неощутимо медленно.
        if (input.isDown(MouseButton::Right) && height > 0) {
            const float perPixel = camera.worldHeight / float(height);
            camera.centerX -= input.mouseDeltaX() * perPixel;
            camera.centerY += input.mouseDeltaY() * perPixel;
        }

        // --- указатель ---
        float worldX = 0.0f, worldY = 0.0f;
        camera.toWorld(input.mouseX(), input.mouseY(), width, height, worldX, worldY);

        uint32_t under = 0xFFFFFFFFu;
        if (client.ready()) {
            under = render::MapView::pick(client.galaxy(), worldX, worldY, camera.worldHeight);
        }
        selection.hoverSystem = selectedFleet != 0xFFFFFFFFu ? under : 0xFFFFFFFFu;

        if (client.ready() && input.wasPressed(MouseButton::Left) && under != 0xFFFFFFFFu) {
            if (selectedFleet != 0xFFFFFFFFu && under != selection.system) {
                // Второй клик по другой системе — это приказ. Ответ придёт
                // снапшотом: клиент ничего не двигает сам.
                if (client.orderMove(selectedFleet, under)) {
                    std::printf("приказ: флот %u -> система %u\n", selectedFleet, under);
                }
                selectedFleet = 0xFFFFFFFFu;
                selection.system = under;
            } else {
                selection.system = under;
                const auto own = client.fleetsAt(under);
                selectedFleet = own.empty() ? 0xFFFFFFFFu : own.front();

                // Подробности показывает панель на экране — печатать их
                // ещё и в терминал значит разделить внимание игрока
                // между двумя окнами.
            }
        }
        selection.fleet = selectedFleet;

        // --- приказы с клавиатуры ---
        if (client.ready() && selection.system < client.galaxy().systemCount()) {
            const Key buildKeys[] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4,
                                     Key::Num5, Key::Num6, Key::Num7, Key::Num8};
            for (int i = 0; i < 8; ++i) {
                if (!input.wasPressed(buildKeys[i])) continue;
                // Первый свободный слот первой планеты: полноценный
                // конструктор застройки — работа интерфейса Фазы 3,
                // здесь важно, что путь «нажал — построилось» замкнут.
                for (const auto& planet : client.planetsAt(selection.system)) {
                    bool placed = false;
                    for (uint8_t slot = 0; slot < planet.slots; ++slot) {
                        if (planet.buildings[slot] != uint8_t(sim::Building::None)) continue;
                        const sim::Building what = sim::Building(i + 1);
                        if (client.orderBuildBuilding(planet.id, slot, what)) {
                            std::printf("строю %s на планете %u, слот %u\n", buildingName(uint8_t(what)),
                                        planet.id, slot);
                            std::fflush(stdout);
                        }
                        placed = true;
                        break;
                    }
                    if (placed) break;
                }
            }

            const Key shipKeys[] = {Key::Q, Key::W, Key::E, Key::R};
            const sim::Hull hulls[] = {sim::Hull::Corvette, sim::Hull::Destroyer,
                                       sim::Hull::Cruiser, sim::Hull::Battleship};
            for (int i = 0; i < 4; ++i) {
                if (!input.wasPressed(shipKeys[i])) continue;
                if (client.orderBuildShip(selection.system, hulls[i], 1)) {
                    std::printf("заказан %s в системе %u\n", hullName(hulls[i]),
                                selection.system);
                    std::fflush(stdout);
                }
            }
        }

        // --- отправка ---
        const size_t outgoing = client.update(now, packet, sizeof(packet));
        if (outgoing > 0) socket.send(server, packet, outgoing);

        // --- кадр ---
        rhi::ClearColor clear;
        clear.r = 0.035f;
        clear.g = 0.043f;
        clear.b = 0.063f;
        if (!device.beginFrame(clear)) break;

        device.setCamera(camera.toRhi());
        if (client.ready()) {
            mapView.build(client.galaxy(), client.view(), client.empire(), selection,
                          camera.toRhi(), frame);
            // Линии первыми: звёзды и корабли ложатся поверх.
            device.drawLines(frame.lines.data(), frame.lines.size());
            device.drawSprites(frame.sprites.data(), frame.sprites.size(), texture);
        }

        // Панели поверх карты, в ЭКРАННЫХ координатах.
        //
        // Камера меняется между вызовами отрисовки — отдельного конвейера
        // для интерфейса не нужно. Начало в левом верхнем углу, ось Y
        // вниз, единица — пиксель: то есть обычные экранные координаты.
        hud.build(client, selection, width, height, hudFrame);
        if (!hudFrame.sprites.empty()) {
            rhi::Camera screen;
            screen.centerX = float(width) * 0.5f;
            screen.centerY = float(height) * 0.5f;
            screen.worldHeight = float(height);
            screen.yDown = true;
            device.setCamera(screen);
            device.drawSprites(hudFrame.sprites.data(), hudFrame.sprites.size(), fontTexture);
        }
        if (!device.endFrame()) break;

        // Снимок: подключились, поиграли заданное время, отрисовали карту.
        if (headless && !shotTaken && now >= shotAt) {
            shotTaken = true;
            if (!client.ready()) {
                std::fprintf(stderr, "снимок не сделан: сервер не ответил\n");
                device.shutdown();
                shutdownPlatform();
                return 1;
            }
            std::vector<Rgba8> pixels;
            if (!device.readback(pixels) ||
                !writePng(shotPath, pixels, device.targetWidth(), device.targetHeight())) {
                std::fprintf(stderr, "не удалось записать %s\n", shotPath.c_str());
                device.shutdown();
                shutdownPlatform();
                return 1;
            }
            std::printf("карта отрисована в %s (%dx%d): систем %u, спрайтов %zu, линий %zu\n",
                        shotPath.c_str(), device.targetWidth(), device.targetHeight(),
                        client.galaxy().systemCount(), frame.sprites.size(),
                        frame.lines.size() / 2);
        }

        // Заголовок окна: имя игрока и число систем. Всё остальное
        // теперь на экране, в панелях.
        if (client.ready() && now >= nextStatus) {
            nextStatus = now + 5000;
            uint32_t systems = 0;
            for (const auto& system : client.view().systems) {
                if (system.owner == uint8_t(client.empire())) ++systems;
            }
            window.setTitle((name + " · систем " + std::to_string(systems)).c_str());
        }
    }

    // Прощаемся: сервер освободит место сразу, а не через пять секунд.
    net::Connection farewell;
    uint8_t bye[64];
    const size_t byeSize = farewell.buildDisconnect(bye, sizeof(bye));
    if (byeSize > 0) socket.send(server, bye, byeSize);

    device.shutdown();
    socket.close();
    net::shutdownSockets();
    shutdownPlatform();
    return 0;
}
