// pw_game_client — играбельный клиент PlanetWar.
//
// ВСЁ ДЕЛАЕТСЯ МЫШЬЮ. Клавиши остались как ускорители для тех, кто их
// выучит, но ни одно действие не требует их знать: выбрать систему,
// войти в неё, выбрать планету и слот, построить здание, заказать
// корабль, отправить флот — всё это кнопки и карточки на экране.
//
// Игрок, впервые открывший игру, обязан суметь построить шахту, не читая
// подсказок и не угадывая, что цифра «1» что-то значит. Это не удобство,
// а условие: в стратегии, где половина действий спрятана в клавиатуре,
// новый игрок не доживает до момента, когда игра становится интересной.
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
#include "pw/render/map_view.h"
#include "pw/render/screen.h"
#include "pw/render/system_view.h"
#include "pw/render/ui.h"
#include "pw/rhi/rhi.h"

using namespace pw;

namespace {

// SPIR-V вкомпилирован в бинарь: на Android ассеты лежат внутри apk,
// на iOS в бандле, на десктопе рядом с программой. Массив одинаков везде.
#include "pw/rhi/shaders.inc"

constexpr uint32_t kNoSystem = 0xFFFFFFFFu;
constexpr uint32_t kNoFleet = 0xFFFFFFFFu;
constexpr uint8_t kNoSlot = 0xFF;

int64_t nowMilliseconds() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - start).count();
}

std::vector<uint8_t> spirv(const unsigned char* data, size_t size) {
    return std::vector<uint8_t>(data, data + size);
}

/// Найти файл рядом с программой или в дереве исходников.
///
/// При разработке запускают из корня репозитория, в собранной игре —
/// из папки рядом с исполняемым файлом.
template <typename Loader>
bool loadFromCandidates(const char* relative, Loader&& loader) {
    const std::string prefixes[] = {"", "../", "../../"};
    for (const std::string& prefix : prefixes) {
        if (loader(prefix + relative)) return true;
    }
    return false;
}

/// Камера карты галактики.
///
/// Зум ограничен с обеих сторон намеренно. Слишком близко — игрок теряет
/// карту из виду и не может принимать решения; слишком далеко — звёзды
/// сливаются в кашу, и по ней нельзя щёлкнуть.
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

void printUsage() {
    std::printf(
        "pw_game_client — клиент PlanetWar\n\n"
        "  --host <адрес>   сервер (по умолчанию 127.0.0.1)\n"
        "  --port <n>       порт (по умолчанию 27015)\n"
        "  --name <имя>     имя игрока\n"
        "  --width <n>      ширина окна\n"
        "  --height <n>     высота окна\n"
        "  --validation     слои проверки Vulkan\n"
        "  --shot <файл>    без окна: подключиться, отрисовать и выйти\n"
        "  --shot-after <с> сколько секунд поиграть до снимка\n"
        "  --shot-cursor <x> <y>  поставить курсор перед снимком —\n"
        "                   так на снимок попадают подсветка и подсказка\n"
        "  --shot-press     щёлкнуть в этой точке до снимка: проверить,\n"
        "                   что кнопка делает и не падает\n"
        "  --sweep <шаг>    без окна: обойти весь экран щелчками с этим шагом\n"
        "                   в обоих видах и выйти. Ищет падения, а не картинку\n"
        "  --shot-zoom <k>  приблизить перед снимком\n"
        "  --shot-system    снимок ВИДА СИСТЕМЫ, а не карты галактики\n"
        "\nВСЁ УПРАВЛЕНИЕ — МЫШЬЮ. Ни одно действие не требует клавиши:\n"
        "  левая            выбрать систему, планету, слот; нажать кнопку\n"
        "  двойной щелчок   открыть систему\n"
        "  правая           тянуть карту, в системе — вращать камеру\n"
        "  колесо           приблизить\n"
        "  наведение        подсказка: что это и сколько стоит\n"
        "\nКлавиши только ускоряют то, что и так лежит под курсором:\n"
        "  Escape           снять приказ, выйти из системы\n"
        "  Enter            войти в выбранную систему и обратно\n"
        "  Tab              следующая планета системы\n");
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
    // Куда поставить курсор перед снимком.
    //
    // Без этого состояния наведения — подсветка строки, подсказка,
    // объясняющая «что и зачем», — нельзя увидеть на снимке ВООБЩЕ.
    // Проверять их оставалось только запуском игры руками, то есть
    // не проверять: правило проекта «снимки ловят замысел» на половину
    // интерфейса просто не распространялось.
    float shotCursorX = -1.0f, shotCursorY = -1.0f;
    // Нажать в этой точке за секунду до снимка.
    //
    // Без этого КНОПКИ НЕ ПРОВЕРЯЕМЫ ВООБЩЕ. Безоконный режим умел
    // поставить курсор, но не умел щёлкнуть, поэтому всё, что случается
    // ПОСЛЕ нажатия — новое состояние экрана, приказ, падение, — можно
    // было увидеть только руками. Игрок и увидел: игра падала на кнопке
    // «Отправить флот», а прогон был зелёный.
    bool shotPress = false;
    // Обход всего экрана щелчками.
    //
    // Кнопка «Отправить флот» роняла игру, а прогон был зелёный: тесты
    // проверяют, ЧТО возвращает экран, но не проверяют, переживёт ли
    // клиент это намерение целиком — от щелчка до следующего кадра.
    // Между экраном и падением лежат обработка намерения, приказ,
    // смена вида и отрисовка, и ни один юнит-тест их не связывает.
    //
    // Обход связывает: он щёлкает во ВСЕ точки сетки в обоих видах
    // и заканчивается кодом ноль, только если клиент дожил до конца.
    int sweepStep = 0;
    bool shotSystem = false;

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
        else if (arg == "--shot-cursor" && i + 2 < argc) {
            shotCursorX = float(std::atof(argv[++i]));
            shotCursorY = float(std::atof(argv[++i]));
        }
        else if (arg == "--shot-press") shotPress = true;
        else if (arg == "--sweep" && i + 1 < argc) sweepStep = std::atoi(argv[++i]);
        else if (arg == "--shot-system") shotSystem = true;
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
    // интерфейс автоматически: отрисовать кадр в CI, посчитать пиксели
    // и сравнить с ожидаемым. Иначе экран проверяется только глазами
    // и только тогда, когда кто-то удосужился посмотреть.
    const bool headless = !shotPath.empty() || sweepStep > 0;

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
                                   spirv(kLineFrag, sizeof(kLineFrag))) ||
        !device.createMeshPipeline(spirv(kMeshVert, sizeof(kMeshVert)),
                                   spirv(kMeshFrag, sizeof(kMeshFrag))) ||
        !device.createGlowPipeline(spirv(kMeshVert, sizeof(kMeshVert)),
                                   spirv(kMeshFrag, sizeof(kMeshFrag)))) {
        std::fprintf(stderr, "не удалось собрать конвейеры: %s\n", device.lastError().c_str());
        return 1;
    }

    // --- ассеты ---
    render::Atlas atlas;
    if (!loadFromCandidates("assets/build/ships.json",
                            [&](const std::string& path) { return atlas.load(path); })) {
        std::fprintf(stderr,
                     "атлас кораблей не найден: %s\n"
                     "соберите ассеты: tools/blender/build_assets.py --quality preview\n",
                     atlas.error().c_str());
        return 1;
    }
    const std::vector<Rgba8> composite = atlas.composite();
    const rhi::TextureHandle shipTexture =
        device.createTexture(atlas.textureWidth(), atlas.textureHeight(), composite.data());

    render::Font font;
    loadFromCandidates("assets/build/font.json",
                       [&](const std::string& path) { return font.load(path); });
    rhi::TextureHandle fontTexture = rhi::kInvalidTexture;
    if (font.valid()) {
        fontTexture = device.createTexture(font.textureWidth(), font.textureHeight(),
                                           font.pixels().data());
    }

    render::UiAtlas uiAtlas;
    loadFromCandidates("assets/build/ui.json",
                       [&](const std::string& path) { return uiAtlas.load(path); });
    rhi::TextureHandle uiTexture = rhi::kInvalidTexture;
    if (uiAtlas.valid()) {
        uiTexture = device.createTexture(uiAtlas.textureWidth(), uiAtlas.textureHeight(),
                                         uiAtlas.pixels().data());
    }

    if (shipTexture == rhi::kInvalidTexture || fontTexture == rhi::kInvalidTexture ||
        uiTexture == rhi::kInvalidTexture) {
        // Игра без надписей и кнопок запускается, но играть в неё нельзя:
        // игрок не видит ни ресурсов, ни того, куда нажимать. Это отказ,
        // а не предупреждение.
        std::fprintf(stderr,
                     "ассеты интерфейса не загружены: %s %s\n"
                     "соберите: tools/blender/build_assets.py --quality preview\n",
                     font.error().c_str(), uiAtlas.error().c_str());
        return 1;
    }

    render::SystemAssets systemAssets;
    if (!loadFromCandidates("assets/build/planets.json",
                            [&](const std::string& path) { return systemAssets.load(path); }) ||
        !systemAssets.upload(device)) {
        std::fprintf(stderr,
                     "планеты не загружены: %s\n"
                     "соберите ассеты: tools/blender/build_assets.py --quality preview\n",
                     systemAssets.error().c_str());
        return 1;
    }

    // --- сеть ---
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

    // --- вид ---
    render::MapView mapView;
    mapView.setAtlas(&atlas);
    render::MapFrame mapFrame;

    render::SystemView systemView;
    systemView.setAssets(&systemAssets);
    render::SystemFrame systemFrame;
    render::SystemCamera systemCamera;

    render::Ui ui;
    ui.setFont(&font);
    ui.setAtlas(&uiAtlas);

    render::Screen screen;
    render::MessageLog messages;
    screen.setMessages(&messages);
    render::ScreenState state;

    CameraControl camera;
    Input input;

    const render::TextColor kInfo{0.86f, 0.88f, 0.92f, 1.0f};
    const render::TextColor kGood{0.62f, 0.84f, 0.52f, 1.0f};
    const render::TextColor kBad{0.88f, 0.45f, 0.45f, 1.0f};

    bool framed = false;
    bool quit = false;
    int64_t nextStatus = 0;
    int64_t lastClickAt = -1000;
    uint32_t lastClickSystem = kNoSystem;

    // Полная памятка — по --help. Всё управление и так на экране: сыпать
    // при каждом запуске таблицу клавиш в терминал значит признавать, что
    // без неё интерфейс не работает.
    std::printf("подключаюсь к %s... управление мышью, памятка: --help\n",
                server.toString().c_str());

    const int64_t shotAt = int64_t(shotAfterSeconds) * 1000;
    bool shotTaken = false;
    // Состояние обхода: номер точки и фаза щелчка внутри неё.
    int sweepPoint = 0;
    int sweepPhase = 0;
    int sweepClicks = 0;
    // Сколько раз обход добрался до каждого намерения.
    //
    // Без этого счётчика обход отвечает только «жив», но не «а всё ли он
    // потрогал». Зелёный прогон, ни разу не нажавший на кнопку, —
    // это и есть та самая молча проходящая проверка.
    int sweepActions[int(render::ActionKind::Count)] = {};
    int sweepOrders = 0;
    bool sweepFailed = false;

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
            std::string text = render::noticeText(event.kind);
            if (event.kind != game::NoticeKind::FleetDestroyed) {
                text += " · система " + std::to_string(event.system);
            }
            messages.add(text, render::noticeIsBad(event.kind) ? kBad : kGood, now,
                         event.kind == game::NoticeKind::FleetDestroyed ? kNoSystem
                                                                        : event.system,
                         render::noticeIcon(event.kind));
        }
        messages.update(now);

        int width = device.targetWidth(), height = device.targetHeight();
        if (!headless) window.framebufferSize(width, height);

        // --- первая настройка камеры ---
        if (client.ready() && !framed) {
            framed = true;
            const float extent = float(client.galaxy().extent().toDouble());
            camera.maxHeight = extent * 3.0f;
            camera.minHeight = std::max(60.0f, extent * 0.05f);
            // Начинаем НАД СВОИМ ДОМОМ, а не над всей галактикой. Вся
            // галактика в первом кадре — это сотня одинаковых точек, среди
            // которых игрок ищет себя; кнопка «Вся галактика» внизу отдаёт
            // этот вид в одно нажатие, когда он действительно понадобится.
            camera.worldHeight = std::clamp(extent * 0.9f, camera.minHeight,
                                            camera.maxHeight);
            camera.centerX = float(client.galaxy().positionX(client.capital()).toDouble());
            camera.centerY = float(client.galaxy().positionY(client.capital()).toDouble());
            state.system = client.capital();
            const auto own = client.fleetsAt(client.capital());
            if (!own.empty()) state.fleet = own.front();

            // Первое, что человек видит, — ответ на вопрос «где я и что
            // делать». Без него игрок оказывается посреди сотни звёзд
            // и первым делом ищет себя, а не играет.
            messages.add("ваш дом — система " + std::to_string(client.capital()),
                         kGood, now, client.capital(), "icon_planet");
            messages.add("стройте шахты: минералы — основа всего", kInfo, now,
                         kNoSystem, "bld_mine");

            if (headless && shotSystem) {
                state.inSystem = true;
                systemCamera.focusOrbit = 0xFFFFFFFFu;
                systemCamera.distance =
                    render::fitDistance(client.galaxy().planetCount(client.capital()));
                if (shotZoom > 1.0f) {
                    systemCamera.distance /= shotZoom;
                    systemCamera.focusOrbit = 0;
                    state.planetIndex = 0;
                }
                state.slot = 0;
                for (const auto& planet : client.planetsAt(client.capital())) {
                    client.orderBuildBuilding(planet.id, 0, sim::Building::Mine);
                    if (planet.slots > 1) {
                        client.orderBuildBuilding(planet.id, 1, sim::Building::PowerPlant);
                    }
                }
            } else if (headless && shotZoom > 1.0f) {
                camera.worldHeight /= shotZoom;
                camera.centerX = float(client.galaxy().positionX(client.capital()).toDouble());
                camera.centerY = float(client.galaxy().positionY(client.capital()).toDouble());
            }
            window.setTitle(("PlanetWar — " + name).c_str());
        }

        // --- обход щелчками ---
        //
        // Три кадра на точку: нажали, подержали, отпустили. Кнопка
        // срабатывает по ОТПУСКАНИЮ и только если курсор всё ещё на ней,
        // поэтому щелчок «в один кадр» проверял бы не то, что делает
        // человек.
        //
        // Щелчок кладётся В САМ ВВОД, а не в структуру интерфейса. Первая
        // версия обхода подменяла поля `UiInput` — и следующие же три
        // строки, заполнявшие их из настоящей мыши, затирали подмену.
        // Обход честно печатал «щелчков 1760», не сделав НИ ОДНОГО: он
        // считал свои намерения, а не то, что дошло до игры. Проверка,
        // которая молча проходит, хуже отсутствующей — эта прошла дважды,
        // в том числе под санитайзером, пока падение лежало на месте.
        if (sweepStep > 0 && client.ready()) {
            const int columns = std::max(1, width / sweepStep);
            const int rows = std::max(1, height / sweepStep);
            const int gridTotal = columns * rows;

            // СЕТКА ПЛЮС ЗОНДЫ, а не одна сетка.
            //
            // Сетка с шагом 38 проходит МИМО кнопки высотой 28: строка
            // сетки просто не попадает в её полосу. Именно так обход
            // и не доходил до кнопки «Отправить флот» — той самой,
            // на которой игра падала. Шаг, меньший самого мелкого
            // элемента, стоил бы четырёх минут на прогон.
            //
            // Поэтому после двух сеточных проходов идут два ЗОНДА:
            // вертикальные линии с мелким шагом по левому столбцу
            // и по правому. Панели выстроены столбцами, все их кнопки
            // лежат на этих двух линиях, и попасть по ним — уже не
            // вопрос удачи. Точек у зондов меньше двухсот.
            const int probeStep = std::max(4, sweepStep / 6);
            const int probeRows = std::max(1, height / probeStep);
            // ТРИ линии, а не две: левый столбец, ПОСЕРЕДИНЕ КАРТА,
            // правый столбец. Средняя линия нужна не для симметрии:
            // взведённый приказ ждёт щелчка ПО КАРТЕ, и без него путь
            // «нажал отправить → указал цель» обрывается на середине —
            // ровно там, где игра и падала. Линия по карте его дожимает.
            const int probeTotal = probeRows * 3;
            const int totalPoints = gridTotal * 2 + probeTotal;

            if (sweepPoint >= totalPoints) {
                std::printf("обход закончен: %d точек сеткой и %d зондом, "
                            "щелчков %d — клиент жив\n",
                            gridTotal * 2, probeTotal, sweepClicks);
                std::printf("  намерения:");
                for (int k = 1; k < int(render::ActionKind::Count); ++k) {
                    if (sweepActions[k] == 0) continue;
                    std::printf(" %s×%d", render::actionName(render::ActionKind(k)),
                                sweepActions[k]);
                }
                std::printf("\n  приказов флоту отдано: %d\n", sweepOrders);
                if (sweepActions[int(render::ActionKind::BeginMove)] == 0) {
                    // Молчаливо неполный обход — это не обход. Кнопка,
                    // с которой начался весь разбор, обязана быть нажата.
                    std::fprintf(stderr,
                                 "обход не засчитан: до кнопки «Отправить флот» "
                                 "так и не дошли\n");
                    sweepFailed = true;
                }
                break;
            }

            const bool probing = sweepPoint >= gridTotal * 2;
            const bool wantSystem = !probing && sweepPoint >= gridTotal;
            if (state.inSystem != wantSystem &&
                state.system < client.galaxy().systemCount()) {
                state.inSystem = wantSystem;
                if (wantSystem) {
                    systemCamera = render::SystemCamera{};
                    systemCamera.focusOrbit = 0xFFFFFFFFu;
                    systemCamera.distance =
                        render::fitDistance(client.galaxy().planetCount(state.system));
                }
            }

            float pointX = 0.0f;
            float pointY = 0.0f;
            if (probing) {
                // Столица и флот в ней закреплены на каждом кадре: панель
                // флота обязана быть на месте, иначе кнопки, которую мы
                // ищем, на экране просто нет.
                state.system = client.capital();
                const auto own = client.fleetsAt(state.system);
                if (!own.empty()) state.fleet = own.front();

                const int at = sweepPoint - gridTotal * 2;
                const int lane = at / probeRows;
                const int step = at % probeRows;
                const float lanes[] = {0.12f, 0.50f, 0.88f};
                pointX = float(width) * lanes[lane < 3 ? lane : 2];
                pointY = float(step * probeStep + probeStep / 2);
            } else {
                const int at = sweepPoint % gridTotal;
                pointX = float((at % columns) * sweepStep + sweepStep / 2);
                pointY = float((at / columns) * sweepStep + sweepStep / 2);
            }

            input.setMouse(pointX, pointY);
            input.setButton(MouseButton::Left, sweepPhase <= 1);
            if (sweepPhase == 2) ++sweepClicks;
            if (++sweepPhase > 2) {
                sweepPhase = 0;
                ++sweepPoint;
            }
        } else if (headless && shotCursorX >= 0.0f) {
            // Курсор подменяем в САМОМ вводе, а не только в интерфейсе.
            //
            // Подменяя лишь интерфейс, я проверял половину картины: мир
            // по-прежнему считал, что курсора нет, и всё, что зависит от
            // него за пределами панелей — подсветка системы под курсором,
            // линия к цели приказа, выбор планеты — оставалось
            // непроверенным.
            input.setMouse(shotCursorX, shotCursorY);
            if (shotPress) {
                // Нажатие и отпускание разносим по кадрам по той же
                // причине, что и в обходе.
                const int64_t left = shotAt - now;
                input.setButton(MouseButton::Left, left <= 900 && left > 450);
            }
        }

        // Что видно на карте — экрану, для рамки «вы здесь» на мини-карте.
        // Считается из ТОЙ ЖЕ камеры, которой рисуется мир, а не из своих
        // чисел: два источника правды о видимой области разошлись бы
        // в первый же день, и рамка начала бы врать.
        state.viewCenterX = camera.centerX;
        state.viewCenterY = camera.centerY;
        state.viewHeight = camera.worldHeight;
        state.viewWidth =
            camera.worldHeight * (height > 0 ? float(width) / float(height) : 1.0f);

        // --- интерфейс ---
        //
        // Собирается ПЕРВЫМ, до разбора щелчков по миру: экран обязан
        // успеть сказать, забрал ли он мышь. Иначе нажатие на кнопку
        // заодно отдаёт приказ флоту в системе под ней.
        render::UiInput uiInput;
        uiInput.mouseX = input.mouseX();
        uiInput.mouseY = input.mouseY();
        uiInput.down = input.isDown(MouseButton::Left);
        uiInput.pressed = input.wasPressed(MouseButton::Left);
        uiInput.released = input.wasReleased(MouseButton::Left);

        ui.begin(uiInput, width, height);
        const render::ScreenAction action = screen.build(ui, client, state, now);
        ui.end();
        if (sweepStep > 0 && action.kind < render::ActionKind::Count) {
            ++sweepActions[int(action.kind)];
        }

        // --- намерения из интерфейса ---
        switch (action.kind) {
            case render::ActionKind::EnterSystem:
                if (state.system < client.galaxy().systemCount()) {
                    state.inSystem = true;
                    systemCamera = render::SystemCamera{};
                    systemCamera.focusOrbit = 0xFFFFFFFFu;
                    systemCamera.distance =
                        render::fitDistance(client.galaxy().planetCount(state.system));
                }
                break;
            case render::ActionKind::LeaveSystem:
                state.inSystem = false;
                break;
            case render::ActionKind::SelectPlanet:
                state.planetIndex = action.value;
                state.slot = kNoSlot;
                if (state.inSystem) systemCamera.focusOrbit = action.value;
                break;
            case render::ActionKind::SelectSlot:
                state.slot = action.slot;
                break;
            case render::ActionKind::Build:
                if (client.orderBuildBuilding(action.planet, action.slot,
                                              sim::Building(action.value))) {
                    messages.add(std::string("строю ") +
                                     render::buildingNameAccusative(uint8_t(action.value)),
                                 kInfo, now, state.system,
                                 render::buildingIcon(uint8_t(action.value)));
                    state.slot = kNoSlot;
                }
                break;
            case render::ActionKind::Demolish:
                if (client.orderBuildBuilding(action.planet, action.slot,
                                              sim::Building::None)) {
                    messages.add("снесено", kInfo, now, state.system, "icon_demolish");
                }
                break;
            case render::ActionKind::CancelBuild:
                // Отмена — это снос в том же слоте: сервер понимает
                // Building::None и как отмену начатого, и как снос готового.
                if (state.system < client.galaxy().systemCount()) {
                    const auto planets = client.planetsAt(state.system);
                    if (state.planetIndex < planets.size() &&
                        planets[state.planetIndex].building()) {
                        client.orderBuildBuilding(action.planet,
                                                  planets[state.planetIndex].buildSlot,
                                                  sim::Building::None);
                        messages.add("стройка отменена", kInfo, now, state.system);
                    }
                }
                break;
            case render::ActionKind::OrderShip:
                if (client.orderBuildShip(state.system, sim::Hull(action.value), 1)) {
                    messages.add(std::string("заказан ") +
                                     render::hullName(uint8_t(action.value)),
                                 kInfo, now, state.system,
                                 render::hullIcon(uint8_t(action.value)));
                }
                break;
            case render::ActionKind::SelectFleet:
                state.fleet = action.value;
                state.awaitingMoveTarget = false;
                break;
            case render::ActionKind::BeginMove:
                state.awaitingMoveTarget = state.fleet != kNoFleet;
                if (state.awaitingMoveTarget) state.inSystem = false;
                break;
            case render::ActionKind::CancelMove:
                state.awaitingMoveTarget = false;
                break;
            case render::ActionKind::FocusSystem:
                if (action.value < client.galaxy().systemCount()) {
                    state.system = action.value;
                    state.planetIndex = 0;
                    state.slot = kNoSlot;
                    state.inSystem = false;
                    camera.centerX =
                        float(client.galaxy().positionX(action.value).toDouble());
                    camera.centerY =
                        float(client.galaxy().positionY(action.value).toDouble());
                    camera.worldHeight = std::max(camera.minHeight, camera.worldHeight * 0.5f);
                }
                break;
            case render::ActionKind::ResetView:
                if (state.inSystem) {
                    systemCamera.focusOrbit = 0xFFFFFFFFu;
                    systemCamera.distance =
                        render::fitDistance(client.galaxy().planetCount(state.system));
                } else if (client.ready()) {
                    camera.worldHeight = float(client.galaxy().extent().toDouble()) * 2.0f;
                    camera.centerX = 0.0f;
                    camera.centerY = 0.0f;
                }
                break;
            case render::ActionKind::Quit:
                // Обход НЕ ИМЕЕТ ПРАВА закрыть игру.
                //
                // Он щёлкает по всему экрану подряд, в том числе дважды
                // подряд по кнопке «Выход» — а два щелчка по ней это
                // и есть подтверждённый выход. Обход честно завершался
                // кодом ноль, не дойдя и до четверти точек: снаружи это
                // выглядело как успешная проверка. Намерение считается
                // (оно попадает в отчёт), но не исполняется.
                if (sweepStep == 0) quit = true;
                break;
            case render::ActionKind::Count:
            case render::ActionKind::None:
                break;
        }
        if (quit) break;

        // --- клавиши-ускорители ---
        //
        // Каждая дублирует кнопку на экране. Ни одного действия, которое
        // делается ТОЛЬКО клавишей, здесь нет и быть не должно.
        if (input.wasPressed(Key::Escape)) {
            if (state.awaitingMoveTarget) state.awaitingMoveTarget = false;
            else if (state.inSystem) state.inSystem = false;
            else break;
        }
        if (input.wasPressed(Key::Enter) && client.ready() &&
            state.system < client.galaxy().systemCount()) {
            state.inSystem = !state.inSystem;
            if (state.inSystem) {
                systemCamera = render::SystemCamera{};
                systemCamera.focusOrbit = 0xFFFFFFFFu;
                systemCamera.distance =
                    render::fitDistance(client.galaxy().planetCount(state.system));
            }
        }
        if (input.wasPressed(Key::Tab) && client.ready() &&
            state.system < client.galaxy().systemCount()) {
            const auto planets = client.planetsAt(state.system);
            if (!planets.empty()) {
                state.planetIndex = uint32_t((state.planetIndex + 1) % planets.size());
                state.slot = kNoSlot;
                if (state.inSystem) systemCamera.focusOrbit = state.planetIndex;
            }
        }

        // --- мир: только если интерфейс не забрал мышь ---
        const bool worldInput = !ui.wantsMouse();

        if (worldInput && input.wheel() != 0.0f) {
            if (state.inSystem) {
                systemCamera.distance *= std::pow(0.85f, input.wheel());
                systemCamera.distance = std::clamp(systemCamera.distance, 6.0f, 220.0f);
            } else {
                camera.zoom(input.wheel());
            }
        }

        // Правая кнопка: тянуть карту или вращать камеру в системе.
        // Тянуть, а не двигать рывками: рука ожидает, что карта поедет
        // за курсором, и любое другое поведение ощущается сломанным.
        if (input.isDown(MouseButton::Right) && width > 0 && height > 0) {
            if (state.inSystem) {
                systemCamera.yawTurns -= input.mouseDeltaX() / float(width);
                systemCamera.pitchTurns += input.mouseDeltaY() / float(height) * 0.5f;
                // Через полюс камеру не пускаем: там она переворачивается,
                // и игрок теряет ориентацию в системе, где всё круглое.
                systemCamera.pitchTurns = std::clamp(systemCamera.pitchTurns, 0.005f, 0.24f);
            } else {
                const float perPixel = camera.worldHeight / float(height);
                camera.centerX -= input.mouseDeltaX() * perPixel;
                camera.centerY += input.mouseDeltaY() * perPixel;
            }
        }

        // --- щелчок по миру ---
        float worldX = 0.0f, worldY = 0.0f;
        camera.toWorld(input.mouseX(), input.mouseY(), width, height, worldX, worldY);

        uint32_t under = kNoSystem;
        if (client.ready() && !state.inSystem) {
            under = render::MapView::pick(client.galaxy(), worldX, worldY,
                                          camera.worldHeight);
        }

        if (client.ready() && worldInput && input.wasPressed(MouseButton::Left)) {
            if (state.inSystem) {
                // В системе щелчок выбирает планету.
                const uint32_t orbit = render::SystemView::pick(
                    systemFrame, input.mouseX() / float(std::max(1, width)),
                    input.mouseY() / float(std::max(1, height)));
                if (orbit != 0xFFFFFFFFu) {
                    state.planetIndex = orbit;
                    state.slot = kNoSlot;
                    systemCamera.focusOrbit = orbit;
                }
            } else if (under != kNoSystem) {
                if (state.awaitingMoveTarget && state.fleet != kNoFleet) {
                    // Приказ взведён — щелчок задаёт цель. Ответ придёт
                    // снапшотом: клиент ничего не двигает сам.
                    if (sweepStep > 0) ++sweepOrders;
                    if (client.orderMove(state.fleet, under)) {
                        messages.add("флот идёт в систему " + std::to_string(under), kInfo,
                                     now, under, "icon_fleet");
                    }
                    state.awaitingMoveTarget = false;
                } else {
                    // Двойной щелчок по системе открывает её. Первый
                    // выбирает, второй входит — так же, как папка
                    // в проводнике, и объяснять это не приходится.
                    const bool doubleClick =
                        under == lastClickSystem && now - lastClickAt < 350;
                    state.system = under;
                    state.planetIndex = 0;
                    state.slot = kNoSlot;
                    const auto own = client.fleetsAt(under);
                    state.fleet = own.empty() ? kNoFleet : own.front();

                    if (doubleClick) {
                        state.inSystem = true;
                        systemCamera = render::SystemCamera{};
                        systemCamera.focusOrbit = 0xFFFFFFFFu;
                        systemCamera.distance =
                            render::fitDistance(client.galaxy().planetCount(under));
                    }
                    lastClickSystem = under;
                    lastClickAt = now;
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

        if (client.ready() && state.inSystem &&
            state.system < client.galaxy().systemCount()) {
            const float aspect = height > 0 ? float(width) / float(height) : 1.0f;
            systemView.build(client.galaxy(), client.view(), state.system, client.empire(),
                             systemCamera, aspect, systemFrame);

            device.setCamera3D(systemFrame.camera);
            for (const render::MeshBatch& batch : systemFrame.batches) {
                if (batch.glow) {
                    device.drawGlow(batch.mesh, batch.instances.data(),
                                    batch.instances.size(), batch.texture);
                } else {
                    device.drawMeshes(batch.mesh, batch.instances.data(),
                                      batch.instances.size(), batch.texture);
                }
            }
        } else if (client.ready()) {
            render::Selection selection;
            selection.system = state.system;
            selection.fleet = state.fleet;
            selection.planetIndex = state.planetIndex;
            // Линия к цели тянется только когда приказ взведён: постоянная
            // линия за курсором — это шум, который игрок перестаёт видеть.
            selection.hoverSystem = state.awaitingMoveTarget ? under : kNoSystem;

            device.setCamera(camera.toRhi());
            mapView.build(client.galaxy(), client.view(), client.empire(), selection,
                          camera.toRhi(), mapFrame);
            device.drawLines(mapFrame.lines.data(), mapFrame.lines.size());
            device.drawSprites(mapFrame.sprites.data(), mapFrame.sprites.size(), shipTexture);
        }

        // Интерфейс поверх мира, в ЭКРАННЫХ координатах: начало в левом
        // верхнем углу, ось Y вниз, единица — пиксель.
        {
            rhi::Camera screenCamera;
            screenCamera.centerX = float(width) * 0.5f;
            screenCamera.centerY = float(height) * 0.5f;
            screenCamera.worldHeight = float(height);
            screenCamera.yDown = true;
            device.setCamera(screenCamera);

            for (const render::UiBatch& batch : ui.frame().batches) {
                if (batch.sprites.empty()) continue;
                device.drawSprites(batch.sprites.data(), batch.sprites.size(),
                                   batch.texture == render::UiTexture::Font ? fontTexture
                                                                            : uiTexture);
            }
        }
        if (!device.endFrame()) break;

        // --- снимок ---
        if (headless && !shotPath.empty() && !shotTaken && now >= shotAt) {
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
            std::printf("кадр отрисован в %s (%dx%d): систем %u, спрайтов интерфейса %zu\n",
                        shotPath.c_str(), device.targetWidth(), device.targetHeight(),
                        client.galaxy().systemCount(), ui.frame().spriteCount());
        }

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

    // Счёт снимаем ПОСЛЕ выключения: половина нарушений — про то, что
    // осталось неудалённым, и до `shutdown` их ещё нет. Прогон, переживший
    // обход, но нарушивший спецификацию Vulkan, зелёным не считается:
    // ровно такой прогон и пропустил падение на кнопке «Отправить флот».
    if (const uint64_t bad = rhi::Device::validationErrors(); bad > 0) {
        std::fprintf(stderr, "проверочные слои насчитали нарушений: %llu\n",
                     static_cast<unsigned long long>(bad));
        sweepFailed = true;
    }
    return sweepFailed ? 1 : 0;
}
