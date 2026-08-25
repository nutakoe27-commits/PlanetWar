// pw_triangle — контрольная точка Фазы 0.
//
// Два режима, и второй важнее первого:
//
//   окно      — обычный запуск, треугольник виден на экране;
//   безголовый — кадр рисуется в память и сохраняется в PNG.
//
// Безголовый режим делает графику проверяемой автоматически. Без него рендер
// проверяется только глазами и только тогда, когда кто-то удосужился
// посмотреть. С ним CI на каждом коммите рисует кадр и сверяет его с
// эталоном — на машине, где нет ни дисплея, ни видеокарты.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pw/core/log.h"
#include "pw/platform/platform.h"
#include "pw/platform/window.h"
#include "pw/rhi/rhi.h"

namespace {
#include "pw/rhi/shaders.inc"

std::vector<uint8_t> asBytes(const unsigned char* data, size_t size) {
    return std::vector<uint8_t>(data, data + size);
}

/// Цвет фона — тот же тёмно-синий, что у карты галактики в макетах.
constexpr pw::rhi::ClearColor kSpace{0.039f, 0.055f, 0.098f, 1.0f};

void usage() {
    std::printf(
        "pw_triangle — проверка платформенного слоя и Vulkan\n"
        "\n"
        "  --headless          рисовать в память, без окна\n"
        "  --out <файл.png>    куда сохранить кадр (подразумевает --headless)\n"
        "  --size <ш> <в>      размер цели, по умолчанию 1280x720\n"
        "  --frames <n>        сколько кадров отрисовать в оконном режиме\n"
        "  --validation        включить слои проверки Vulkan\n");
}
}  // namespace

int main(int argc, char** argv) {
    pw::setLogLevel(pw::LogLevel::Info);

    bool headless = false;
    bool validation = false;
    std::string outPath;
    int width = 1280, height = 720;
    int maxFrames = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--validation") {
            validation = true;
        } else if (arg == "--out" && i + 1 < argc) {
            outPath = argv[++i];
            headless = true;
        } else if (arg == "--size" && i + 2 < argc) {
            width = std::atoi(argv[++i]);
            height = std::atoi(argv[++i]);
        } else if (arg == "--frames" && i + 1 < argc) {
            maxFrames = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::printf("неизвестный аргумент: %s\n\n", arg.c_str());
            usage();
            return 2;
        }
    }

    if (!pw::initPlatform(headless)) {
        std::printf("не удалось поднять платформу\n");
        return 1;
    }

    pw::WindowDesc windowDesc;
    windowDesc.title = "PlanetWar — Фаза 0";
    windowDesc.width = width;
    windowDesc.height = height;
    windowDesc.headless = headless;

    pw::Window window(windowDesc);
    if (!window.valid()) {
        std::printf("не удалось создать окно\n");
        pw::shutdownPlatform();
        return 1;
    }

    pw::rhi::DeviceDesc deviceDesc;
    deviceDesc.window = &window;
    deviceDesc.validation = validation;
    deviceDesc.width = width;
    deviceDesc.height = height;

    pw::rhi::Device device;
    if (!device.init(deviceDesc)) {
        std::printf("Vulkan: %s\n", device.lastError().c_str());
        pw::shutdownPlatform();
        return 1;
    }

    if (!device.createPipeline(asBytes(kTriangleVert, sizeof(kTriangleVert)),
                               asBytes(kTriangleFrag, sizeof(kTriangleFrag)))) {
        std::printf("конвейер: %s\n", device.lastError().c_str());
        pw::shutdownPlatform();
        return 1;
    }

    std::printf("устройство: %s\n", device.adapterName().c_str());
    std::printf("цель:       %dx%d, %s\n", device.targetWidth(), device.targetHeight(),
                device.headless() ? "в память" : "на экран");

    pw::Input input;
    int frames = 0;
    bool running = true;

    while (running) {
        running = window.pumpEvents(input);
        if (input.wasPressed(pw::Key::Escape)) running = false;

        if (!device.beginFrame(kSpace)) {
            std::printf("кадр: %s\n", device.lastError().c_str());
            break;
        }
        device.draw(3);
        if (!device.endFrame()) {
            std::printf("кадр: %s\n", device.lastError().c_str());
            break;
        }

        ++frames;
        if (headless) break;
        if (maxFrames > 0 && frames >= maxFrames) running = false;
    }

    int exitCode = 0;
    if (headless && !outPath.empty()) {
        std::vector<pw::rhi::Rgba8> pixels;
        if (!device.readback(pixels)) {
            std::printf("считывание: %s\n", device.lastError().c_str());
            exitCode = 1;
        } else if (!pw::rhi::writePng(outPath, pixels, device.targetWidth(),
                                      device.targetHeight())) {
            std::printf("не удалось записать %s\n", outPath.c_str());
            exitCode = 1;
        } else {
            std::printf("кадр сохранён: %s\n", outPath.c_str());
        }
    }

    std::printf("отрисовано кадров: %d\n", frames);

    device.shutdown();
    pw::shutdownPlatform();
    return exitCode;
}
