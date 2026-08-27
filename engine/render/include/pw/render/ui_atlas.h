#pragma once

// Атлас интерфейса: рамки, кнопки, значки.
//
// Отдельно от спрайтов кораблей намеренно. У атласа интерфейса своя
// раскладка (спрайты именованные, а не «корпус плюс поворот») и своё
// свойство, которого нет у остального: ПОЛЕ РАСТЯЖКИ. Панель произвольного
// размера рисуется девятью кусками — четыре угла не тянутся, четыре края
// тянутся вдоль одной оси, середина заполняет остаток. Без этого рамка
// с фаской растягивалась бы целиком и превращалась в размытое пятно.
//
// Всё испечено в Blender: у панели настоящая фаска и настоящий свет,
// у значка здания — та же модель, что стоит на планете. Вектора в графике
// игры нет, интерфейс не исключение.

#include <cstdint>
#include <string>
#include <vector>

#include "pw/core/png.h"

namespace pw::render {

/// Один именованный спрайт атласа.
struct UiSprite {
    std::string name;
    /// Координаты в атласе, доли от единицы — сразу в том виде, в каком
    /// их ждёт шейдер.
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    /// Положение и размер в ПИКСЕЛЯХ. Нужны и растяжке, и проверкам:
    /// обратный пересчёт из долей даёт ошибку округления и заставляет
    /// заглядывать в соседний спрайт.
    int x = 0, y = 0, width = 0, height = 0;
    /// Сколько пикселей у края НЕ растягивается. Ноль — обычный спрайт.
    int border = 0;

    bool stretchable() const { return border > 0; }
};

class UiAtlas {
public:
    /// Прочитать описание и текстуру. `jsonPath` — assets/build/ui.json.
    bool load(const std::string& jsonPath);

    bool valid() const { return !sprites_.empty() && !pixels_.empty(); }
    const std::string& error() const { return error_; }

    /// Спрайт по имени. nullptr — такого в атласе нет.
    ///
    /// Именно nullptr, а не «какой-нибудь»: молча подставленный чужой
    /// спрайт превращает опечатку в имени в загадочную картинку, которую
    /// потом ищут глазами полдня.
    const UiSprite* find(const std::string& name) const;

    const std::vector<Rgba8>& pixels() const { return pixels_; }
    int textureWidth() const { return width_; }
    int textureHeight() const { return height_; }
    const std::vector<UiSprite>& sprites() const { return sprites_; }

private:
    std::vector<UiSprite> sprites_;
    std::vector<Rgba8> pixels_;
    int width_ = 0, height_ = 0;
    std::string error_;
};

}  // namespace pw::render
