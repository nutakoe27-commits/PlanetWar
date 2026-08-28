#pragma once

// Отрисовка карты галактики.
//
// Превращает состояние мира (WorldView от сервера плюс своя галактика)
// в спрайты и отрезки. Ничего не знает ни про Vulkan, ни про окно: отдаёт
// массивы, а кто и как их рисует — дело вызывающего.
//
// Из этого следует главное: карту можно собрать и проверить БЕЗ ВИДЕОКАРТЫ.
// Тест кладёт мир, просит собрать кадр и считает спрайты — столько ли
// звёзд, тем ли цветом, там ли флоты. Иначе рендер проверялся бы только
// глазами и только тогда, когда кто-то удосужился посмотреть.

#include <cstdint>
#include <vector>

#include "pw/game/snapshot.h"
#include "pw/render/atlas.h"
#include "pw/rhi/rhi.h"
#include "pw/sim/galaxy.h"

namespace pw::render {

/// Цвет империи. Восемь различимых цветов на восемь мест в сезоне.
///
/// Подобраны так, чтобы различаться и при дальтонизме: не «красный против
/// зелёного», а разные светлота и насыщенность. Владение системой читается
/// с карты мгновенно, и путать его цену нельзя.
struct EmpireColor {
    float r, g, b;
};

const EmpireColor& empireColor(uint32_t empire);
/// Цвет ничьей системы.
const EmpireColor& neutralColor();

/// Что игрок выделил на карте.
struct Selection {
    /// kNoSystem, если ничего не выбрано.
    uint32_t system = 0xFFFFFFFFu;
    /// Выбранный флот; 0xFFFFFFFF — нет.
    uint32_t fleet = 0xFFFFFFFFu;
    /// Куда игрок целится приказом. Рисуется линией от выделенного.
    uint32_t hoverSystem = 0xFFFFFFFFu;

    /// Какая планета выбранной системы сейчас в работе.
    ///
    /// Номер по порядку в системе, а не номер сущности: игрок думает
    /// «вторая планета отсюда», а не «планета 507».
    uint32_t planetIndex = 0;
};

/// Собранный кадр карты: всё, что нужно отдать в отрисовку.
struct MapFrame {
    std::vector<rhi::LineVertex> lines;
    std::vector<rhi::SpriteInstance> sprites;

    void clear() {
        lines.clear();
        sprites.clear();
    }
};

class MapView {
public:
    /// Атлас нужен, чтобы знать координаты кадров кораблей.
    void setAtlas(const Atlas* atlas) { atlas_ = atlas; }

    /// Часы анимации: сколько секунд идёт клиент. Ноль означает
    /// «движения нет» и даёт УСТОЯВШУЮСЯ картинку.
    ///
    /// Через сеттер, а не параметром build: часы — свойство клиента,
    /// а не описание того, что рисовать. Тест и снимок оставляют ноль
    /// и получают кадр без единой пульсирующей детали — иначе проверка
    /// ловила бы случайную фазу дыхания кольца.
    void setClock(float seconds) { clock_ = seconds; }
    float clock() const { return clock_; }

    /// Собрать кадр.
    ///
    /// `empire` — чья это картинка: своё выделяется ярче, чужое приглушается.
    void build(const sim::Galaxy& galaxy, const game::WorldView& world, uint32_t empire,
               const Selection& selection, const rhi::Camera& camera, MapFrame& out) const;

    /// Найти систему под точкой мира. 0xFFFFFFFF — мимо.
    ///
    /// Радиус попадания растёт с отдалением камеры: на общем плане звезда
    /// занимает несколько пикселей, и требовать попасть в неё точно
    /// значило бы сделать карту неуправляемой.
    static uint32_t pick(const sim::Galaxy& galaxy, float worldX, float worldY,
                         float worldHeight);

private:
    const Atlas* atlas_ = nullptr;
    float clock_ = 0.0f;
};

}  // namespace pw::render
