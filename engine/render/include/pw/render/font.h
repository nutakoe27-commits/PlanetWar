#pragma once

// Текст на экране.
//
// Игрок обязан видеть свои ресурсы, состав флота и что происходит
// в системе — на экране, а не в терминале. Значит нужен текст, а текст
// на GPU — это атлас глифов и по спрайту на символ.
//
// Шрифт ПРОПОРЦИОНАЛЬНЫЙ, но цифры моноширинные. Это не половинчатость,
// а то же правило, что в наборе таблиц.
//
// Пока шрифт был моноширинным целиком, у «ш» и у «i» была одна ширина,
// и половина строки уходила в пустоту: подсказка на две строки читалась
// как телеграмма. Метрики каждой буквы меряются по САМОМУ атласу при
// выпечке — между шрифтом и пикселями стоит растеризатор Blender,
// и верить надо тому, что получилось, а не тому, что было задумано.
//
// Цифры при этом остаются одной ширины: счётчик ресурсов меняется
// постоянно, и столбик чисел, где «1» уже «8», рябит при каждом
// изменении значения.
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

    /// Средний шаг символа при заданной высоте строки.
    ///
    /// Нужен там, где строки ещё нет: оценка ширины поля, запас под
    /// счётчик. Настоящая ширина строки считается по буквам — width().
    float advance(float lineHeight) const { return lineHeight * averageAdvance_; }

    /// Шаг конкретного символа. Для неизвестных — средний.
    float advanceOf(uint32_t code, float lineHeight) const;

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
        /// Где в клетке лежит краска и на сколько двигать курсор —
        /// долями клетки. Померено по атласу при выпечке.
        float left = 0.0f, right = 1.0f, advance = 0.47f;
    };

    /// Кодовая точка -> глиф. Набор небольшой и фиксированный, поэтому
    /// плоский список с поиском по коду быстрее и проще словаря.
    std::vector<uint32_t> codes_;
    std::vector<Glyph> glyphs_;

    std::vector<Rgba8> pixels_;
    int width_ = 0, height_ = 0;
    int cell_ = 0;
    float averageAdvance_ = 0.47f;
    std::string error_;

    /// Прочитать метрики из манифеста. Их отсутствие — не ошибка:
    /// старый атлас без метрик работает как моноширинный.
    void readMetrics(const std::string& json);

    const Glyph* find(uint32_t code) const;
};

/// Разобрать UTF-8 в кодовые точки.
///
/// Свой разбор, потому что строки в интерфейсе русские, а полагаться на
/// локаль нельзя: она разная на разных машинах, и надписи расползались бы
/// у части игроков.
std::vector<uint32_t> decodeUtf8(const std::string& text);

}  // namespace pw::render
