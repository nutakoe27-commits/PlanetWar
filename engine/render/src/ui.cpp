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

    // ПЕРЕНОС ПО СЛОВАМ. Подсказка в одну строку тянулась во всю ширину
    // экрана: «ваша · слотов 9, свободно 7 · оборона 100% из 100 — упадёт
    // до нуля, и планету заберут» — это тысяча с лишним пикселей текста,
    // который глаз обязан пройти слева направо целиком. Строка длиннее
    // примерно шестидесяти знаков перестаёт читаться с одного взгляда,
    // и подсказка из помощи превращается в работу.
    const float maxWidth = std::min(float(width_) * 0.26f, lineHeight_ * 26.0f);
    const std::vector<std::string> lines = wrap(tooltipText_, maxWidth);
    if (lines.empty()) return;

    float widest = 0.0f;
    for (const std::string& line : lines) widest = std::max(widest, textWidth(line));

    const float step = lineHeight_ * 1.25f;
    const float width = widest + pad * 2.0f;
    const float height = step * float(lines.size()) + pad * 1.2f;

    // Уводим от края экрана: подсказка, наполовину вылезшая за монитор, —
    // это подсказка, которую не прочитать.
    float x = tooltipX_ + theme_.unit * 1.5f;
    float y = tooltipY_ + theme_.unit * 1.5f;
    if (x + width > float(width_) - pad) x = float(width_) - pad - width;
    if (y + height > float(height_) - pad) y = tooltipY_ - height - theme_.unit;
    x = std::max(pad, x);
    y = std::max(pad, y);

    const Rect box{x, y, width, height};
    panel(box, "hud_header");
    for (size_t index = 0; index < lines.size(); ++index) {
        text(x + pad, y + pad * 0.6f + step * float(index), lines[index], theme_.text);
    }
}

/// Разбить строку на строки не шире `maxWidth`.
///
/// По пробелам: рвать слово посередине хуже, чем вылезти за границу
/// на одно длинное слово. Границы предложений тут нет — подсказки
/// пишутся короткими и разделяются точкой-разделителем.
std::vector<std::string> Ui::wrap(const std::string& value, float maxWidth) const {
    std::vector<std::string> lines;
    std::string current;

    size_t at = 0;
    while (at <= value.size()) {
        const size_t space = value.find(' ', at);
        const std::string word = value.substr(at, space - at);

        const std::string candidate = current.empty() ? word : current + " " + word;
        if (!current.empty() && textWidth(candidate) > maxWidth) {
            lines.push_back(current);
            current = word;
        } else {
            current = candidate;
        }

        if (space == std::string::npos) break;
        at = space + 1;
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
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
    // Полоса рисуется НЕ ТОНЬШЕ трёх пикселей и всегда с подложкой.
    //
    // Первая версия брала высоту из вызывающего кода, и полоса обороны
    // выходила в пять пикселей на подложке того же цвета, что панель:
    // на снимке она читалась как чёрточка, а не как индикатор. Полоса
    // без видимого жёлоба не сообщает, сколько ОСТАЛОСЬ, — а именно это
    // от неё и нужно.
    const Rect track{r.x, r.y, r.w, std::max(r.h, 4.0f)};
    fill(track, theme_.track);
    // Обводка в один пиксель: без неё жёлоб сливается с подложкой панели,
    // и полоса снова читается как чёрточка, а не как «столько осталось».
    fill(Rect{track.x, track.y, track.w, 1.0f}, theme_.edgeDim);
    fill(Rect{track.x, track.bottom() - 1.0f, track.w, 1.0f}, theme_.edgeDim);

    const float filled = std::clamp(value, 0.0f, 1.0f);
    if (filled <= 0.0f) return;

    const float pad = track.h > 8.0f ? 2.0f : 1.0f;
    const Rect inner = track.inset(pad);
    const UiSprite* sprite = lookup("bar_fill");
    quad(Rect{inner.x, inner.y, std::max(1.0f, inner.w * filled), inner.h},
         sprite != nullptr ? *sprite : fallbackSprite(), color);
}

void Ui::listRow(const Rect& r, bool hovered, bool selected) {
    // Спокойная строка не рисуется вовсе. Список из десяти подложек —
    // это стопка кнопок, а не перечень: глаз начинает выбирать там,
    // где надо просто просматривать.
    if (selected) {
        fill(r, theme_.rowActive);
    } else if (hovered) {
        fill(r, theme_.rowHover);
    } else {
        return;
    }
    // Отметка слева, а не рамка вокруг. Рамка обводит строку и тем
    // отделяет её от списка; полоска показывает место в списке,
    // не разрывая его.
    if (selected) {
        fill(Rect{r.x, r.y, std::max(2.0f, theme_.unit * 0.25f), r.h}, theme_.edge);
    }
}

Rect Ui::panelTitle(const Rect& panel, float height, const std::string& title,
                    const std::string& note, const TextColor* noteColor) {
    // Полоса рисуется ОТ КРАЯ ДО КРАЯ панели, заходя под её поля: заголовок
    // с отступами по бокам читается как ещё одна строка содержимого,
    // а не как шапка.
    const Rect band{panel.x + 1.0f, panel.y + 1.0f, panel.w - 2.0f, height};
    fill(band, theme_.headerFill);
    fill(Rect{band.x, band.bottom() - 1.0f, band.w, 1.0f}, theme_.edgeDim);

    const float pad = theme_.unit;
    text(band.x + pad, band.y + (band.h - lineHeight_) * 0.5f, title, theme_.text);
    if (!note.empty()) {
        textRight(Rect{band.x, band.y + (band.h - lineHeight_) * 0.5f, band.w - pad,
                       lineHeight_},
                  note, noteColor != nullptr ? *noteColor : theme_.textDim);
    }
    return band;
}

void Ui::sectionHeader(const Rect& r) {
    fill(r, theme_.headerFill);
    // Черта снизу: заголовок обязан читаться как крышка над строками,
    // а не как ещё одна строка того же списка.
    fill(Rect{r.x, r.bottom() - 1.0f, r.w, 1.0f}, theme_.edgeDim);
}

void Ui::separator(const Rect& r) {
    fill(Rect{r.x, std::round(r.y), r.w, 1.0f}, TextColor{1.0f, 1.0f, 1.0f, 0.07f});
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
    if (!enabled) return "hud_panel_deep";
    if (held) return "hud_button_down";
    switch (style) {
        case ButtonStyle::Accent: return hovered ? "hud_button_hover" : "hud_button_accent";
        case ButtonStyle::Danger: return hovered ? "hud_button_hover" : "hud_button_danger";
        case ButtonStyle::Quiet:  return hovered ? "hud_row_hover" : nullptr;
        default:                  return hovered ? "hud_button_hover" : "hud_button";
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

    const char* plate = held               ? "hud_button_down"
                        : selected         ? "hud_button_accent"
                        : result.hovered   ? "hud_slot_hover"
                                           : "hud_slot";
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
