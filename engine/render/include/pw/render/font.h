#pragma once

// Текст на экране.
//
// Игрок обязан видеть свои ресурсы, состав флота и что происходит
// в системе — на экране, а не в терминале. Значит нужен текст, а текст
// на GPU — это атлас глифов и по спрайту на символ.
//
// Шрифт моноширинный намеренно. Сетка глифов становится точной, координаты
// считаются без ошибок округления, а счётчики ресурсов не дёргаются при
// каждом изменении цифры — в пропорциональном шрифте они дёргались бы
// постоянно, потому что цифры разной ширины.
//
// Надписи рисуются ТЕМ ЖЕ конвейером спрайтов, что и карта. Отдельного
// конвейера для текста нет и не нужно: буква — это тот же квадрат
// с куском текстуры.

#include <cstdint>
#include <string>
#include <vector>

#include "pw/core/png.h"
#include "pw/rhi/rhi.h"

namespace pw::render {

/// Цвет надписи. Тон умножается на белый глиф, поэтому один атлас
/// работает на все цвета интерфейса.
struct TextColor {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

class Font {
public:
    /// Прочитать описание и текстуру. `jsonPath` — assets/build/font.json.
    bool load(const std::string& jsonPath);

    bool valid() const { return !glyphs_.empty() && !pixels_.empty(); }
    const std::string& error() const { return error_; }

    const std::vector<Rgba8>& pixels() const { return pixels_; }
    int textureWidth() const { return width_; }
    int textureHeight() const { return height_; }

    /// Ширина одного символа при заданной высоте строки.
    ///
    /// Шрифт моноширинный, поэтому ширина строки — это просто число
    /// символов, умноженное на это.
    ///
    /// Число не взято на глаз: у DejaVu Sans Mono шаг равен 0,602 кегля,
    /// а глиф печётся размером 0,78 клетки. Клетка рисуется высотой
    /// lineHeight, значит кегль на экране равен 0,78·lineHeight, и шаг —
    /// 0,602·0,78. Первая версия считала шаг от целой клетки, и буквы
    /// стояли с заметными провалами между ними.
    float advance(float lineHeight) const { return lineHeight * 0.47f; }

    /// Ширина строки в пикселях.
    float width(const std::string& utf8, float lineHeight) const;

    /// Разложить строку в спрайты.
    ///
    /// Координаты экранные: начало в левом верхнем углу, ось Y вниз.
    /// `x`, `y` — левый верхний угол первой буквы.
    void layout(const std::string& utf8, float x, float y, float lineHeight,
                const TextColor& color, std::vector<rhi::SpriteInstance>& out) const;

private:
    struct Glyph {
        float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    };

    /// Кодовая точка -> глиф. Набор небольшой и фиксированный, поэтому
    /// плоский список с поиском по коду быстрее и проще словаря.
    std::vector<uint32_t> codes_;
    std::vector<Glyph> glyphs_;

    std::vector<Rgba8> pixels_;
    int width_ = 0, height_ = 0;
    int cell_ = 0;
    std::string error_;

    const Glyph* find(uint32_t code) const;
};

/// Разобрать UTF-8 в кодовые точки.
///
/// Свой разбор, потому что строки в интерфейсе русские, а полагаться на
/// локаль нельзя: она разная на разных машинах, и надписи расползались бы
/// у части игроков.
std::vector<uint32_t> decodeUtf8(const std::string& text);

}  // namespace pw::render
