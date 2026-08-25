// pw_rhi — абстракция графического API.
//
// Один бэкенд на все платформы: Vulkan. На маках и айфонах он идёт поверх
// Metal через MoltenVK — см. ADR-013. Второй, нативный бэкенд на Metal нам
// сейчас не нужен: 2.5D-карта рисуется единицами вызовов отрисовки, и
// накладные расходы трансляции там неразличимы, а стоимость второй
// реализации — месяцы.
//
// ДВА РЕЖИМА ЦЕЛИ, и второй не менее важен первого:
//   оконный    — поверхность из окна, изображение показывается на экране;
//   безголовый — рисуем в память, без окна и без видеокарты.
//
// Безголовый режим существует не для галочки. Он позволяет проверять рендер
// автоматически: отрисовать кадр в CI, считать пиксели и сравнить с эталоном.
// Иначе графика проверяется только глазами и только тогда, когда кто-то
// удосужился посмотреть.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pw/core/png.h"

namespace pw {
class Window;
}

namespace pw::rhi {

// Тип пикселя и запись PNG живут в ядре: они не зависят от графического
// API и нужны в том числе симуляции — посмотреть на процедурную галактику.
using pw::Rgba8;
using pw::writePng;

struct DeviceDesc {
    /// Слои проверки Vulkan. Ловят неверное использование API там, где оно
    /// иначе проявилось бы порчей памяти или чёрным экраном.
    bool validation = false;

    /// nullptr — безголовый режим.
    const Window* window = nullptr;

    int width = 1280;   // размер цели в безголовом режиме
    int height = 720;
};

struct ClearColor {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

/// Один спрайт. Всё, что отличает его от соседа, — здесь.
///
/// Раскладка совпадает с поэкземплярными атрибутами sprite.vert байт в байт:
/// массив этих структур уходит в видеопамять как есть, без перекладки.
struct SpriteInstance {
    float x = 0.0f, y = 0.0f;                   // центр в мировых координатах
    float halfWidth = 1.0f, halfHeight = 1.0f;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;   // прямоугольник в атласе
    /// Тон УМНОЖАЕТСЯ на текстуру: спрайты испечены серыми, цвет империи
    /// берётся отсюда. Один атлас на всех игроков вместо копии под каждый цвет.
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    /// Поворот в ОБОРОТАХ, как и везде в проекте, а не в радианах.
    float rotationTurns = 0.0f;
};

struct LineVertex {
    float x = 0.0f, y = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

/// Куда смотрит камера и с каким увеличением.
struct Camera {
    float centerX = 0.0f, centerY = 0.0f;
    /// Сколько мировых единиц влезает по высоте экрана. Задаём высоту,
    /// а не ширину: иначе на широком мониторе игрок видел бы больше карты,
    /// чем на обычном, — то есть преимущество за форму монитора.
    float worldHeight = 1000.0f;
};

/// Дескриптор текстуры. Непрозрачный: тип Vulkan наружу не выпускается.
using TextureHandle = uint32_t;
inline constexpr TextureHandle kInvalidTexture = 0;

/// Устройство, цель отрисовки и конвейеры.
///
/// Конвейеров два: спрайты и линии. Разделение не косметическое —
/// линия связывает две произвольные точки, и растягивать под неё
/// повёрнутый квадрат значило бы считать угол и длину на процессоре
/// для каждого из тысяч рёбер графа.
class Device {
public:
    Device();
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool init(const DeviceDesc& desc);
    void shutdown();

    bool valid() const;
    const std::string& adapterName() const;
    bool headless() const;

    int targetWidth() const;
    int targetHeight() const;

    /// Собрать графический конвейер из скомпилированного SPIR-V.
    /// Шейдеры компилируются на сборке и коммитятся — как и таблицы
    /// тригонометрии, по той же причине: результат обязан быть одинаковым
    /// у всех, а не зависеть от версии компилятора шейдеров на машине.
    bool createPipeline(const std::vector<uint8_t>& vertexSpirv,
                        const std::vector<uint8_t>& fragmentSpirv);

    bool beginFrame(const ClearColor& clear);
    /// Отрисовать вершины текущим конвейером. Вершины задаёт сам шейдер —
    /// буфера вершин на этом этапе ещё нет.
    void draw(uint32_t vertexCount);
    bool endFrame();

    /// Собрать конвейер спрайтов: инстансированный квадрат с атласом.
    bool createSpritePipeline(const std::vector<uint8_t>& vertexSpirv,
                              const std::vector<uint8_t>& fragmentSpirv);
    /// Собрать конвейер линий.
    bool createLinePipeline(const std::vector<uint8_t>& vertexSpirv,
                            const std::vector<uint8_t>& fragmentSpirv);

    /// Загрузить текстуру RGBA8. Данные копируются, вызывающий их не держит.
    TextureHandle createTexture(int width, int height, const Rgba8* pixels);

    /// Задать камеру на кадр. Соотношение сторон берётся из цели.
    void setCamera(const Camera& camera);

    /// Отрисовать спрайты одним вызовом.
    ///
    /// Данные копируются в кадровый буфер: он переиспользуется, поэтому
    /// вызывающий может собирать спрайты в обычный вектор и не думать
    /// о времени жизни.
    void drawSprites(const SpriteInstance* instances, size_t count, TextureHandle atlas);

    /// Отрисовать отрезки. Вершины идут парами.
    void drawLines(const LineVertex* vertices, size_t count);

    /// Считать цель в память. Только безголовый режим.
    /// Ради этого метода безголовый путь и написан: он превращает проверку
    /// картинки из ручного разглядывания в обычный автотест.
    bool readback(std::vector<Rgba8>& out) const;

    const std::string& lastError() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pw::rhi
