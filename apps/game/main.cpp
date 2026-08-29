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
#include <unordered_map>
#include <vector>

#include "pw/core/log.h"
#include "pw/game/client.h"
#include "pw/net/socket.h"
#include "pw/platform/input.h"
#include "pw/platform/platform.h"
#include "pw/platform/window.h"
#include "pw/render/atlas.h"
#include "pw/render/effects.h"
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

/// Каким эффектом мир отвечает на эту новость.
///
/// Отдельной таблицей, а не ветвлением по месту: событий одиннадцать,
/// и «что показать» для каждого — это решение дизайнера, которое должно
/// лежать там, где его можно прочитать целиком.
///
/// Часть новостей эффекта НЕ ПОЛУЧАЕТ, и это тоже решение. «Приказ
/// отвергнут» и «флот уничтожен» не привязаны к точке карты: первый
/// относится к нажатию, второй — к отряду, которого уже нет. Кольцо
/// в случайном месте было бы хуже молчания.
bool effectForNotice(game::NoticeKind kind, render::EffectKind& out, bool& ownColor) {
    using game::NoticeKind;
    switch (kind) {
        case NoticeKind::ColonyFounded:
            out = render::EffectKind::Colonized;  ownColor = true;  return true;
        case NoticeKind::PlanetCaptured:
        case NoticeKind::SystemCaptured:
            out = render::EffectKind::Captured;   ownColor = true;  return true;
        case NoticeKind::PlanetLost:
        case NoticeKind::SystemLost:
            out = render::EffectKind::Lost;       ownColor = false; return true;
        case NoticeKind::PlanetSieged:
            out = render::EffectKind::Sieged;     ownColor = false; return true;
        case NoticeKind::BattleWon:
        case NoticeKind::BattleLost:
        case NoticeKind::BattleDraw:
            out = render::EffectKind::Battle;     ownColor = false; return true;
        default:
            return false;
    }
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

    // КУДА КАМЕРА ЕДЕТ.
    //
    // Наведение по щелчку в журнале раньше ПЕРЕСТАВЛЯЛО камеру: система
    // была здесь, стала там, и связи между двумя картинками нет никакой.
    // На карте из двух сотен одинаковых точек это худший вид перехода —
    // игрок обязан заново понять, куда он попал, и первым делом ищет
    // свою территорию. Перелёт за полсекунды сохраняет связь: видно,
    // В КАКУЮ СТОРОНУ и НАСКОЛЬКО далеко мы переехали.
    //
    // Перетаскивание и колесо цель не используют: там рука игрока сама
    // задаёт движение, и сглаживание превратилось бы в задержку.
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetHeight = 2000.0f;
    /// Сглаживать ли перелёты. Выключено в безоконном режиме: снимок
    /// обязан быть устоявшимся.
    bool smooth = false;

    void zoom(float steps) {
        // Умножением, а не сложением: шаг колеса должен менять масштаб
        // одинаково и вблизи, и издалека.
        worldHeight *= std::pow(0.85f, steps);
        worldHeight = std::clamp(worldHeight, minHeight, maxHeight);
        targetHeight = worldHeight;
    }

    /// Поставить камеру немедленно. Рука игрока и первый кадр.
    void place(float x, float y, float height) {
        centerX = targetX = x;
        centerY = targetY = y;
        worldHeight = targetHeight = std::clamp(height, minHeight, maxHeight);
    }

    /// Перелететь. Без сглаживания — то же, что place.
    void flyTo(float x, float y, float height) {
        targetX = x;
        targetY = y;
        targetHeight = std::clamp(height, minHeight, maxHeight);
        if (!smooth) place(x, y, height);
    }

    /// Догнать цель за этот кадр.
    void follow(float deltaSeconds) {
        if (!smooth || deltaSeconds <= 0.0f) return;
        // Та же экспонента, что и в интерфейсе: не зависит от частоты
        // кадров и не перелетает цель. Полсекунды на весь путь — заметно
        // глазу и не заставляет ждать.
        const float k = 1.0f - std::exp(-deltaSeconds / (0.5f / 3.0f));
        centerX += (targetX - centerX) * k;
        centerY += (targetY - centerY) * k;
        worldHeight += (targetHeight - worldHeight) * k;
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
        "  --shot-squad     открыть окно отряда и набрать половину: окно\n"
        "                   открывается щелчком, и снять его иначе нечем\n"
        "  --shot-route     задать отряду маршрут, патруль и приписку:\n"
        "                   ломаная и кольца рисуются только по приказу\n"
        "  --motion <0|1>   анимация интерфейса и мира. В окне включена,\n"
        "                   в безоконном режиме выключена: снимок обязан\n"
        "                   быть устоявшимся, а не случайной фазой перехода\n"
        "\nВСЁ УПРАВЛЕНИЕ — МЫШЬЮ. Ни одно действие не требует клавиши:\n"
        "  левая            выбрать систему, планету, слот; нажать кнопку\n"
        "  протянуть левой  рамка выделения: приказ уйдёт всем отрядам в ней\n"
        "  двойной щелчок   открыть систему\n"
        "  правая по звезде приказ: выделенные отряды идут туда\n"
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
    int motionFlag = -1;   // -1 — решить по режиму
    bool shotSquad = false;
    bool shotRoute = false;
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
        else if (arg == "--shot-squad") shotSquad = true;
        else if (arg == "--shot-route") shotRoute = true;
        else if (arg == "--motion" && i + 1 < argc) motionFlag = std::atoi(argv[++i]);
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

    // РАМКА ВЫДЕЛЕНИЯ. Левая кнопка по пустому месту карты не делала
    // ничего — рамка встаёт ровно в эту дыру и не требует ни клавиши.
    // Правая занята под перетаскивание карты, и трогать её нельзя.
    bool banding = false;
    float bandStartX = 0.0f, bandStartY = 0.0f;
    // ПРАВЫЙ ЩЕЛЧОК ПО ЗВЕЗДЕ — «ИДИ ТУДА». Тот самый жест, которым
    // в RTS отдают приказ, и здесь он был свободен: правая кнопка тянула
    // карту, а щелчок правой БЕЗ протаскивания не делал ничего. Чтобы
    // одно не съело другое, смотрим, сдвинулась ли мышь между нажатием
    // и отпусканием: сдвинулась — это перетаскивание, нет — приказ.
    bool panning = false;
    bool panned = false;
    // Меньше этого протаскивание — это щелчок, а не рамка. Без порога
    // любое нажатие оставляло бы выделение из одного случайного отряда,
    // попавшего в рамку нулевого размера.
    constexpr float kBandThreshold = 6.0f;

    // Полная памятка — по --help. Всё управление и так на экране: сыпать
    // при каждом запуске таблицу клавиш в терминал значит признавать, что
    // без неё интерфейс не работает.
    std::printf("подключаюсь к %s... управление мышью, памятка: --help\n",
                server.toString().c_str());

    const int64_t shotAt = int64_t(shotAfterSeconds) * 1000;
    bool shotTaken = false;
    // Состояние обхода: номер точки и фаза щелчка внутри неё.
    // Ожидание нового отряда после выделения: в какой системе ждём
    // и кто там был до этого.
    uint32_t awaitingSplit = kNoSystem;
    std::vector<uint32_t> knownFleets;

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
    // Правый щелчок по звезде — второй способ отдать приказ, и у него
    // своя ветка кода. Считаем его отдельно: иначе обход отчитается
    // «приказы отданы», проверив только один из двух путей.
    int sweepRightOrders = 0;
    bool sweepFailed = false;

    // АНИМАЦИЯ ЖИВЁТ ТОЛЬКО В ОКНЕ.
    //
    // Безоконный режим снимает КАДР, а кадр обязан быть устоявшимся:
    // снимок, поймавший середину перехода, показывает наполовину
    // выехавшую панель и наполовину досчитанное число, и отличить это
    // от дефекта нельзя. Обход щелчками — та же история: он ищет падения,
    // и лишний источник различий между прогонами ему только мешает.
    //
    // Флагом --motion можно включить движение и без окна: иначе снять
    // саму анимацию было бы нечем.
    const bool motion = motionFlag >= 0 ? motionFlag != 0 : !headless;
    ui.setMotion(motion);
    camera.smooth = motion;

    // Эффекты мира: кольца и вспышки там, где что-то случилось.
    // Живут только при включённом движении — неподвижный кадр показал бы
    // случайную фазу кольца, и снимок перестал бы быть повторяемым.
    render::Effects effects;

    // ДОСТРОЙКА ЗДАНИЯ — СОБЫТИЕ БЕЗ УВЕДОМЛЕНИЯ.
    //
    // Сервер о ней не сообщает, и правильно делает: игрок сам заказал
    // стройку и сам знает срок, а полтора часа спустя новость «шахта
    // готова» была бы шумом. Но на карте это по-прежнему изменение мира,
    // и заметить его нечем — здание просто оказывается построенным.
    //
    // Клиент замечает достройку сам, сравнивая число зданий со своим
    // прошлым срезом. Раз в секунду, а не каждый кадр: обход всех своих
    // планет шестьдесят раз в секунду ради события, случающегося раз
    // в час, — плохая сделка, а секунда опоздания незаметна.
    std::unordered_map<uint32_t, uint32_t> planetSystem;   // планета -> система
    // Маска занятых слотов, а не их число: по числу нельзя сказать, КАКОЕ
    // здание появилось, а «шахта построена» и «построено что-то» — разные
    // сообщения. Двенадцать слотов укладываются в шестнадцать бит.
    std::unordered_map<uint32_t, uint16_t> builtBefore;
    int64_t nextBuildScan = 0;

    int64_t previousFrame = nowMilliseconds();

    while (window.pumpEvents(input)) {
        const int64_t now = nowMilliseconds();
        // Шаг кадра в секундах. Берётся из ТЕХ ЖЕ часов, что и сеть:
        // два независимых источника времени в одном цикле однажды
        // разъезжаются, и анимация начинает жить своей жизнью.
        //
        // Ограничен сверху ЗДЕСЬ, один раз и для всех, кто им пользуется.
        // Когда окно свернули на минуту, между кадрами проходит минута,
        // и без ограничения все переходы доигрываются за один кадр,
        // а все эффекты разом умирают — то есть игрок, вернувшись,
        // не видит ничего именно в тот момент, когда больше всего хочет
        // понять, что изменилось. Восьмая доля секунды — примерно два
        // пропущенных кадра: столько догонять честно.
        const float frameDelta =
            std::min(0.125f, float(now - previousFrame) / 1000.0f);
        previousFrame = now;
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

            // ЭФФЕКТ НА КАРТЕ ТАМ, ГДЕ ЭТО СЛУЧИЛОСЬ.
            //
            // Карточка в углу говорит словами, но слова читают, а вспышку
            // видят. Кольцо на карте отвечает на вопрос «где» раньше, чем
            // игрок дочитает «что», — и это единственный способ заметить
            // событие, глядя не туда.
            render::EffectKind kind = render::EffectKind::Count;
            bool ownColor = false;
            if (motion && event.system < client.galaxy().systemCount() &&
                effectForNotice(event.kind, kind, ownColor)) {
                effects.spawn(kind,
                              float(client.galaxy().positionX(event.system).toDouble()),
                              float(client.galaxy().positionY(event.system).toDouble()),
                              camera.worldHeight * 0.012f,
                              ownColor ? render::empireColor(client.empire())
                                       : render::EmpireColor{0.95f, 0.42f, 0.36f},
                              event.system);
            }
        }
        messages.update(now);

        if (motion && client.ready() && now >= nextBuildScan) {
            nextBuildScan = now + 1000;
            for (const auto& [id, planet] : client.view().planets) {
                if (planet.owner != uint8_t(client.empire())) continue;
                uint16_t mask = 0;
                for (int slot = 0; slot < sim::kMaxSlots; ++slot) {
                    if (planet.buildings[slot] != 0) mask |= uint16_t(1u << slot);
                }
                const auto seen = builtBefore.find(id);
                const uint16_t before = seen != builtBefore.end() ? seen->second : mask;
                const uint16_t appeared = uint16_t(mask & ~before);
                builtBefore[id] = mask;
                if (appeared == 0) continue;

                const auto where = planetSystem.find(id);
                if (where == planetSystem.end()) continue;
                effects.spawn(render::EffectKind::Built,
                              float(client.galaxy().positionX(where->second).toDouble()),
                              float(client.galaxy().positionY(where->second).toDouble()),
                              camera.worldHeight * 0.012f,
                              render::empireColor(client.empire()), id);
                for (int slot = 0; slot < sim::kMaxSlots; ++slot) {
                    if ((appeared & (1u << slot)) == 0) continue;
                    messages.add(std::string(render::buildingName(planet.buildings[slot])) +
                                     " построена",
                                 kGood, now, where->second, "icon_planet");
                }
            }
        }

        if (motion) effects.update(frameDelta);
        camera.follow(frameDelta);

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
            camera.place(float(client.galaxy().positionX(client.capital()).toDouble()),
                         float(client.galaxy().positionY(client.capital()).toDouble()),
                         std::clamp(extent * 0.9f, camera.minHeight, camera.maxHeight));
            state.system = client.capital();
            const auto own = client.fleetsAt(client.capital());
            if (!own.empty()) state.fleet = own.front();
            // Окно отряда открывается щелчком, и снять его иначе нечем:
            // снимок делается без единого нажатия. Заодно набирается
            // половина: снимок пустого набора не показывает главного —
            // как выглядят полосы разделения, когда их потянули.
            state.squadOpen = shotSquad && !own.empty();
            if (state.squadOpen) {
                const auto found = client.view().fleets.find(state.fleet);
                if (found != client.view().fleets.end()) {
                    state.splitTake = sim::fleetHalf(found->second.composition);
                }
            }

            // МАРШРУТ НА СНИМКЕ. Ломаная маршрута, кружки точек, кольцо
            // приписки и метка стойки рисуются только у отряда, которому
            // отдали приказ, — а снимок делается без единого нажатия.
            // Без этого флага весь новый слой карты нельзя УВИДЕТЬ,
            // а увиденное на снимке — единственный способ поймать
            // дефект отрисовки.
            if (shotRoute && state.fleet != kNoFleet) {
                // Маршрут набирается ХОДЬБОЙ ПО ГРАФУ, а не тремя соседями
                // дома: у столицы их может быть один — так и вышло на seed 7,
                // и снимок показывал одно звено вместо трёх, хотя рисовалось
                // всё правильно.
                std::vector<uint32_t> path;
                uint32_t at = client.capital();
                uint32_t came = kNoSystem;
                for (int step = 0; step < 3; ++step) {
                    const uint32_t count = client.galaxy().neighborCount(at);
                    uint32_t next = kNoSystem;
                    for (uint32_t k = 0; k < count; ++k) {
                        const uint32_t candidate = client.galaxy().neighbors(at)[k];
                        if (candidate == came) continue;
                        if (std::find(path.begin(), path.end(), candidate) != path.end()) {
                            continue;
                        }
                        next = candidate;
                        break;
                    }
                    if (next == kNoSystem) break;
                    path.push_back(next);
                    came = at;
                    at = next;
                }
                for (size_t k = 0; k < path.size(); ++k) {
                    if (k == 0) {
                        client.orderRoute(state.fleet, path[k]);
                    } else {
                        client.orderRouteAppend(state.fleet, path[k]);
                    }
                }
                const uint32_t home = client.capital();
                client.orderStance(state.fleet, sim::Stance::Patrol);
                client.orderEvade(state.fleet, true);
                client.orderAnchorSystem(state.fleet, home);
                // Вся галактика в кадре: маршрут из трёх точек уходит
                // за край экрана при обычном приближении, и снимок
                // показывал бы одно звено из трёх.
                camera.place(0.0f, 0.0f,
                             float(client.galaxy().extent().toDouble()) * 2.0f);
            }

            // Таблица «планета -> система» строится ОДИН РАЗ: планеты
            // не переезжают, а перебирать всю галактику каждый кадр
            // ради неподвижных данных незачем.
            for (uint32_t index = 0; index < client.galaxy().systemCount(); ++index) {
                for (const auto& planet : client.planetsAt(index)) {
                    planetSystem[planet.id] = index;
                }
            }

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
                camera.place(float(client.galaxy().positionX(client.capital()).toDouble()),
                             float(client.galaxy().positionY(client.capital()).toDouble()),
                             camera.worldHeight / shotZoom);
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
            // Поэтому после двух сеточных проходов идут ЗОНДЫ:
            // вертикальные линии с мелким шагом по столбцам панелей.
            // Панели выстроены столбцами, все их кнопки лежат на этих
            // линиях, и попасть по ним — уже не вопрос удачи.
            // ШАГ ЗОНДА — ДВЕНАДЦАТЬ ПИКСЕЛЕЙ ПРИ ШАГЕ СЕТКИ ТРИДЦАТЬ ВОСЕМЬ.
            //
            // Не меньше: самый мелкий нажимаемый элемент на экране —
            // галочка выделения в строке отряда, около шестнадцати
            // пикселей в обе стороны, а в любой отрезок длиной
            // шестнадцать решётка с шагом двенадцать попадает всегда.
            // И не больше: шаг восемнадцать её уже пропускает.
            //
            // Не меньше ещё и потому, что зонд стоит времени. Шаг шесть
            // давал вчетверо больше точек, и обход переставал укладываться
            // в четверть часа — то есть переставал запускаться.
            const int probeStep = std::max(4, sweepStep / 3);
            const int probeRows = std::max(1, height / probeStep);
            // ЧАСТАЯ СЕТКА ПО ПАНЕЛЯМ, А НЕ ТРИ ЛИНИИ.
            //
            // Трёх линий хватало, пока в полосе приказов стояла ОДНА
            // кнопка во всю ширину панели: линия по левому столбцу
            // попадала в неё, куда бы её ни поставили. Кнопок стало
            // четыре, левая линия легла ровно на «Стоп» — недоступный,
            // пока у отряда нет маршрута, — и обход перестал доходить
            // до приказов ВООБЩЕ, отчитываясь при этом «клиент жив».
            //
            // Молчаливо неполный обход хуже отсутствующего, и лечится
            // он не подгонкой одной координаты, а решёткой по всей ширине
            // панелей — левым пяти восьмым экрана, где стоят и столбец
            // выбранного, и окно отряда.
            const int probeLaneStep = probeStep;
            const int probeLanes = std::max(1, int(float(width) * 0.58f) / probeLaneStep);
            // Плюс две линии: по КАРТЕ и по списку справа. Линия по карте
            // нужна не для симметрии — взведённый приказ ждёт щелчка
            // по системе, и без неё путь «нажал приказ → указал цель»
            // обрывается на середине, ровно там, где игра однажды падала.
            const int probeLaneCount = probeLanes + 2;
            const int probeTotal = probeRows * probeLaneCount;
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
                std::printf("\n  приказов флоту отдано: %d маршрутом, %d правой\n",
                            sweepOrders, sweepRightOrders);
                // ОБХОД ОБЯЗАН ДОЙТИ ДО ВСЕГО СЛОВАРЯ ПРИКАЗОВ.
                //
                // Раньше здесь стояла одна проверка — «нажали ли
                // отправить флот», — и она молчала о том, что весь
                // остальной словарь обход не трогает. Управление флотом
                // это два десятка кнопок: маршрут, стойки, уклонение,
                // приписка, разделение, выделение группой. Перечислять
                // их поимённо стоит трёх строк и ловит ровно тот случай,
                // ради которого обход и заведён: кнопка есть на экране,
                // но до неё нельзя дотянуться.
                const render::ActionKind required[] = {
                    render::ActionKind::BeginMove,   render::ActionKind::ToggleSquad,
                    render::ActionKind::SetStance,   render::ActionKind::SetEvade,
                    render::ActionKind::SplitAdjust, render::ActionKind::SplitPreset,
                    render::ActionKind::SplitFleet,  render::ActionKind::AnchorSystem,
                    render::ActionKind::AnchorPlanet, render::ActionKind::ToggleSelect,
                };
                for (render::ActionKind kind : required) {
                    if (sweepActions[int(kind)] > 0) continue;
                    std::fprintf(stderr,
                                 "обход не засчитан: до намерения «%s» "
                                 "так и не дошли\n",
                                 render::actionName(kind));
                    sweepFailed = true;
                }
                if (sweepRightOrders == 0) {
                    std::fprintf(stderr,
                                 "обход не засчитан: правый щелчок по звезде "
                                 "ни разу не отдал приказ\n");
                    sweepFailed = true;
                }
                if (sweepOrders == 0) {
                    // Намерения мало: «Приказ» только ВЗВОДИТ режим, а падала
                    // игра на следующем шаге — когда взведённый приказ ловил
                    // щелчок по карте и слал его серверу. Этот шаг обязан быть
                    // пройден целиком, а не наполовину.
                    std::fprintf(stderr,
                                 "обход не засчитан: взведённый приказ ни разу "
                                 "не дошёл до щелчка по карте\n");
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
            bool rightProbe = false;
            if (probing) {
                // Столица и флот в ней закреплены на каждом кадре: панель
                // флота обязана быть на месте, иначе кнопки, которую мы
                // ищем, на экране просто нет.
                const uint32_t capital = client.capital();
                state.system = capital;
                const auto own = client.fleetsAt(state.system);
                if (!own.empty()) state.fleet = own.front();
                // И окно отряда — тоже. Кнопка «Отряд…» его ПЕРЕКЛЮЧАЕТ,
                // а зонд проходит по ней несколько раз: окно оставалось
                // то открытым, то закрытым в зависимости от чётности,
                // и половина словаря приказов зондом не проверялась —
                // молча, потому что закрытого окна на экране просто нет.
                state.squadOpen = !own.empty();

                const int at = sweepPoint - gridTotal * 2;
                const int lane = std::min(at / probeRows, probeLaneCount - 1);
                const int step = at % probeRows;
                pointX = lane < probeLanes
                             ? float(lane * probeLaneStep + probeLaneStep / 2)
                         : lane == probeLanes ? float(width) * 0.68f
                                              : float(width) * 0.88f;
                pointY = float(step * probeStep + probeStep / 2);

                // ЛИНИЯ ПО КАРТЕ ЦЕЛИТСЯ В НАСТОЯЩУЮ ЗВЕЗДУ.
                //
                // Доля ширины экрана попадала в звезду только пока камера
                // стояла там, где её оставил предыдущий проход, — а её
                // двигают и «навести», и «обзор». В итоге оба пути приказа
                // не проверялись НИ РАЗУ, и обход об этом молчал.
                //
                // Поэтому здесь камера ставится над домом, а щелчок идёт
                // ровно в соседнюю систему: она заведомо на экране, и оба
                // пути приказа — взведённый режим и правая кнопка —
                // проверяются на каждом прогоне, а не при удачном
                // расположении камеры.
                if (lane == probeLanes && client.galaxy().neighborCount(capital) > 0) {
                    // Точка выбрана В ПРОСВЕТЕ между окном отряда слева
                    // и списком справа: щелчок по звезде, накрытой панелью,
                    // до карты не доходит вовсе.
                    pointX = float(width) * 0.68f;
                    pointY = float(height) * 0.60f;

                    // Камера ставится так, чтобы соседняя система оказалась
                    // ровно под этой точкой. Считается через toWorld:
                    // при камере в цели точка даёт мир W, значит нужная
                    // камера — это цель, отражённая относительно W.
                    const uint32_t target = client.galaxy().neighbors(capital)[0];
                    const float tx = float(client.galaxy().positionX(target).toDouble());
                    const float ty = float(client.galaxy().positionY(target).toDouble());
                    const float span = float(client.galaxy().extent().toDouble()) * 0.9f;
                    camera.place(tx, ty, span);
                    float wx = 0.0f, wy = 0.0f;
                    camera.toWorld(pointX, pointY, width, height, wx, wy);
                    camera.place(tx * 2.0f - wx, ty * 2.0f - wy, span);

                    // Половина строк идёт левой кнопкой по взведённому
                    // приказу, половина — правой без него: у приказа два
                    // пути, и оба обязаны быть пройдены.
                    state.awaitingMoveTarget = (step % 2) == 0;
                    state.routePoints = 0;
                }

                // НА ЛИНИИ ПО КАРТЕ ЖМЁМ ЧЕРЕЗ СТРОКУ ПРАВУЮ КНОПКУ.
                //
                // У приказа два пути: взведённый режим плюс левый щелчок
                // и правый щелчок по звезде. Ветки разные, и обход, знающий
                // только левую кнопку, проверял бы половину — молча, как
                // и полагается пропущенной проверке.
                //
                // Мышь между фазами не двигается, поэтому правое нажатие
                // здесь читается именно как приказ, а не как перетаскивание
                // карты, — что заодно проверяет и само это различение.
                rightProbe = lane == probeLanes && (step % 2) == 1;
            } else {
                const int at = sweepPoint % gridTotal;
                pointX = float((at % columns) * sweepStep + sweepStep / 2);
                pointY = float((at / columns) * sweepStep + sweepStep / 2);
            }

            input.setMouse(pointX, pointY);
            input.setButton(rightProbe ? MouseButton::Right : MouseButton::Left,
                            sweepPhase <= 1);
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

        ui.begin(uiInput, width, height, frameDelta);
        const render::ScreenAction action = screen.build(ui, client, state, now);
        ui.end();
        if (sweepStep > 0 && action.kind < render::ActionKind::Count) {
            ++sweepActions[int(action.kind)];
        }

        // ПРИКАЗ УХОДИТ ВСЕМ ВЫДЕЛЕННЫМ. Выделение из одного отряда —
        // обычный случай, и отдельной ветки под него нет: пустой список
        // означает «только выбранный», и дальше всё одинаково.
        //
        // Разделение и слияние сюда НЕ входят: «взять по три корвета
        // из каждого из пяти отрядов» — это не приказ, а таблица, и она
        // делается в окне отряда по одному.
        auto forSelected = [&](uint32_t primary, auto&& send) -> uint32_t {
            uint32_t sent = 0;
            if (state.selection.empty()) {
                if (primary != kNoFleet && send(primary)) ++sent;
                return sent;
            }
            for (uint32_t id : state.selection) {
                if (send(id)) ++sent;
            }
            return sent;
        };
        // «Маршрут снят» против «маршрут снят · отрядов 4»: приказ,
        // отданный группе, обязан отчитаться числом. Иначе игрок не знает,
        // дошёл ли он до всех, — а именно это его и волнует.
        auto squadNote = [](const std::string& text, uint32_t count) {
            return count > 1 ? text + " · отрядов " + std::to_string(count) : text;
        };

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
            case render::ActionKind::Colonize:
                if (client.orderColonize(action.value, action.planet)) {
                    messages.add("высаживаем колонию", kInfo, now, state.system,
                                 "hull_colonizer");
                    if (motion && state.system < client.galaxy().systemCount()) {
                        effects.spawn(
                            render::EffectKind::Order,
                            float(client.galaxy().positionX(state.system).toDouble()),
                            float(client.galaxy().positionY(state.system).toDouble()),
                            camera.worldHeight * 0.012f,
                            render::empireColor(client.empire()), state.system);
                    }
                }
                break;
            case render::ActionKind::SplitFleet:
                if (client.orderSplitFleet(action.value, sim::Hull(action.slot), 1)) {
                    messages.add(std::string("выделен ") +
                                     render::hullName(uint8_t(action.slot)),
                                 kInfo, now, state.system,
                                 render::hullIcon(uint8_t(action.slot)));
                    // Запоминаем, какие отряды тут были ДО выделения.
                    //
                    // Новый флот создаёт сервер, и его номер приходит
                    // снапшотом через несколько кадров. Игрок к этому
                    // моменту уже смотрит на список и не знает, какой
                    // из двух отрядов новый: они отличаются только числом
                    // кораблей. Поэтому клиент запоминает старый состав
                    // списка и выбирает того, кого в нём не было.
                    //
                    // Выделяют почти всегда затем, чтобы сразу отправить, —
                    // и лишний щелчок «а теперь найди новый отряд» съел бы
                    // ровно ту лёгкость, ради которой выделение и сделано.
                    knownFleets.clear();
                    for (uint32_t id : client.fleetsAt(state.system)) {
                        knownFleets.push_back(id);
                    }
                    awaitingSplit = state.system;
                }
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
                // Щелчок по строке значит «работаю с этим одним»: группа
                // сбрасывается. Добавляют к группе отдельной кнопкой.
                state.selection.clear();
                state.awaitingMoveTarget = false;
                break;
            case render::ActionKind::ToggleSelect: {
                auto& group = state.selection;
                // Первое добавление подхватывает уже выбранный отряд:
                // «выбран первый, добавляю второй» обязано дать двух,
                // а не одного второго.
                if (group.empty() && state.fleet != kNoFleet &&
                    state.fleet != action.value) {
                    group.push_back(state.fleet);
                }
                const auto at = std::find(group.begin(), group.end(), action.value);
                if (at != group.end()) {
                    group.erase(at);
                } else {
                    group.push_back(action.value);
                }
                if (group.size() == 1) {
                    state.fleet = group.front();
                    group.clear();
                } else if (!group.empty() &&
                           std::find(group.begin(), group.end(), state.fleet) ==
                               group.end()) {
                    state.fleet = group.front();
                }
                break;
            }
            case render::ActionKind::BeginMove:
                state.awaitingMoveTarget = state.fleet != kNoFleet;
                // Счётчик точек обнуляется вместе со взводом: первый
                // щелчок нового захода ЗАМЕНЯЕТ маршрут, а не дописывает
                // его к прошлому. Иначе игрок, взводивший приказ дважды,
                // получал бы маршрут из старых точек с новыми на хвосте.
                state.routePoints = 0;
                if (state.awaitingMoveTarget) state.inSystem = false;
                break;
            case render::ActionKind::CancelMove:
                state.awaitingMoveTarget = false;
                state.routePoints = 0;
                break;

            // --- управление отрядом ---
            case render::ActionKind::ClearRoute: {
                const uint32_t sent = forSelected(
                    action.value, [&](uint32_t id) { return client.orderRouteClear(id); });
                if (sent > 0) {
                    messages.add(squadNote("маршрут снят", sent), kInfo, now, state.system,
                                 "icon_fleet");
                }
                break;
            }
            case render::ActionKind::GoHome: {
                // У каждого отряда СВОЯ приписка, поэтому «домой» группой —
                // это не одна цель на всех, а по цели на отряд.
                uint32_t where = kNoSystem;
                const uint32_t sent = forSelected(action.value, [&](uint32_t id) {
                    const auto found = client.view().fleets.find(id);
                    if (found == client.view().fleets.end()) return false;
                    const uint32_t home = found->second.anchor;
                    if (home >= client.galaxy().systemCount()) return false;
                    if (!client.orderRoute(id, home)) return false;
                    where = home;
                    return true;
                });
                if (sent > 0) {
                    messages.add(squadNote("отряд идёт домой", sent), kInfo, now, where,
                                 "icon_fleet");
                }
                break;
            }
            case render::ActionKind::SetStance: {
                if (action.slot >= uint8_t(sim::Stance::Count)) break;
                const sim::Stance want = sim::Stance(action.slot);
                const uint32_t sent = forSelected(
                    action.value, [&](uint32_t id) { return client.orderStance(id, want); });
                if (sent > 0) {
                    messages.add(squadNote(std::string("стойка: ") + sim::stanceName(want),
                                           sent),
                                 kInfo, now, state.system, "icon_fleet");
                }
                break;
            }
            case render::ActionKind::SetEvade: {
                const bool on = action.slot != 0;
                const uint32_t sent = forSelected(
                    action.value, [&](uint32_t id) { return client.orderEvade(id, on); });
                if (sent > 0) {
                    messages.add(squadNote(on ? "отряд будет уклоняться"
                                              : "отряд примет любой бой",
                                           sent),
                                 kInfo, now, state.system, "icon_defense");
                }
                break;
            }
            case render::ActionKind::AnchorSystem: {
                const uint32_t home = state.system;
                const uint32_t sent = forSelected(action.value, [&](uint32_t id) {
                    return client.orderAnchorSystem(id, home);
                });
                if (sent > 0) {
                    messages.add(squadNote("приписан к системе", sent), kInfo, now, home,
                                 "icon_star");
                }
                break;
            }
            case render::ActionKind::AnchorPlanet: {
                const uint32_t planet = action.planet;
                const uint32_t sent = forSelected(action.value, [&](uint32_t id) {
                    return client.orderAnchorPlanet(id, planet);
                });
                if (sent > 0) {
                    messages.add(squadNote("приписан к планете", sent), kInfo, now,
                                 state.system, "icon_planet");
                }
                break;
            }
            case render::ActionKind::MergeFleet:
                if (client.orderMergeFleet(state.fleet, action.value)) {
                    messages.add("отряды слиты", kInfo, now, state.system, "icon_fleet");
                }
                break;
            case render::ActionKind::ToggleSquad:
                state.squadOpen = !state.squadOpen;
                // Набор разделения сбрасывается при закрытии: вернувшись
                // к отряду завтра, игрок не должен обнаружить в окне
                // позавчерашнее намерение, о котором давно забыл.
                if (!state.squadOpen) state.splitTake = sim::Fleet{};
                break;
            case render::ActionKind::SplitAdjust:
                if (action.slot >= 1 && action.slot < uint8_t(sim::Hull::Count)) {
                    state.splitTake[sim::Hull(action.slot)] = action.value;
                }
                break;
            case render::ActionKind::SplitPreset: {
                const auto found = client.view().fleets.find(state.fleet);
                const sim::Fleet whole =
                    found != client.view().fleets.end() ? found->second.composition
                                                        : sim::Fleet{};
                if (action.value == 1) {
                    state.splitTake = sim::fleetHalf(whole);
                } else if (action.value == 2) {
                    state.splitTake = sim::fleetOnly(whole, sim::Hull::Colonizer);
                } else {
                    state.splitTake = sim::Fleet{};
                }
                break;
            }
            case render::ActionKind::SplitConfirm:
                if (client.orderSplitFleet(action.value, state.splitTake)) {
                    messages.add("отряд выделен", kInfo, now, state.system, "icon_fleet");
                    // Тот же приём, что и у быстрого выделения: запоминаем
                    // состав списка ДО приказа, чтобы выбрать новый отряд,
                    // когда он приедет снапшотом.
                    awaitingSplit = true;
                    knownFleets = client.fleetsAt(state.system);
                    state.splitTake = sim::Fleet{};
                }
                break;
            case render::ActionKind::FocusSystem:
                if (action.value < client.galaxy().systemCount()) {
                    state.system = action.value;
                    state.planetIndex = 0;
                    state.slot = kNoSlot;
                    state.inSystem = false;
                    camera.flyTo(float(client.galaxy().positionX(action.value).toDouble()),
                                 float(client.galaxy().positionY(action.value).toDouble()),
                                 std::max(camera.minHeight, camera.worldHeight * 0.5f));
                }
                break;
            case render::ActionKind::ResetView:
                if (state.inSystem) {
                    systemCamera.focusOrbit = 0xFFFFFFFFu;
                    systemCamera.distance =
                        render::fitDistance(client.galaxy().planetCount(state.system));
                } else if (client.ready()) {
                    camera.flyTo(0.0f, 0.0f,
                                 float(client.galaxy().extent().toDouble()) * 2.0f);
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
            // Escape снимает по ОДНОМУ слою за нажатие, от самого
            // временного к самому постоянному: набор маршрута, окно
            // отряда, выделение группой, вид системы — и только потом
            // выход. Иначе одно нажатие сносило бы сразу всё, включая
            // группу, которую собирали дольше всего.
            if (state.awaitingMoveTarget) {
                state.awaitingMoveTarget = false;
                state.routePoints = 0;
            } else if (state.squadOpen) {
                state.squadOpen = false;
                state.splitTake = sim::Fleet{};
            } else if (!state.selection.empty()) {
                state.selection.clear();
            } else if (state.inSystem) {
                state.inSystem = false;
            } else {
                break;
            }
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
        // ДВИЖЕНИЕ СЧИТАЕМ ТОЛЬКО ПОСЛЕ НАЖАТИЯ, и порядок этих двух
        // проверок именно поэтому такой. Если сначала взводить `panning`,
        // а потом мерить сдвиг, то кадр нажатия принесёт сдвиг, которым
        // рука доводила курсор ДО звезды, — и правый щелчок никогда
        // не отличался бы от перетаскивания. То есть приказ не работал бы
        // вовсе, а выглядело бы это как «иногда не срабатывает».
        if (panning && (std::fabs(input.mouseDeltaX()) > 1.0f ||
                        std::fabs(input.mouseDeltaY()) > 1.0f)) {
            panned = true;
        }
        if (input.wasPressed(MouseButton::Right)) {
            panning = true;
            panned = false;
        }

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
                // Рука игрока отменяет начатый перелёт: догонять цель,
                // от которой человек только что уехал, — это борьба
                // с игроком за управление камерой.
                camera.targetX = camera.centerX;
                camera.targetY = camera.centerY;
            }
        }

        // ВЫДЕЛЕНИЕ ЧИСТИТСЯ ОТ МЁРТВЫХ. Отряд гибнет в бою и сливается
        // с соседом, и его номер остаётся в списке навсегда: панель пишет
        // «выбрано 5», когда живых трое, а приказ молча уходит в никуда.
        if (client.ready() && !state.selection.empty()) {
            const auto& alive = client.view().fleets;
            state.selection.erase(
                std::remove_if(state.selection.begin(), state.selection.end(),
                               [&](uint32_t id) { return alive.count(id) == 0; }),
                state.selection.end());
            // Один уцелевший — это уже не группа, а обычный выбор.
            if (state.selection.size() == 1) {
                state.fleet = state.selection.front();
                state.selection.clear();
            }
        }

        // Новый отряд появился — выбираем его.
        if (awaitingSplit != kNoSystem && client.ready()) {
            for (uint32_t id : client.fleetsAt(awaitingSplit)) {
                if (std::find(knownFleets.begin(), knownFleets.end(), id) !=
                    knownFleets.end()) {
                    continue;
                }
                state.fleet = id;
                awaitingSplit = kNoSystem;
                knownFleets.clear();
                break;
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

        // --- РАМКА ВЫДЕЛЕНИЯ ---
        //
        // Левая кнопка по пустому месту карты не делала ничего, и рамка
        // встаёт ровно в эту дыру: правая занята под перетаскивание карты,
        // а клавиш в этой игре нет вовсе. Начинается по нажатию мимо
        // звёзд, живёт, пока кнопку держат, и решает на отпускании.
        if (client.ready() && worldInput && !state.inSystem &&
            !state.awaitingMoveTarget && under == kNoSystem &&
            input.wasPressed(MouseButton::Left)) {
            banding = true;
            bandStartX = input.mouseX();
            bandStartY = input.mouseY();
        }
        if (banding) {
            state.bandX0 = bandStartX;
            state.bandY0 = bandStartY;
            state.bandX1 = input.mouseX();
            state.bandY1 = input.mouseY();
            const bool dragged =
                std::fabs(state.bandX1 - bandStartX) > kBandThreshold ||
                std::fabs(state.bandY1 - bandStartY) > kBandThreshold;
            state.bandActive = dragged;

            if (!input.isDown(MouseButton::Left)) {
                if (dragged) {
                    // Углы рамки переводим в мир и проверяем там: флот
                    // в пути стоит МЕЖДУ узлами, и брать его по системе
                    // отправления значило бы выделять то, чего в рамке нет.
                    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
                    camera.toWorld(state.bandX0, state.bandY0, width, height, ax, ay);
                    camera.toWorld(state.bandX1, state.bandY1, width, height, bx, by);
                    const float minX = std::min(ax, bx), maxX = std::max(ax, bx);
                    const float minY = std::min(ay, by), maxY = std::max(ay, by);

                    state.selection.clear();
                    const uint32_t systems = client.galaxy().systemCount();
                    for (const auto& [id, fleet] : client.view().fleets) {
                        if (fleet.empire != uint8_t(client.empire())) continue;
                        if (fleet.system >= systems) continue;
                        float fx = float(client.galaxy().positionX(fleet.system).toDouble());
                        float fy = float(client.galaxy().positionY(fleet.system).toDouble());
                        if (fleet.nextSystem != fleet.system && fleet.nextSystem < systems) {
                            const float t =
                                std::clamp(float(fleet.progress.toDouble()), 0.0f, 1.0f);
                            fx += (float(client.galaxy().positionX(fleet.nextSystem)
                                             .toDouble()) - fx) * t;
                            fy += (float(client.galaxy().positionY(fleet.nextSystem)
                                             .toDouble()) - fy) * t;
                        }
                        if (fx < minX || fx > maxX || fy < minY || fy > maxY) continue;
                        state.selection.push_back(id);
                    }
                    // Порядок обхода словаря не определён, а номера отрядов
                    // игрок читает по возрастанию: без сортировки «первый
                    // выделенный» менялся бы от кадра к кадру.
                    std::sort(state.selection.begin(), state.selection.end());

                    if (state.selection.empty()) {
                        messages.add("в рамке нет ваших отрядов", kInfo, now, kNoSystem,
                                     "icon_alert");
                    } else {
                        state.fleet = state.selection.front();
                        const auto& picked = client.view().fleets.at(state.fleet);
                        state.system = picked.system;
                        // Выделение из одного отряда — это обычный выбор,
                        // и группы тут нет: список из одного номера только
                        // мешал бы, показывая «выбрано 1».
                        if (state.selection.size() == 1) {
                            state.selection.clear();
                        } else {
                            messages.add("выделено отрядов: " +
                                             std::to_string(state.selection.size()),
                                         kInfo, now, picked.system, "icon_fleet");
                        }
                    }
                } else {
                    // Щелчок без протаскивания снимает группу: в RTS щелчок
                    // по пустому месту всегда значит «больше ничего не
                    // выделено», и ломать это ожидание незачем.
                    state.selection.clear();
                }
                banding = false;
                state.bandActive = false;
            }
        }

        // --- ПРАВЫЙ ЩЕЛЧОК ПО ЗВЕЗДЕ: «ИДИ ТУДА» ---
        //
        // Приказ одним движением, как в любой RTS: выделил рамкой —
        // ткнул правой в систему. Полоса кнопок никуда не делась,
        // но набирать через неё маршрут из одной точки — это три
        // действия там, где хватает одного.
        //
        // Маршрут ЗАМЕНЯЕТСЯ целиком: правый щелчок это «отставить
        // всё и идти сюда». Добавление точек осталось за взведённым
        // «Приказом», где игрок явно сказал, что собирает план.
        if (input.wasReleased(MouseButton::Right)) {
            const bool order = client.ready() && worldInput && !state.inSystem &&
                               !panned && under != kNoSystem;
            if (order) {
                const uint32_t sent = forSelected(state.fleet, [&](uint32_t id) {
                    return client.orderRoute(id, under);
                });
                if (sent > 0) {
                    if (sweepStep > 0) ++sweepRightOrders;
                    state.awaitingMoveTarget = false;
                    state.routePoints = 0;
                    messages.add(squadNote("флот идёт в систему " +
                                               std::to_string(under),
                                           sent),
                                 kInfo, now, under, "icon_fleet");
                    if (motion) {
                        effects.spawn(render::EffectKind::Order,
                                      float(client.galaxy().positionX(under).toDouble()),
                                      float(client.galaxy().positionY(under).toDouble()),
                                      camera.worldHeight * 0.012f,
                                      render::empireColor(client.empire()), under);
                    }
                }
            }
            panning = false;
            panned = false;
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
                    // ПЕРВЫЙ ЩЕЛЧОК ЗАМЕНЯЕТ МАРШРУТ, КАЖДЫЙ СЛЕДУЮЩИЙ
                    // ДОБАВЛЯЕТ ТОЧКУ. Режим остаётся взведённым, пока
                    // игрок не скажет «Готово», не нажмёт Escape или
                    // не щёлкнет мимо звёзд. Это и есть shift-клик из RTS,
                    // сделанный без единой клавиши.
                    const bool firstPoint = state.routePoints == 0;
                    const uint32_t reached = forSelected(state.fleet, [&](uint32_t id) {
                        return firstPoint ? client.orderRoute(id, under)
                                          : client.orderRouteAppend(id, under);
                    });
                    const bool sent = reached > 0;
                    if (sent) {
                        ++state.routePoints;
                        messages.add(
                            squadNote(firstPoint ? "флот идёт в систему " +
                                                       std::to_string(under)
                                                 : "точка " +
                                                       std::to_string(state.routePoints) +
                                                       ": система " +
                                                       std::to_string(under),
                                      reached),
                            kInfo, now, under, "icon_fleet");
                        // ОТКЛИК НА ПРИКАЗ, а не новость о мире. Флот
                        // тронется через секунду, а до тех пор картинка
                        // не меняется ничем — и щелчок выглядит потерянным.
                        // Кольцо, сжимающееся к цели, отвечает сразу:
                        // приказ принят, идём сюда.
                        if (motion) {
                            effects.spawn(render::EffectKind::Order,
                                          float(client.galaxy().positionX(under).toDouble()),
                                          float(client.galaxy().positionY(under).toDouble()),
                                          camera.worldHeight * 0.012f,
                                          render::empireColor(client.empire()), under);
                        }
                    }
                    // Маршрут кончился местом: дальше добавлять некуда,
                    // и режим снимается сам, чтобы следующий щелчок
                    // не выглядел проигнорированным.
                    if (state.routePoints >= sim::FleetOrders::kMaxRoute) {
                        state.awaitingMoveTarget = false;
                        state.routePoints = 0;
                        messages.add("маршрут заполнен: восемь точек", kInfo, now,
                                     under, "icon_alert");
                    }
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
                    // Выбрали другую систему — группа больше не про неё.
                    state.selection.clear();

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
            } else if (state.awaitingMoveTarget) {
                // Щелчок МИМО ЗВЁЗД заканчивает набор маршрута. Третий
                // способ сказать «готово» после кнопки и Escape, и самый
                // естественный: рука уже на карте, и тянуться к панели
                // ради одного нажатия незачем.
                state.awaitingMoveTarget = false;
                state.routePoints = 0;
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
            // Настройки перелёта ставятся ЗДЕСЬ, а не при создании камеры:
            // камеру сбрасывают при каждом входе в систему, и настройка,
            // положенная один раз, пережила бы ровно один вход.
            systemCamera.followSeconds = motion ? 0.5f : 0.0f;
            systemCamera.deltaSeconds = frameDelta;
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
            selection.group = state.selection.empty() ? nullptr : state.selection.data();
            selection.groupCount = uint32_t(state.selection.size());
            // Линия к цели тянется только когда приказ взведён: постоянная
            // линия за курсором — это шум, который игрок перестаёт видеть.
            selection.hoverSystem = state.awaitingMoveTarget ? under : kNoSystem;

            device.setCamera(camera.toRhi());
            // Часы карты — те же, что у интерфейса: два независимых
            // счётчика времени в одном кадре однажды разъезжаются,
            // и пульс кольца перестаёт совпадать с пульсом панели.
            mapView.setClock(motion ? ui.clock() : 0.0f);
            mapView.build(client.galaxy(), client.view(), client.empire(), selection,
                          camera.toRhi(), mapFrame);
            // Эффекты ПОСЛЕ карты и в тот же кадр: у них та же камера
            // и та же пачка отрезков, а значит нет ни второго
            // преобразования, ни второго способа ошибиться.
            effects.build(mapFrame);
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
