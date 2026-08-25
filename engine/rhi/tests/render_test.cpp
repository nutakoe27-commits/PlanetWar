// Автопроверка рендера.
//
// Смысл теста: сделать графику проверяемой машиной. Обычно рендер смотрят
// глазами и только тогда, когда кто-то удосужился запустить — а регрессия
// в шейдере или в порядке слоёв всплывает через недели. Здесь кадр рисуется
// в память на программном растеризаторе (видеокарта не нужна вовсе) и
// проверяется по опорным точкам.
//
// Проверяются СВОЙСТВА кадра, а не побайтовое совпадение с эталоном.
// Побитовое сравнение картинок между драйверами — заведомо ломкий тест:
// правила растеризации стандартизованы, а точность интерполяции нет.
// Свойства же обязаны выполняться на любом устройстве.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cmath>

#include "pw/core/log.h"
#include "pw/platform/platform.h"
#include "pw/platform/window.h"
#include "pw/rhi/rhi.h"

namespace {
#include "pw/rhi/shaders.inc"

constexpr int kWidth = 640;
constexpr int kHeight = 360;

// Тот же фон, что в образце: тёмно-синий цвет карты галактики.
constexpr pw::rhi::ClearColor kClear{0.039f, 0.055f, 0.098f, 1.0f};

std::vector<uint8_t> bytes(const unsigned char* data, size_t size) {
    return std::vector<uint8_t>(data, data + size);
}

/// Точка внутри треугольника: вершина, подтянутая к центру, чтобы не
/// попасть на кромку, где значение зависит от правил растеризации.
struct Point {
    int x, y;
};

Point inward(float ndcX, float ndcY) {
    constexpr float kCentroidX = 0.0f;
    constexpr float kCentroidY = (-0.62f + 0.55f + 0.55f) / 3.0f;
    constexpr float kPull = 0.25f;
    const float x = ndcX + kPull * (kCentroidX - ndcX);
    const float y = ndcY + kPull * (kCentroidY - ndcY);
    return Point{int((x * 0.5f + 0.5f) * kWidth), int((y * 0.5f + 0.5f) * kHeight)};
}

struct Frame {
    std::vector<pw::rhi::Rgba8> pixels;
    bool ok = false;

    const pw::rhi::Rgba8& at(Point p) const {
        return pixels[size_t(p.y) * size_t(kWidth) + size_t(p.x)];
    }
};

Frame renderOnce() {
    Frame frame;
    pw::setLogLevel(pw::LogLevel::Warn);

    if (!pw::initPlatform(/*headless=*/true)) return frame;

    pw::WindowDesc windowDesc;
    windowDesc.headless = true;
    windowDesc.width = kWidth;
    windowDesc.height = kHeight;
    pw::Window window(windowDesc);

    pw::rhi::DeviceDesc deviceDesc;
    deviceDesc.window = &window;
    deviceDesc.width = kWidth;
    deviceDesc.height = kHeight;
    deviceDesc.validation = true;  // слои проверки ловят неверное использование API

    pw::rhi::Device device;
    if (!device.init(deviceDesc)) {
        pw::shutdownPlatform();
        return frame;
    }
    if (!device.createPipeline(bytes(kTriangleVert, sizeof(kTriangleVert)),
                               bytes(kTriangleFrag, sizeof(kTriangleFrag)))) {
        pw::shutdownPlatform();
        return frame;
    }

    if (device.beginFrame(kClear)) {
        device.draw(3);
        device.endFrame();
    }
    frame.ok = device.readback(frame.pixels);

    device.shutdown();
    pw::shutdownPlatform();
    return frame;
}

const Frame& frame() {
    static Frame cached = renderOnce();
    return cached;
}

}  // namespace

TEST_CASE("рендер: устройство поднимается и кадр считывается") {
    REQUIRE(frame().ok);
    CHECK(frame().pixels.size() == size_t(kWidth) * size_t(kHeight));
}

TEST_CASE("рендер: фон закрашен цветом очистки") {
    REQUIRE(frame().ok);
    // Углы гарантированно вне треугольника.
    for (Point corner : {Point{4, 4}, Point{kWidth - 5, 4}, Point{4, kHeight - 5}}) {
        const auto& px = frame().at(corner);
        CHECK(std::abs(int(px.r) - 10) <= 6);
        CHECK(std::abs(int(px.g) - 14) <= 6);
        CHECK(std::abs(int(px.b) - 25) <= 6);
        CHECK(int(px.a) == 255);
    }
}

TEST_CASE("рендер: вершина треугольника янтарная") {
    REQUIRE(frame().ok);
    // Верхняя вершина в NDC — (0, -0.62): в Vulkan ось Y направлена вниз.
    const auto& px = frame().at(inward(0.0f, -0.62f));
    CHECK(int(px.r) > 170);
    CHECK(int(px.r) > int(px.g));
    CHECK(int(px.g) > int(px.b));
}

TEST_CASE("рендер: правое основание уходит в синий") {
    REQUIRE(frame().ok);
    const auto& px = frame().at(inward(0.66f, 0.55f));
    CHECK(int(px.b) > int(px.r));
    CHECK(int(px.b) > 100);
}

TEST_CASE("рендер: левое основание темнее правого") {
    REQUIRE(frame().ok);
    const auto& left = frame().at(inward(-0.66f, 0.55f));
    const auto& right = frame().at(inward(0.66f, 0.55f));
    // Левая вершина серо-стальная, правая — насыщенно-синяя.
    CHECK(int(left.b) < int(right.b));
}

TEST_CASE("рендер: интерполяция цвета работает") {
    REQUIRE(frame().ok);
    // Если бы интерполяция сломалась, треугольник вышел бы одноцветным.
    // Считаем различные цвета в кадре по огрублённой сетке.
    std::vector<bool> seen(32 * 32 * 32, false);
    int distinct = 0;
    for (const auto& px : frame().pixels) {
        const int key = (px.r >> 3) * 1024 + (px.g >> 3) * 32 + (px.b >> 3);
        if (!seen[size_t(key)]) {
            seen[size_t(key)] = true;
            ++distinct;
        }
    }
    CHECK(distinct > 40);
}

TEST_CASE("рендер: треугольник занимает разумную долю кадра") {
    REQUIRE(frame().ok);
    // Площадь треугольника в NDC: 0.5 * 1.32 * 1.17 = 0.772 при полной
    // площади 4.0, то есть около 19% кадра. Сильное отклонение означает
    // сбитую область вывода или ножницы отсечения.
    int inside = 0;
    for (const auto& px : frame().pixels) {
        if (int(px.r) + int(px.g) + int(px.b) > 60) ++inside;
    }
    const double share = double(inside) / double(kWidth * kHeight);
    CHECK(share > 0.14);
    CHECK(share < 0.26);
}
