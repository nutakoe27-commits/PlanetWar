#include "doctest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "pw/game/server.h"
#include "pw/render/screen.h"

using namespace pw;
using namespace pw::render;

// ---------------------------------------------------------------------------
// Окно отряда и рамка выделения
// ---------------------------------------------------------------------------
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ ФАЙЛ. Управление флотом — самая большая связка кнопок
// в игре: разделение, четыре стойки, уклонение, приписка, слияние,
// выделение группой. Разложить её по test_screen.cpp значило бы утопить
// в нём проверки застройки, а искать «почему покраснело разделение»
// в файле на тысячу строк дороже, чем завести второй.
//
// Проверки идут ПО ПОВЕДЕНИЮ, а не по координатам: тест обходит экран
// щелчками и смотрит, какое намерение вернулось. Раскладку окно вправе
// менять — обязанность у него одна: до каждой кнопки можно дотянуться
// мышью, и она отдаёт то, что обещает надпись.

namespace {

/// Сервер и клиент в одной памяти. Тот же приём, что в test_screen.cpp:
/// свой, а не общий, чтобы правка в чужом слое не ломала сборку здесь.
class LiveGame {
public:
    explicit LiveGame(uint32_t systems = 40) {
        game::ServerConfig config;
        config.galaxy.seed = 0xC0FFEE;
        config.galaxy.systemCount = systems;
        config.maxPlayers = 4;
        config.speed = 1;
        server_.start(config);
        client.connect(address_, "Командир", now_);
        run(600);
    }

    void run(int64_t milliseconds) {
        const int64_t until = now_ + milliseconds;
        while (now_ < until) step();
    }

    game::Client client;

private:
    void step() {
        ++now_;
        std::vector<game::OutgoingPacket> outgoing;
        server_.update(now_, outgoing);
        for (const game::OutgoingPacket& packet : outgoing) {
            client.receive(packet.data.data(), packet.data.size(), now_);
        }
        uint8_t buffer[net::kMaxPacketSize];
        const size_t size = client.update(now_, buffer, sizeof(buffer));
        if (size > 0) server_.receive(address_, buffer, size, now_);
    }

    game::Server server_;
    net::Address address_ = net::Address::loopback(20101);
    int64_t now_ = 0;
};

struct Squad {
    LiveGame game;
    Ui ui;
    Screen screen;
    MessageLog messages;
    ScreenState state;
    int screenWidth = 1600;
    int screenHeight = 900;

    Squad() {
        screen.setMessages(&messages);
        state.system = game.client.capital();
        const auto own = game.client.fleetsAt(state.system);
        if (!own.empty()) state.fleet = own.front();
        state.squadOpen = true;
    }

    ScreenAction frame(float x, float y, bool down, bool pressed, bool released) {
        UiInput input;
        input.mouseX = x;
        input.mouseY = y;
        input.down = down;
        input.pressed = pressed;
        input.released = released;
        ui.begin(input, screenWidth, screenHeight);
        const ScreenAction action = screen.build(ui, game.client, state, /*now=*/1000);
        ui.end();
        return action;
    }

    ScreenAction draw() { return frame(-100.0f, -100.0f, false, false, false); }

    ScreenAction click(float x, float y) {
        frame(x, y, true, true, false);
        return frame(x, y, false, false, true);
    }

    bool hasLine(const std::string& needle) const {
        for (const std::string& line : ui.frame().lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    /// Обойти весь экран щелчками и вернуть ПЕРВОЕ намерение, которое
    /// подошло. Шаг по горизонтали крупнее: кнопки шире, чем выше,
    /// и полный обход по пикселю стоил бы полминуты на тест.
    template <typename Match>
    ScreenAction hunt(Match match, int stepX = 5, int stepY = 3) {
        for (int y = 0; y < screenHeight; y += stepY) {
            for (int x = 0; x < screenWidth; x += stepX) {
                const ScreenAction action = click(float(x), float(y));
                if (match(action)) return action;
            }
        }
        return ScreenAction{};
    }

    const game::FleetView& fleet() const { return game.client.view().fleets.at(state.fleet); }
};

/// Один кадр полосы набора: нажали в точке, отпустили там же.
uint32_t barAt(float pickX, uint32_t start, uint32_t range) {
    Ui ui;
    const Rect bar{200.0f, 100.0f, 300.0f, 20.0f};
    const uint32_t id = uiId("bar");

    UiInput press;
    press.mouseX = pickX;
    press.mouseY = 110.0f;
    press.down = true;
    press.pressed = true;
    ui.begin(press, 800, 600);
    const DragResult result = ui.dragBar(id, bar, start, range, TextColor{});
    ui.end();
    return result.value;
}

}  // namespace

// ---------------------------------------------------------------------------
// Полоса набора
// ---------------------------------------------------------------------------

TEST_CASE("полоса: протяжка в середину берёт половину") {
    // РАДИ ЭТОГО ОНА И ЗАВЕДЕНА. Счётчиком «минус — число — плюс» отряд
    // из сорока корветов делится пополам за двадцать щелчков; полоса
    // делает то же одним движением.
    CHECK(barAt(350.0f, 0, 40) == 20u);
    CHECK(barAt(350.0f, 0, 8) == 4u);
}

TEST_CASE("полоса: края достижимы точно") {
    // Если край округляется внутрь, «забрать всех» мышью недостижимо
    // и остаётся только кнопка — а полоса тогда наполовину бесполезна.
    // Полоса лежит от 200 до 500, и 499 — её последний пиксель.
    CHECK(barAt(499.0f, 0, 12) == 12u);   // правый край
    CHECK(barAt(480.0f, 0, 12) == 12u);   // и заметно левее него
    CHECK(barAt(200.0f, 5, 12) == 0u);    // левый край
    CHECK(barAt(212.0f, 5, 12) == 0u);    // и заметно правее него
    // Зоны всех значений одной ширины: крайние не уже остальных.
    // Тринадцать значений на триста пикселей — по двадцать три с лишним
    // на каждое, и «ноль» с «двенадцатью» ловятся не труднее семёрки.
    CHECK(barAt(222.0f, 0, 12) == 0u);
    CHECK(barAt(224.0f, 0, 12) == 1u);
    CHECK(barAt(476.0f, 0, 12) == 11u);
    CHECK(barAt(478.0f, 0, 12) == 12u);
    // А щелчок точно по насечке отдаёт её число.
    CHECK(barAt(275.0f, 0, 12) == 3u);
    CHECK(barAt(425.0f, 0, 12) == 9u);
}

TEST_CASE("полоса: без кораблей ничего не отдаёт") {
    // Класс, которого в отряде нет, обязан молчать: иначе набор
    // ссылался бы на корабли, которых не существует.
    CHECK(barAt(350.0f, 0, 0) == 0u);
}

TEST_CASE("полоса: щелчок мимо не двигает число") {
    Ui ui;
    const Rect bar{200.0f, 100.0f, 300.0f, 20.0f};
    UiInput press;
    press.mouseX = 600.0f;   // правее полосы
    press.mouseY = 400.0f;   // и ниже
    press.down = true;
    press.pressed = true;
    ui.begin(press, 800, 600);
    const DragResult result = ui.dragBar(uiId("bar"), bar, 3, 10, TextColor{});
    ui.end();
    CHECK(result.value == 3u);
    CHECK_FALSE(result.changed);
    CHECK_FALSE(result.held);
}

TEST_CASE("полоса: за курсором тянется и после ухода с неё") {
    // Быстрая протяжка всегда выносит курсор за край полосы. Если
    // на этом набор срывается, точное число выставить нельзя вовсе.
    Ui ui;
    const Rect bar{200.0f, 100.0f, 300.0f, 20.0f};
    const uint32_t id = uiId("bar");

    UiInput press;
    press.mouseX = 260.0f;
    press.mouseY = 110.0f;
    press.down = true;
    press.pressed = true;
    ui.begin(press, 800, 600);
    ui.dragBar(id, bar, 0, 10, TextColor{});
    ui.end();

    UiInput drag;
    drag.mouseX = 440.0f;
    drag.mouseY = 400.0f;   // курсор ушёл ВНИЗ с полосы
    drag.down = true;
    ui.begin(drag, 800, 600);
    const DragResult result = ui.dragBar(id, bar, 2, 10, TextColor{});
    ui.end();
    CHECK(result.held);
    CHECK(result.value == 8u);
    CHECK(result.changed);
}

// ---------------------------------------------------------------------------
// Окно отряда
// ---------------------------------------------------------------------------

TEST_CASE("отряд: закрытое окно не рисуется") {
    Squad squad;
    REQUIRE(squad.game.client.ready());
    REQUIRE(squad.state.fleet != 0xFFFFFFFFu);

    squad.state.squadOpen = false;
    squad.draw();
    CHECK_FALSE(squad.hasLine("СОСТАВ И РАЗДЕЛЕНИЕ"));

    squad.state.squadOpen = true;
    squad.draw();
    CHECK(squad.hasLine("СОСТАВ И РАЗДЕЛЕНИЕ"));
    CHECK(squad.hasLine("ЧТО ДЕЛАЕТ САМ"));
    CHECK(squad.hasLine("ПРИПИСКА"));
}

TEST_CASE("отряд: в окне видно состав, стойку и приписку") {
    Squad squad;
    REQUIRE(squad.game.client.ready());
    squad.draw();

    const game::FleetView& view = squad.fleet();
    REQUIRE(view.anchor < squad.game.client.galaxy().systemCount());
    CHECK(squad.hasLine("стойка"));
    CHECK(squad.hasLine("дом: система " + std::to_string(view.anchor)));
    // Каждый класс, который в отряде есть, назван по имени.
    for (uint8_t hull = 1; hull < uint8_t(sim::Hull::Count); ++hull) {
        if (view.composition[sim::Hull(hull)] == 0) continue;
        CAPTURE(hull);
        CHECK(squad.hasLine(hullName(hull)));
    }
}

TEST_CASE("отряд: до каждой стойки можно дотянуться мышью") {
    Squad squad;
    REQUIRE(squad.game.client.ready());

    // Четыре стойки — четыре разных намерения. Именно «все четыре»:
    // ряд, в котором нажимается только выбранная, уже случался, и с виду
    // он неотличим от исправного.
    std::vector<bool> reached(size_t(sim::Stance::Count), false);
    for (int y = 0; y < squad.screenHeight; y += 3) {
        for (int x = 0; x < squad.screenWidth; x += 5) {
            const ScreenAction action = squad.click(float(x), float(y));
            if (action.kind != ActionKind::SetStance) continue;
            REQUIRE(action.slot < uint8_t(sim::Stance::Count));
            CHECK(action.value == squad.state.fleet);
            reached[action.slot] = true;
        }
    }
    for (uint8_t raw = 0; raw < uint8_t(sim::Stance::Count); ++raw) {
        CAPTURE(raw);
        CHECK(reached[raw]);
    }
}

TEST_CASE("отряд: уклонение переключается и подписано состоянием") {
    Squad squad;
    REQUIRE(squad.game.client.ready());
    REQUIRE(squad.fleet().evade == 0);

    squad.draw();
    CHECK(squad.hasLine("Принимает любой бой"));

    const ScreenAction action = squad.hunt(
        [](const ScreenAction& a) { return a.kind == ActionKind::SetEvade; });
    REQUIRE(action.kind == ActionKind::SetEvade);
    CHECK(action.value == squad.state.fleet);
    CHECK(action.slot == 1);   // выключено — предлагает включить
}

TEST_CASE("отряд: заготовки набора отдают свои номера") {
    Squad squad;
    REQUIRE(squad.game.client.ready());

    std::vector<bool> reached(3, false);
    for (int y = 0; y < squad.screenHeight; y += 3) {
        for (int x = 0; x < squad.screenWidth; x += 5) {
            const ScreenAction action = squad.click(float(x), float(y));
            if (action.kind != ActionKind::SplitPreset) continue;
            REQUIRE(action.value < 3);
            reached[action.value] = true;
        }
    }
    CHECK(reached[0]);   // сброс
    CHECK(reached[1]);   // половину
    CHECK(reached[2]);   // колонистов
}

TEST_CASE("отряд: «всё» набирает весь класс, а потом снимает его") {
    Squad squad;
    REQUIRE(squad.game.client.ready());

    // Ищем класс, которого в отряде больше одного: на единственном
    // корабле «всё» и «один» неразличимы, и проверка была бы пустой.
    uint8_t hull = 0;
    uint32_t have = 0;
    for (uint8_t k = 1; k < uint8_t(sim::Hull::Count); ++k) {
        const uint32_t count = squad.fleet().composition[sim::Hull(k)];
        if (count > 1) { hull = k; have = count; break; }
    }
    REQUIRE(hull != 0);

    const ScreenAction take = squad.hunt([&](const ScreenAction& a) {
        return a.kind == ActionKind::SplitAdjust && a.slot == hull && a.value == have;
    });
    REQUIRE(take.kind == ActionKind::SplitAdjust);

    // Набрали весь класс — та же кнопка обязана снимать набор.
    squad.state.splitTake[sim::Hull(hull)] = have;
    squad.draw();
    CHECK(squad.hasLine("снять"));
    const ScreenAction drop = squad.hunt([&](const ScreenAction& a) {
        return a.kind == ActionKind::SplitAdjust && a.slot == hull && a.value == 0;
    });
    CHECK(drop.kind == ActionKind::SplitAdjust);
}

TEST_CASE("отряд: сводка показывает обе половины и время линии") {
    Squad squad;
    REQUIRE(squad.game.client.ready());

    // Пустой набор: уходит — пусто.
    squad.draw();
    CHECK(squad.hasLine("уходит: пусто"));

    // ВРЕМЯ ЛИНИИ — ГЛАВНОЕ ЧИСЛО ЭТОГО ОКНА. Скорость флота задаёт самый
    // медленный корабль, и без неё разделение — бухгалтерия, а не решение.
    squad.state.splitTake = sim::fleetHalf(squad.fleet().composition);
    REQUIRE(sim::fleetShipCount(squad.state.splitTake) > 0);
    squad.draw();
    CHECK(squad.hasLine("остаётся"));
    CHECK(squad.hasLine("линия"));
    CHECK_FALSE(squad.hasLine("уходит: пусто"));
}

TEST_CASE("отряд: весь флот целиком выделить нельзя") {
    // Разделение, забирающее всё, оставило бы пустой отряд — то есть
    // переименование, а не разделение. Кнопка обязана быть НЕДОСТУПНА,
    // а не молча ничего не делать.
    Squad squad;
    REQUIRE(squad.game.client.ready());

    squad.state.splitTake = sim::fleetHalf(squad.fleet().composition);
    const ScreenAction half = squad.hunt(
        [](const ScreenAction& a) { return a.kind == ActionKind::SplitConfirm; });
    CHECK(half.kind == ActionKind::SplitConfirm);

    squad.state.splitTake = squad.fleet().composition;
    const ScreenAction whole = squad.hunt(
        [](const ScreenAction& a) { return a.kind == ActionKind::SplitConfirm; });
    CHECK(whole.kind == ActionKind::None);
}

TEST_CASE("отряд: приписка к системе и к планете") {
    Squad squad;
    REQUIRE(squad.game.client.ready());
    const auto planets = squad.game.client.planetsAt(squad.state.system);
    REQUIRE_FALSE(planets.empty());

    const ScreenAction toSystem = squad.hunt(
        [](const ScreenAction& a) { return a.kind == ActionKind::AnchorSystem; });
    REQUIRE(toSystem.kind == ActionKind::AnchorSystem);
    CHECK(toSystem.value == squad.state.fleet);

    const ScreenAction toPlanet = squad.hunt(
        [](const ScreenAction& a) { return a.kind == ActionKind::AnchorPlanet; });
    REQUIRE(toPlanet.kind == ActionKind::AnchorPlanet);
    CHECK(toPlanet.value == squad.state.fleet);
    CHECK(toPlanet.planet == planets[squad.state.planetIndex].id);
}

TEST_CASE("отряд: слияние появляется только когда есть с кем сливаться") {
    Squad squad;
    REQUIRE(squad.game.client.ready());

    // Один отряд в системе — раздела «СЛИЯНИЕ» нет вовсе.
    REQUIRE(squad.game.client.fleetsAt(squad.state.system).size() == 1);
    squad.draw();
    CHECK_FALSE(squad.hasLine("СЛИЯНИЕ"));

    // Отцепляем один корабль — и сосед появляется.
    uint8_t hull = 0;
    for (uint8_t k = 1; k < uint8_t(sim::Hull::Count); ++k) {
        if (squad.fleet().composition[sim::Hull(k)] > 1) { hull = k; break; }
    }
    REQUIRE(hull != 0);
    REQUIRE(squad.game.client.orderSplitFleet(squad.state.fleet, sim::Hull(hull), 1));
    squad.game.run(400);
    REQUIRE(squad.game.client.fleetsAt(squad.state.system).size() == 2);

    squad.draw();
    CHECK(squad.hasLine("СЛИЯНИЕ"));
    const ScreenAction merge = squad.hunt(
        [](const ScreenAction& a) { return a.kind == ActionKind::MergeFleet; });
    REQUIRE(merge.kind == ActionKind::MergeFleet);
    CHECK(merge.value != squad.state.fleet);
}

// ---------------------------------------------------------------------------
// Выделение группой
// ---------------------------------------------------------------------------

TEST_CASE("выделение: кнопка в строке добавляет отряд к группе") {
    Squad squad;
    REQUIRE(squad.game.client.ready());

    const ScreenAction action = squad.hunt(
        [](const ScreenAction& a) { return a.kind == ActionKind::ToggleSelect; });
    REQUIRE(action.kind == ActionKind::ToggleSelect);
    CHECK(action.value == squad.state.fleet);
}

TEST_CASE("выделение: щелчок по строке — это выбор, а не добавление") {
    // Два разных ответа в одном месте — самая дорогая ошибка списка:
    // игрок теряет группу ровно тогда, когда собирал её дольше всего.
    Squad squad;
    REQUIRE(squad.game.client.ready());

    const ScreenAction pick = squad.hunt(
        [](const ScreenAction& a) { return a.kind == ActionKind::SelectFleet; });
    REQUIRE(pick.kind == ActionKind::SelectFleet);
    CHECK(pick.value == squad.state.fleet);
}

TEST_CASE("выделение: шапка панели считает выбранные отряды") {
    Squad squad;
    REQUIRE(squad.game.client.ready());

    squad.draw();
    CHECK_FALSE(squad.hasLine("выбрано"));

    // Одного мало: «выбрано 1» — это обычный выбор, и объявлять его
    // отдельной надписью значит шуметь.
    squad.state.selection = {squad.state.fleet};
    squad.draw();
    CHECK_FALSE(squad.hasLine("выбрано"));

    squad.state.selection = {squad.state.fleet, squad.state.fleet + 1000};
    squad.draw();
    CHECK(squad.hasLine("выбрано 2"));
}

TEST_CASE("рамка: рисуется, только пока её тянут") {
    Squad squad;
    REQUIRE(squad.game.client.ready());

    squad.state.bandActive = false;
    squad.draw();
    const size_t quiet = squad.ui.frame().spriteCount();

    squad.state.bandActive = true;
    squad.state.bandX0 = 500.0f;
    squad.state.bandY0 = 200.0f;
    squad.state.bandX1 = 900.0f;
    squad.state.bandY1 = 600.0f;
    squad.draw();
    // Заливка и четыре кромки — пять прямоугольников сверх обычного кадра.
    CHECK(squad.ui.frame().spriteCount() == quiet + 5);
}
