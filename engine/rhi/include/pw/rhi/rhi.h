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

namespace pw {
class Window;
}

namespace pw::rhi {

struct Rgba8 {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

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

/// Устройство, цель отрисовки и конвейер. Для первого треугольника этого
/// достаточно; разделять их по классам будем, когда появится второй конвейер.
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

    /// Считать цель в память. Только безголовый режим.
    /// Ради этого метода безголовый путь и написан: он превращает проверку
    /// картинки из ручного разглядывания в обычный автотест.
    bool readback(std::vector<Rgba8>& out) const;

    const std::string& lastError() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

/// Сохранить кадр в PNG. Без сторонних библиотек: пишем несжатый поток
/// в формате deflate «store», это полтора десятка строк и ноль зависимостей.
bool writePng(const std::string& path, const std::vector<Rgba8>& pixels,
              int width, int height);

}  // namespace pw::rhi
