#pragma once

// Интерфейс: панели, кнопки, подсказки.
//
// НЕМЕДЛЕННЫЙ РЕЖИМ. Экран собирается заново каждый кадр из состояния,
// которое и так уже есть: ресурсы империи, выбранная планета, ход стройки.
// Дерева виджетов нет, синхронизировать нечего, и рассинхрону интерфейса
// с миром просто неоткуда взяться — а это самый частый и самый противный
// класс ошибок в игровом UI.
//
// Между кадрами живут ровно два числа: над чем курсор и что зажато.
// Всё остальное — производное.
//
// НИЧЕГО НЕ ЗНАЕТ НИ ПРО VULKAN, НИ ПРО ОКНО. Отдаёт списки спрайтов,
// как MapView и SystemView. Значит экран собирается и проверяется без
// видеокарты: тест кладёт состояние, просит собрать кадр и читает, какие
// кнопки в нём оказались и куда попал бы щелчок.

#include <cstdint>
#include <string>
#include <vector>

#include "pw/render/font.h"
#include "pw/render/ui_atlas.h"
#include "pw/rhi/rhi.h"

namespace pw::render {

/// Прямоугольник в пикселях экрана. Начало в левом верхнем углу.
struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    Rect inset(float by) const { return Rect{x + by, y + by, w - by * 2, h - by * 2}; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
};

/// Что мышь делала в этом кадре.
struct UiInput {
    float mouseX = 0.0f, mouseY = 0.0f;
    bool down = false;      // кнопка удерживается
    bool pressed = false;   // нажата именно в этом кадре
    bool released = false;
};

/// Куда рисовать пачку спрайтов: атлас интерфейса или атлас шрифта.
enum class UiTexture : uint8_t { Atlas, Font };

/// Одна пачка спрайтов подряд идущего слоя.
///
/// Пачки хранятся СПИСКОМ, а не двумя массивами, потому что порядок
/// важен: подсказка обязана лечь поверх и панели, и текста под ней.
/// Два массива «сначала все панели, потом весь текст» этого не дают.
struct UiBatch {
    UiTexture texture = UiTexture::Atlas;
    std::vector<rhi::SpriteInstance> sprites;
};

struct UiFrame {
    std::vector<UiBatch> batches;
    /// Строки как текст — для проверок и отладочного вывода.
    std::vector<std::string> lines;

    void clear() {
        batches.clear();
        lines.clear();
    }
    size_t spriteCount() const {
        size_t total = 0;
        for (const UiBatch& batch : batches) total += batch.sprites.size();
        return total;
    }
};

/// Палитра интерфейса.
///
/// Собрана в одном месте, чтобы её можно было поменять целиком, а не
/// охотиться за литералами по всему файлу. Цвета подобраны так, чтобы
/// различаться и по светлоте: интерфейс, который читается только
/// по оттенку, не читается при дальтонизме.
struct UiTheme {
    TextColor text{0.90f, 0.93f, 0.97f, 1.0f};
    TextColor textDim{0.60f, 0.65f, 0.73f, 1.0f};
    TextColor textGood{0.56f, 0.84f, 0.58f, 1.0f};
    TextColor textWarn{0.94f, 0.76f, 0.36f, 1.0f};
    TextColor textBad{0.92f, 0.48f, 0.46f, 1.0f};
    TextColor textAccent{0.56f, 0.78f, 0.98f, 1.0f};

    /// Базовый шаг сетки. Все отступы кратны ему, и из этого сам собой
    /// получается ритм — то, чем сделанный руками интерфейс отличается
    /// от собранного из случайных чисел.
    float unit = 8.0f;
};

/// Вид кнопки.
enum class ButtonStyle : uint8_t {
    Normal,
    Accent,   // основное действие экрана
    Danger,   // снос, отмена
    Quiet,    // как строка списка: подложка появляется только под курсором
};

/// Что вернула кнопка.
struct ButtonResult {
    bool clicked = false;
    bool hovered = false;
    operator bool() const { return clicked; }
};

class Ui {
public:
    void setFont(const Font* font) { font_ = font; }
    void setAtlas(const UiAtlas* atlas) { atlas_ = atlas; }
    void setTheme(const UiTheme& theme) { theme_ = theme; }

    const UiTheme& theme() const { return theme_; }
    const UiFrame& frame() const { return frame_; }

    /// Начать кадр. Дальше идут вызовы виджетов, потом end().
    void begin(const UiInput& input, int screenWidth, int screenHeight);
    void end();

    /// Спрайты, которых экран просил, а в атласе не нашлось.
    ///
    /// Копится, а не падает: интерфейс обязан рисоваться и на неполном
    /// атласе. Но молча подменять пропажу заглушкой нельзя — переименовали
    /// спрайт в Blender, и кнопка тихо теряет подложку, а замечает это
    /// игрок. Список пуст — проверяется тестом на живом экране.
    const std::vector<std::string>& missing() const { return missing_; }

    /// Забрал ли интерфейс мышь. Мир обязан спросить это ПЕРЕД тем, как
    /// обрабатывать щелчок: иначе нажатие на кнопку заодно отдаёт приказ
    /// флоту в системе под ней.
    bool wantsMouse() const { return wantsMouse_; }

    /// Высота строки текста. Растёт с экраном: на 4K надпись в 14 пикселей
    /// нечитаема, на ноутбуке в 28 занимает пол-экрана.
    float lineHeight() const { return lineHeight_; }
    float unit() const { return theme_.unit; }
    int screenWidth() const { return width_; }
    int screenHeight() const { return height_; }

    // --- рисование ---

    /// Панель с растяжкой рамки по девяти частям.
    void panel(const Rect& r, const char* sprite = "panel", float alpha = 1.0f);
    /// Сплошной прямоугольник заданного цвета.
    void fill(const Rect& r, const TextColor& color);
    /// Значок. Вписывается в прямоугольник целиком, с сохранением пропорций.
    void icon(const Rect& r, const char* sprite, const TextColor& tint = {});
    /// Надпись. `x`, `y` — левый верхний угол.
    void text(float x, float y, const std::string& value, const TextColor& color);
    /// Надпись, выровненная по правому краю прямоугольника.
    void textRight(const Rect& r, const std::string& value, const TextColor& color);
    /// Надпись по центру прямоугольника.
    void textCentered(const Rect& r, const std::string& value, const TextColor& color);
    /// Полоса: подложка плюс заливка. `value` от нуля до единицы.
    void progress(const Rect& r, float value, const TextColor& color);

    /// Ширина строки в пикселях.
    float textWidth(const std::string& value) const;

    // --- виджеты ---

    /// Кнопка с надписью. `id` обязан быть уникальным в пределах кадра.
    ButtonResult button(uint32_t id, const Rect& r, const std::string& label,
                        ButtonStyle style = ButtonStyle::Normal, bool enabled = true);
    /// Кнопка со значком и надписью справа от него.
    ButtonResult iconButton(uint32_t id, const Rect& r, const char* sprite,
                            const std::string& label,
                            ButtonStyle style = ButtonStyle::Normal, bool enabled = true);
    /// Ячейка: квадратная кнопка только со значком. Для сетки застройки.
    ButtonResult slot(uint32_t id, const Rect& r, const char* sprite, bool selected,
                      bool enabled = true);
    /// Невидимая кнопка: сама ничего не рисует, но ловит щелчок и наведение.
    /// Ею делаются кликабельные строки списков.
    ButtonResult hotspot(uint32_t id, const Rect& r, bool enabled = true);

    /// Подсказка под курсором. Рисуется в конце кадра, поверх всего.
    ///
    /// Копится, а не рисуется сразу, именно поэтому: подсказка, нарисованная
    /// в момент вызова, оказывалась бы под панелями, объявленными позже.
    void tooltip(const std::string& text);

private:
    const Font* font_ = nullptr;
    const UiAtlas* atlas_ = nullptr;
    UiTheme theme_;
    UiFrame frame_;

    UiInput input_;
    int width_ = 0, height_ = 0;
    float lineHeight_ = 16.0f;

    /// Над каким виджетом курсор и какой зажат.
    ///
    /// Активный запоминается на время удержания: кнопка обязана
    /// срабатывать по ОТПУСКАНИЮ и только если курсор всё ещё на ней.
    /// Иначе промахнуться после нажатия нельзя, и любое случайное
    /// нажатие становится необратимым.
    uint32_t hot_ = 0;
    uint32_t active_ = 0;
    bool wantsMouse_ = false;

    std::string tooltipText_;
    float tooltipX_ = 0.0f, tooltipY_ = 0.0f;
    std::vector<std::string> missing_;

    /// Найти спрайт, записав промах. Возвращает nullptr, если атласа нет
    /// вовсе: без ассетов интерфейс всё равно должен рисоваться.
    const UiSprite* lookup(const char* name);
    UiBatch& batchFor(UiTexture texture);
    void quad(const Rect& r, const UiSprite& sprite, const TextColor& tint);
    ButtonResult behaviour(uint32_t id, const Rect& r, bool enabled);
    const char* stylePlate(ButtonStyle style, bool hovered, bool held,
                           bool enabled) const;
};

/// Устойчивый номер виджета из строки.
///
/// Строкой, а не числом: номер, посчитанный вручную, разъезжается при
/// первой же вставке кнопки в середину, и нажатие начинает срабатывать
/// не на ту. FNV-1a — тот же хеш, что и во всём остальном проекте.
constexpr uint32_t uiId(const char* name, uint32_t salt = 0) {
    uint32_t hash = 2166136261u ^ salt;
    for (const char* p = name; *p != '\0'; ++p) {
        hash ^= uint32_t(uint8_t(*p));
        hash *= 16777619u;
    }
    // Ноль зарезервирован под «ничего»: виджет с номером ноль был бы
    // всегда «горячим», и весь экран отзывался бы на любое движение мыши.
    return hash == 0 ? 1u : hash;
}

}  // namespace pw::render
