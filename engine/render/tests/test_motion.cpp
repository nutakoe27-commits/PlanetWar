// Движение: анимация интерфейса и эффекты мира.
//
// ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ И ПОЧЕМУ ИМЕННО ЭТО.
//
// Анимацию нельзя проверить глазом по одному снимку — снимок показывает
// одну фазу из полусотни. Зато у неё есть свойства, которые проверяются
// точно, и все они про то, чтобы движение НЕ СЛОМАЛО игру:
//
//   1. По умолчанию его нет. Кадр, собранный тестом или снимком, обязан
//      быть устоявшимся, иначе все остальные проверки начнут ловить
//      случайную фазу перехода и падать через раз.
//   2. Оно не зависит от частоты кадров. Иначе на слабой машине панель
//      выезжает вдвое медленнее, а полоса догоняет вдвое дольше.
//   3. Оно ничего не решает. Кнопка нажимается ровно так же, пока
//      подложка перетекает: анимация, наказывающая за скорость, — худший
//      вид анимации.
//   4. Оно заканчивается. Значение доезжает до цели точно, вспышка гаснет
//      в ноль, эффект умирает, а таблица номеров чистится: интерфейс,
//      который вечно «почти доехал», вечно и перерисовывается.
#include "doctest.h"

#include <cmath>

#include "pw/render/effects.h"
#include "pw/render/map_view.h"
#include "pw/render/ui.h"

using namespace pw;
using namespace pw::render;

namespace {

/// Сколько кадров укладывается в эти секунды при таком шаге.
///
/// Считается ЦЕЛЫМ ЧИСЛОМ, а не накоплением `t += step`: накопление
/// на разных шагах даёт разное суммарное время (восемь кадров по 1/30
/// это 0.267 с, а не 0.25), и проверка независимости от частоты кадров
/// падала бы на собственной арифметике, а не на коде.
int framesIn(float seconds, float step) {
    const int count = int(seconds / step + 0.5f);
    return count > 0 ? count : 1;
}

/// Прогнать столько секунд «вхолостую»: номера при этом не трогаются.
void run(Ui& ui, float seconds, float step = 1.0f / 60.0f) {
    const int frames = framesIn(seconds, step);
    for (int i = 0; i < frames; ++i) {
        ui.begin(UiInput{}, 800, 600, step);
        ui.end();
    }
}

/// Прогнать столько секунд, каждый кадр догоняя цель. Именно так живёт
/// клиент: значение движется, только пока его СПРАШИВАЮТ.
float chase(Ui& ui, uint32_t id, float target, float seconds, float over,
            float step = 1.0f / 60.0f) {
    float value = 0.0f;
    const int frames = framesIn(over, step);
    for (int i = 0; i < frames; ++i) {
        ui.begin(UiInput{}, 800, 600, step);
        value = ui.approach(id, target, seconds);
        ui.end();
    }
    return value;
}

constexpr uint32_t kId = uiId("проба");

}  // namespace

// ---------------------------------------------------------------------------
// Движение выключено по умолчанию
// ---------------------------------------------------------------------------

TEST_CASE("движение: по умолчанию выключено, и кадр устоявшийся") {
    // САМОЕ ВАЖНОЕ СВОЙСТВО ВО ВСЁМ ФАЙЛЕ. Если бы движение было включено
    // по умолчанию, каждый второй тест экрана начал бы ловить середину
    // перехода: панель наполовину прозрачна, число досчитано наполовину.
    // Такие тесты «чинят» ожиданиями с допуском, и через месяц они
    // не проверяют ничего.
    Ui ui;
    CHECK_FALSE(ui.motion());

    ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
    // Догоняющее значение стоит СРАЗУ на цели.
    CHECK(ui.approach(kId, 42.0f, 0.5f) == doctest::Approx(42.0f));
    // Появление — сразу целиком.
    CHECK(ui.appear(uiId("панель"), 0.3f) == doctest::Approx(1.0f));
    // Вспышки нет вовсе: одиночный кадр не может показать затухание,
    // а мгновенная белая заливка — это дефект, а не отклик.
    CHECK(ui.flash(uiId("искра"), /*trigger=*/true, 0.2f) == doctest::Approx(0.0f));
    // Колебание стоит на нуле.
    CHECK(ui.pulse(1.0f) == doctest::Approx(0.0f));
    ui.end();
}

TEST_CASE("движение: без шага времени ничего не движется") {
    // Клиент, который забыл передать шаг кадра, обязан получить
    // устоявшуюся картинку, а не застывшую на нуле.
    Ui ui;
    ui.setMotion(true);
    ui.begin(UiInput{}, 800, 600, /*deltaSeconds=*/0.0f);
    CHECK(ui.approach(kId, 7.0f, 0.5f) == doctest::Approx(7.0f));
    ui.end();
}

// ---------------------------------------------------------------------------
// Догон цели
// ---------------------------------------------------------------------------

TEST_CASE("движение: значение догоняет цель и доезжает точно") {
    Ui ui;
    ui.setMotion(true);

    // Первое появление номера — сразу на цели: иначе каждое открытие
    // панели начиналось бы с разгона нуля до правды.
    ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
    CHECK(ui.approach(kId, 0.0f, 0.4f) == doctest::Approx(0.0f));
    ui.end();

    // Дальше цель меняется, и значение идёт к ней постепенно.
    float seen = 0.0f;
    ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
    seen = ui.approach(kId, 100.0f, 0.4f);
    ui.end();
    CHECK(seen > 0.0f);
    CHECK(seen < 100.0f);   // не телепортировалось

    // За объявленный срок покрывается почти весь путь.
    const float almost = chase(ui, kId, 100.0f, /*seconds=*/0.4f, /*over=*/0.4f);
    CAPTURE(almost);
    CHECK(almost > 90.0f);

    // И оно ДОЕЗЖАЕТ до цели точно, а не застревает на 99.9: значение,
    // вечно «почти доехавшее», заставляет интерфейс вечно перерисовываться.
    seen = chase(ui, kId, 100.0f, /*seconds=*/0.4f, /*over=*/3.0f);
    CHECK(seen == doctest::Approx(100.0f));
}

TEST_CASE("движение: не зависит от частоты кадров") {
    // Иначе на слабой машине панель выезжает вдвое медленнее, а полоса
    // догоняет вдвое дольше — то есть игра ощущается по-разному
    // на разном железе без всякой на то причины.
    // Число кадров задаётся ЯВНО, а не считается из секунд: 0.25 секунды
    // при шаге 1/30 — это семь с половиной кадров, и любое округление
    // даёт разное суммарное время. Тогда проверка ловила бы собственную
    // арифметику вместо кода.
    auto after = [](float step, int frames) {
        Ui ui;
        ui.setMotion(true);
        ui.begin(UiInput{}, 800, 600, step);
        ui.approach(kId, 0.0f, 0.5f);   // завели номер на нуле
        ui.end();

        float value = 0.0f;
        for (int i = 0; i < frames; ++i) {
            ui.begin(UiInput{}, 800, 600, step);
            value = ui.approach(kId, 1.0f, 0.5f);
            ui.end();
        }
        return value;
    };

    // Одно и то же игровое время — треть секунды — разными шагами.
    const float fast = after(1.0f / 120.0f, 40);
    const float slow = after(1.0f / 40.0f, 13);
    CAPTURE(fast);
    CAPTURE(slow);
    CHECK(std::fabs(fast - slow) < 0.01f);
}

TEST_CASE("движение: длинный провал между кадрами не доигрывает всё разом") {
    // Окно свернули на минуту. Без ограничения шага все переходы за один
    // кадр доехали бы до конца — то есть выглядели бы мгновенными ровно
    // тогда, когда игрок возвращается и больше всего хочет понять,
    // что изменилось.
    Ui ui;
    ui.setMotion(true);
    ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
    ui.approach(kId, 0.0f, 1.0f);
    ui.end();

    ui.begin(UiInput{}, 800, 600, /*deltaSeconds=*/60.0f);
    const float value = ui.approach(kId, 1.0f, 1.0f);
    ui.end();
    CHECK(value > 0.0f);
    CHECK(value < 0.5f);   // за один кадр — не больше восьмой доли секунды
}

// ---------------------------------------------------------------------------
// Появление и вспышка
// ---------------------------------------------------------------------------

TEST_CASE("движение: появление начинается с нуля и доходит до единицы") {
    Ui ui;
    ui.setMotion(true);

    ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
    const float first = ui.appear(uiId("карточка"), 0.2f);
    ui.end();
    CHECK(first < 0.5f);   // первое появление — почти с нуля

    float value = first;
    for (int i = 0; i < 120; ++i) {
        ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
        value = ui.appear(uiId("карточка"), 0.2f);
        ui.end();
    }
    CHECK(value == doctest::Approx(1.0f));
}

TEST_CASE("движение: вспышка загорается и гаснет в ноль") {
    Ui ui;
    ui.setMotion(true);
    const uint32_t id = uiId("вспышка");

    ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
    CHECK(ui.flash(id, /*trigger=*/true, 0.2f) == doctest::Approx(1.0f));
    ui.end();

    ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
    const float fading = ui.flash(id, /*trigger=*/false, 0.2f);
    ui.end();
    CHECK(fading > 0.0f);
    CHECK(fading < 1.0f);

    float value = fading;
    for (int i = 0; i < 60; ++i) {
        ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
        value = ui.flash(id, /*trigger=*/false, 0.2f);
        ui.end();
    }
    CHECK(value == doctest::Approx(0.0f));
}

TEST_CASE("движение: колебание держится в своих границах") {
    Ui ui;
    ui.setMotion(true);
    float low = 2.0f, high = -1.0f;
    for (int i = 0; i < 240; ++i) {
        ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
        const float value = ui.pulse(1.0f);
        low = std::min(low, value);
        high = std::max(high, value);
        ui.end();
    }
    CHECK(low >= 0.0f);
    CHECK(high <= 1.0f);
    // И оно действительно колеблется, а не стоит: иначе проверка границ
    // прошла бы и на константе.
    CHECK(high - low > 0.9f);
}

// ---------------------------------------------------------------------------
// Движение ничего не решает
// ---------------------------------------------------------------------------

TEST_CASE("движение: кнопка нажимается, пока подложка перетекает") {
    // ГЛАВНОЕ ОБЕЩАНИЕ АНИМАЦИИ В ЭТОМ ПРОЕКТЕ. Прямоугольники попадания,
    // порядок виджетов и результат щелчка от неё не зависят: анимация,
    // наказывающая за скорость, — худший её вид.
    Ui ui;
    ui.setMotion(true);
    const Rect box{100.0f, 100.0f, 120.0f, 40.0f};

    UiInput press;
    press.mouseX = 150.0f;
    press.mouseY = 120.0f;
    press.down = true;
    press.pressed = true;

    // Нажали в первом же кадре — движение ещё не началось.
    ui.begin(press, 800, 600, 1.0f / 60.0f);
    CHECK_FALSE(ui.button(uiId("кнопка"), box, "ок"));
    ui.end();

    UiInput release = press;
    release.pressed = false;
    release.down = false;
    release.released = true;

    ui.begin(release, 800, 600, 1.0f / 60.0f);
    CHECK(ui.button(uiId("кнопка"), box, "ок"));
    ui.end();
}

TEST_CASE("движение: таблица номеров не растёт бесконечно") {
    // Интерфейс немедленного режима не имеет дерева, и состояние живёт
    // по номерам. Номера кнопок, которых давно нет на экране, обязаны
    // забываться: за час игры их иначе накопятся десятки тысяч.
    Ui ui;
    ui.setMotion(true);

    for (uint32_t i = 0; i < 300; ++i) {
        ui.begin(UiInput{}, 800, 600, 1.0f / 60.0f);
        ui.approach(uiId("однодневка", i), 1.0f, 0.2f);
        ui.end();
    }
    const size_t peak = ui.motionCount();
    CHECK(peak > 0);

    // Дальше про них никто не вспоминает.
    run(ui, 10.0f);
    CAPTURE(peak);
    CAPTURE(ui.motionCount());
    CHECK(ui.motionCount() < peak);
}

// ---------------------------------------------------------------------------
// Эффекты мира
// ---------------------------------------------------------------------------

TEST_CASE("эффекты: живут ровно свой срок и исчезают без следа") {
    Effects effects;
    CHECK(effects.count() == 0);

    effects.spawn(EffectKind::Colonized, 10.0f, 20.0f, 3.0f, EmpireColor{1, 1, 1});
    CHECK(effects.count() == 1);

    // На середине жизни эффект ещё рисуется.
    effects.update(effectLifetime(EffectKind::Colonized) * 0.5f);
    MapFrame frame;
    effects.build(frame);
    CHECK_FALSE(frame.lines.empty());
    // И только отрезками: спрайты — это модели из Blender, а кольцо
    // и трасса моделями не являются.
    CHECK(frame.sprites.empty());

    // К концу срока не остаётся ничего.
    effects.update(effectLifetime(EffectKind::Colonized) * 0.6f);
    CHECK(effects.count() == 0);

    frame.clear();
    effects.build(frame);
    CHECK(frame.lines.empty());
}

TEST_CASE("эффекты: у каждого вида свой срок, и все они разумны") {
    // Срок — это дизайнерское решение, а не случайное число. Слишком
    // короткий эффект не успевают заметить, слишком длинный превращает
    // карту в мигающую ёлку.
    for (uint8_t kind = 0; kind < uint8_t(EffectKind::Count); ++kind) {
        const float life = effectLifetime(EffectKind(kind));
        CAPTURE(int(kind));
        CHECK(life > 0.3f);
        CHECK(life < 5.0f);
    }
    // Потеря своей планеты держится дольше всех: это худшая новость в игре.
    CHECK(effectLifetime(EffectKind::Lost) > effectLifetime(EffectKind::Captured));
    // Отклик на щелчок — короче всех: он подтверждает приказ, а не
    // сообщает новость.
    CHECK(effectLifetime(EffectKind::Order) < effectLifetime(EffectKind::Battle));
}

TEST_CASE("эффекты: волна событий не топит карту") {
    // Волна кризиса приносит десяток сражений разом. Без предела карта
    // на секунду превращается в мигающую ёлку, на которой не видно
    // ни одного из событий.
    Effects effects;
    for (uint32_t i = 0; i < kMaxEffects * 3; ++i) {
        effects.spawn(EffectKind::Battle, float(i), 0.0f, 1.0f, EmpireColor{1, 0, 0}, i);
    }
    CHECK(effects.count() <= kMaxEffects);

    // Вытеснены СТАРШИЕ: свежее событие важнее позавчерашнего.
    bool keptLatest = false;
    for (const Effect& effect : effects.live()) {
        if (effect.x == doctest::Approx(float(kMaxEffects * 3 - 1))) keptLatest = true;
    }
    CHECK(keptLatest);
}

TEST_CASE("эффекты: разные виды рисуются по-разному") {
    // Одинаковые кольца разных цветов игрок различает хуже, чем разные
    // формы. Проверяется именно это: у колонизации и сражения не должно
    // получаться одной и той же геометрии.
    auto geometry = [](EffectKind kind) {
        Effects effects;
        effects.spawn(kind, 0.0f, 0.0f, 4.0f, EmpireColor{1, 1, 1}, 7);
        effects.update(effectLifetime(kind) * 0.4f);
        MapFrame frame;
        effects.build(frame);
        return frame.lines;
    };

    const auto colonized = geometry(EffectKind::Colonized);
    const auto battle = geometry(EffectKind::Battle);
    CHECK_FALSE(colonized.empty());
    CHECK_FALSE(battle.empty());
    CHECK(colonized.size() != battle.size());
}

TEST_CASE("эффекты: неподвижный шаг ничего не старит") {
    Effects effects;
    effects.spawn(EffectKind::Lost, 0.0f, 0.0f, 1.0f, EmpireColor{1, 0, 0});
    effects.update(0.0f);
    effects.update(-5.0f);   // отрицательный шаг не воскрешает и не убивает
    CHECK(effects.count() == 1);
}

// ---------------------------------------------------------------------------
// Часы карты
// ---------------------------------------------------------------------------

TEST_CASE("карта: без часов кадр повторяется, с часами дышит") {
    // Негативный контроль на пульс: если бы часы никуда не доезжали,
    // первая проверка прошла бы сама собой, и «дыхание» кольца выделения
    // существовало бы только в комментарии.
    sim::Galaxy galaxy;
    sim::World world;
    sim::registerGalaxyComponents(world);
    sim::GalaxyParams params;
    params.seed = 0xA11CE;
    params.systemCount = 40;
    galaxy.generate(world, params);

    game::WorldView view;
    view.resize(galaxy.systemCount());

    Selection selection;
    selection.system = 3;

    rhi::Camera camera;
    camera.worldHeight = 2000.0f;

    MapView map;
    MapFrame still, alsoStill, breathing;

    map.setClock(0.0f);
    map.build(galaxy, view, /*empire=*/0, selection, camera, still);
    map.build(galaxy, view, /*empire=*/0, selection, camera, alsoStill);
    REQUIRE(still.lines.size() == alsoStill.lines.size());
    REQUIRE_FALSE(still.lines.empty());

    bool identical = true;
    for (size_t i = 0; i < still.lines.size(); ++i) {
        if (still.lines[i].x != alsoStill.lines[i].x) identical = false;
    }
    CHECK(identical);

    // Полсекунды спустя кольцо выделения стоит уже не там.
    map.setClock(0.5f);
    map.build(galaxy, view, /*empire=*/0, selection, camera, breathing);
    REQUIRE(breathing.lines.size() == still.lines.size());

    bool moved = false;
    for (size_t i = 0; i < still.lines.size(); ++i) {
        if (still.lines[i].x != breathing.lines[i].x) moved = true;
    }
    CHECK(moved);
}
