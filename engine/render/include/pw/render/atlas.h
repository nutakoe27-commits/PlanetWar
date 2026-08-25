#pragma once

// Спрайтовый атлас: описание кадров плюс сама текстура.
//
// Кадры печёт Blender (docs/07-ASSET-PIPELINE.md): каждый корпус снимается
// с восьми направлений в ортографической проекции, кадры укладываются
// в один атлас, рядом кладётся json с координатами. Здесь это читается
// и превращается в готовые к отрисовке прямоугольники.
//
// SVG в игре нет и не будет: всё, что видит игрок, — испечённые кадры
// настоящих трёхмерных моделей.

#include <cstdint>
#include <string>
#include <vector>

#include "pw/core/png.h"

namespace pw::render {

/// Один кадр в атласе: где он лежит и какому корпусу с каким поворотом
/// соответствует.
struct AtlasFrame {
    std::string hull;
    uint32_t rotation = 0;
    /// Координаты в атласе, доли от единицы — сразу в том виде, в каком
    /// их ждёт шейдер.
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    /// Положение и размер в ПИКСЕЛЯХ.
    ///
    /// Отдельно от долей намеренно: шейдеру нужны доли, а инструментам
    /// и проверкам — точные пиксели. Обратный пересчёт из долей даёт
    /// ошибку округления и заставляет тест заглядывать в соседний кадр.
    int x = 0, y = 0;
    int width = 0, height = 0;
};

class Atlas {
public:
    /// Прочитать описание и обе текстуры.
    ///
    /// `jsonPath` — файл вида assets/build/ships.json; текстуры берутся
    /// из него же, по именам в поле textures.
    bool load(const std::string& jsonPath);

    bool valid() const { return !frames_.empty() && !albedo_.empty(); }
    const std::string& error() const { return error_; }

    /// Кадр по корпусу и повороту. Поворот задаётся в оборотах и
    /// приводится к ближайшему испечённому направлению.
    const AtlasFrame* frame(const std::string& hull, float rotationTurns) const;
    /// Любой кадр этого корпуса. Нужен, когда поворот не важен.
    const AtlasFrame* frame(const std::string& hull) const;

    const std::vector<Rgba8>& albedo() const { return albedo_; }
    const std::vector<Rgba8>& accentMask() const { return accentMask_; }
    int textureWidth() const { return width_; }
    int textureHeight() const { return height_; }
    uint32_t rotationSteps() const { return rotationSteps_; }
    const std::vector<AtlasFrame>& frames() const { return frames_; }

    /// Собрать текстуру для отрисовки: альбедо, где маска акцента задаёт
    /// альфу цвета империи.
    ///
    /// Цвет империи не пишется в текстуру: он умножается в шейдере, и
    /// один атлас работает на всех игроков. Иначе на каждый цвет
    /// потребовалась бы своя копия — а цветов столько же, сколько империй.
    std::vector<Rgba8> composite() const;

private:
    std::vector<AtlasFrame> frames_;
    std::vector<Rgba8> albedo_;
    std::vector<Rgba8> accentMask_;
    int width_ = 0, height_ = 0;
    uint32_t rotationSteps_ = 1;
    std::string error_;
};

}  // namespace pw::render
