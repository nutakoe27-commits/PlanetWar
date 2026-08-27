#include "pw/render/ui.h"

#include <algorithm>
#include <cmath>

namespace pw::render {

namespace {

/// Спрайт-заглушка на случай отсутствующего атласа.
///
/// Интерфейс обязан рисоваться даже без ассетов: белые прямоугольники —
/// это некрасиво, но это работающая игра, а падение или пустой экран —
/// нет. Заодно в тестах не нужен атлас, чтобы проверить раскладку.
const UiSprite& fallbackSprite() {
    static const UiSprite sprite{"", 0.0f, 0.0f, 1.0f, 1.0f, 0, 0, 1, 1, 0};
    return sprite;
}

}  // namespace

// ---------------------------------------------------------------------------
// Кадр
// ---------------------------------------------------------------------------

void Ui::begin(const UiInput& input, int screenWidth, int screenHeight) {
    frame_.clear();
    input_ = input;
    width_ = screenWidth;
    height_ = screenHeight;
    wantsMouse_ = false;
    hot_ = 0;
    tooltipText_.clear();
    missing_.clear();

    // Доля высоты экрана, а не фиксированный размер: на 4K надпись
    // в 14 пикселей нечитаема, а на ноутбуке в 28 занимает пол-экрана.
    lineHeight_ = std::clamp(float(screenHeight) * 0.0195f, 13.0f, 30.0f);
    // Шаг сетки привязан к строке: интерфейс, у которого отступы не связаны
    // с кеглем, на другом разрешении рассыпается — либо всё слипается,
    // либо всё разъезжается.
    theme_.unit = std::round(lineHeight_ * 0.5f);

}

void Ui::end() {
    // Освобождение зажатого виджета — в КОНЦЕ кадра, а не в начале.
    // В начале кадра отпускания ещё не обработаны: сбрось активный виджет
    // тут — и кадр, в котором кнопку отпустили, не найдёт себя зажатой,
    // а щелчок не случится никогда. В конце же кадра виджет либо уже
    // отработал отпускание сам, либо исчез с экрана вместе с панелью —
    // и тогда его всё равно надо отпустить, иначе он навсегда перехватит
    // следующее нажатие.
    if (!input_.down) active_ = 0;

    if (tooltipText_.empty()) return;

    // Подсказка рисуется последней, поверх всего: объявленная в момент
    // вызова, она оказывалась бы под панелями, которые пришли позже.
    const float pad = theme_.unit;
    const float width = textWidth(tooltipText_) + pad * 2.0f;
    const float height = lineHeight_ + pad * 1.5f;

    // Уводим от края экрана: подсказка, наполовину вылезшая за монитор, —
    // это подсказка, которую не прочитать.
    float x = tooltipX_ + theme_.unit * 1.5f;
    float y = tooltipY_ + theme_.unit * 1.5f;
    if (x + width > float(width_) - pad) x = float(width_) - pad - width;
    if (y + height > float(height_) - pad) y = tooltipY_ - height - theme_.unit;
    x = std::max(pad, x);
    y = std::max(pad, y);

    const Rect box{x, y, width, height};
    panel(box, "panel_light");
    text(x + pad, y + pad * 0.75f, tooltipText_, theme_.text);
}

const UiSprite* Ui::lookup(const char* name) {
    if (atlas_ == nullptr || name == nullptr) return nullptr;
    const UiSprite* sprite = atlas_->find(name);
    if (sprite == nullptr) {
        // По одному разу за кадр: иначе спрайт, которого нет у кнопки
        // в списке из тридцати строк, даст тридцать одинаковых записей.
        if (std::find(missing_.begin(), missing_.end(), name) == missing_.end()) {
            missing_.emplace_back(name);
        }
    }
    return sprite;
}

UiBatch& Ui::batchFor(UiTexture texture) {
    if (!frame_.batches.empty() && frame_.batches.back().texture == texture) {
        return frame_.batches.back();
    }
    frame_.batches.push_back(UiBatch{texture, {}});
    return frame_.batches.back();
}

void Ui::quad(const Rect& r, const UiSprite& sprite, const TextColor& tint) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;

    rhi::SpriteInstance instance;
    instance.x = r.x + r.w * 0.5f;
    instance.y = r.y + r.h * 0.5f;
    instance.halfWidth = r.w * 0.5f;
    instance.halfHeight = r.h * 0.5f;
    instance.u0 = sprite.u0;
    instance.v0 = sprite.v0;
    instance.u1 = sprite.u1;
    instance.v1 = sprite.v1;
    instance.r = tint.r;
    instance.g = tint.g;
    instance.b = tint.b;
    instance.a = tint.a;
    batchFor(UiTexture::Atlas).sprites.push_back(instance);
}

// ---------------------------------------------------------------------------
// Рисование
// ---------------------------------------------------------------------------

void Ui::panel(const Rect& r, const char* spriteName, float alpha) {
    const UiSprite* sprite = lookup(spriteName);
    if (sprite == nullptr) sprite = &fallbackSprite();

    TextColor tint{1.0f, 1.0f, 1.0f, alpha};

    if (!sprite->stretchable() || r.w < float(sprite->border) * 2.0f ||
        r.h < float(sprite->border) * 2.0f) {
        // Мельче двух полей растягивать нечего: девять кусков схлопнулись
        // бы в кашу. Рисуем целиком — на таком размере разницы не видно.
        quad(r, *sprite, tint);
        return;
    }

    // Девять кусков. Углы не тянутся, края тянутся вдоль одной оси,
    // середина заполняет остаток. Именно поэтому у панели любого размера
    // одинаково чёткая фаска.
    const float border = float(sprite->border);
    const float du = (sprite->u1 - sprite->u0) * border / float(sprite->width);
    const float dv = (sprite->v1 - sprite->v0) * border / float(sprite->height);

    const float xs[4] = {r.x, r.x + border, r.right() - border, r.right()};
    const float ys[4] = {r.y, r.y + border, r.bottom() - border, r.bottom()};
    const float us[4] = {sprite->u0, sprite->u0 + du, sprite->u1 - du, sprite->u1};
    const float vs[4] = {sprite->v0, sprite->v0 + dv, sprite->v1 - dv, sprite->v1};

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            UiSprite piece = *sprite;
            piece.u0 = us[column];
            piece.u1 = us[column + 1];
            piece.v0 = vs[row];
            piece.v1 = vs[row + 1];
            quad(Rect{xs[column], ys[row], xs[column + 1] - xs[column],
                      ys[row + 1] - ys[row]},
                 piece, tint);
        }
    }
}

void Ui::fill(const Rect& r, const TextColor& color) {
    const UiSprite* sprite = lookup("white");
    quad(r, sprite != nullptr ? *sprite : fallbackSprite(), color);
}

void Ui::icon(const Rect& r, const char* spriteName, const TextColor& tint) {
    const UiSprite* sprite = lookup(spriteName);
    if (sprite == nullptr) return;

    // Вписываем целиком, сохраняя пропорции: растянутый значок читается
    // как ошибка вёрстки, даже если человек не может сказать, какая.
    const float aspect = float(sprite->width) / float(std::max(1, sprite->height));
    float w = r.w;
    float h = r.w / aspect;
    if (h > r.h) {
        h = r.h;
        w = r.h * aspect;
    }
    quad(Rect{r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h}, *sprite, tint);
}

void Ui::text(float x, float y, const std::string& value, const TextColor& color) {
    frame_.lines.push_back(value);
    if (font_ == nullptr) return;
    font_->layout(value, x, y, lineHeight_, color, batchFor(UiTexture::Font).sprites);
}

float Ui::textWidth(const std::string& value) const {
    if (font_ == nullptr) return float(value.size()) * lineHeight_ * 0.47f;
    return font_->width(value, lineHeight_);
}

void Ui::textRight(const Rect& r, const std::string& value, const TextColor& color) {
    text(r.right() - textWidth(value), r.y + (r.h - lineHeight_) * 0.5f, value, color);
}

void Ui::textCentered(const Rect& r, const std::string& value, const TextColor& color) {
    text(r.x + (r.w - textWidth(value)) * 0.5f, r.y + (r.h - lineHeight_) * 0.5f, value,
         color);
}

void Ui::progress(const Rect& r, float value, const TextColor& color) {
    panel(r, "bar_back");
    const float filled = std::clamp(value, 0.0f, 1.0f);
    if (filled <= 0.0f) return;

    const Rect inner = r.inset(2.0f);
    const UiSprite* sprite = lookup("bar_fill");
    quad(Rect{inner.x, inner.y, inner.w * filled, inner.h},
         sprite != nullptr ? *sprite : fallbackSprite(), color);
}

// ---------------------------------------------------------------------------
// Поведение
// ---------------------------------------------------------------------------

ButtonResult Ui::behaviour(uint32_t id, const Rect& r, bool enabled) {
    ButtonResult result;
    if (!enabled) return result;

    const bool inside = r.contains(input_.mouseX, input_.mouseY);
    if (inside) {
        hot_ = id;
        wantsMouse_ = true;
        result.hovered = true;
        if (input_.pressed) active_ = id;
    }

    // Срабатывает по ОТПУСКАНИЮ и только если курсор всё ещё на кнопке.
    // Иначе промахнуться после нажатия нельзя, и любое случайное нажатие
    // становится необратимым — а среди кнопок здесь есть снос здания.
    if (active_ == id) {
        wantsMouse_ = true;
        if (input_.released) {
            result.clicked = inside;
            active_ = 0;
        }
    }
    return result;
}

const char* Ui::stylePlate(ButtonStyle style, bool hovered, bool held,
                           bool enabled) const {
    if (!enabled) return "panel_dark";
    if (held) return "button_down";
    switch (style) {
        case ButtonStyle::Accent: return hovered ? "button_hover" : "button_accent";
        case ButtonStyle::Danger: return hovered ? "button_hover" : "button_danger";
        case ButtonStyle::Quiet:  return hovered ? "button" : nullptr;
        default:                  return hovered ? "button_hover" : "button";
    }
}

ButtonResult Ui::button(uint32_t id, const Rect& r, const std::string& label,
                        ButtonStyle style, bool enabled) {
    const ButtonResult result = behaviour(id, r, enabled);
    const bool held = active_ == id && input_.down;

    if (const char* plate = stylePlate(style, result.hovered, held, enabled)) {
        panel(r, plate);
    }

    // Нажатая кнопка сдвигает надпись на пиксель вниз. Мелочь, которой
    // палец верит: без неё нажатие ощущается как подсветка, а не как
    // нажатие.
    const float shift = held ? 1.0f : 0.0f;
    textCentered(Rect{r.x, r.y + shift, r.w, r.h}, label,
                 enabled ? theme_.text : theme_.textDim);
    return result;
}

ButtonResult Ui::iconButton(uint32_t id, const Rect& r, const char* sprite,
                            const std::string& label, ButtonStyle style, bool enabled) {
    const ButtonResult result = behaviour(id, r, enabled);
    const bool held = active_ == id && input_.down;

    if (const char* plate = stylePlate(style, result.hovered, held, enabled)) {
        panel(r, plate);
    }

    const float shift = held ? 1.0f : 0.0f;
    const float pad = theme_.unit * 0.5f;
    const float iconSize = r.h - pad * 2.0f;
    icon(Rect{r.x + pad, r.y + pad + shift, iconSize, iconSize}, sprite,
         enabled ? TextColor{} : TextColor{0.6f, 0.6f, 0.6f, 0.6f});

    if (!label.empty()) {
        text(r.x + pad * 2.0f + iconSize,
             r.y + (r.h - lineHeight_) * 0.5f + shift, label,
             enabled ? theme_.text : theme_.textDim);
    }
    return result;
}

ButtonResult Ui::slot(uint32_t id, const Rect& r, const char* sprite, bool selected,
                      bool enabled) {
    const ButtonResult result = behaviour(id, r, enabled);
    const bool held = active_ == id && input_.down;

    const char* plate = held               ? "button_down"
                        : selected         ? "button_accent"
                        : result.hovered   ? "slot_hover"
                                           : "slot";
    panel(r, plate);
    if (sprite != nullptr) {
        icon(r.inset(theme_.unit * 0.4f), sprite,
             enabled ? TextColor{} : TextColor{0.55f, 0.58f, 0.62f, 0.7f});
    }
    return result;
}

ButtonResult Ui::hotspot(uint32_t id, const Rect& r, bool enabled) {
    return behaviour(id, r, enabled);
}

void Ui::tooltip(const std::string& value) {
    if (value.empty()) return;
    tooltipText_ = value;
    tooltipX_ = input_.mouseX;
    tooltipY_ = input_.mouseY;
}

}  // namespace pw::render
