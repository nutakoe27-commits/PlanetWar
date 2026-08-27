#include "doctest.h"

#include <algorithm>
#include <fstream>
#include <vector>

#include "pw/game/server.h"
#include "pw/render/screen.h"

using namespace pw;
using namespace pw::render;

namespace {

/// Экран без окна и без видеокарты.
///
/// Ui отдаёт списки спрайтов и строк, а Screen — намерение игрока.
/// Значит проверять можно всё: и что нужная надпись оказалась на экране,
/// и что щелчок в заданную точку вернул именно то намерение.
struct Fixture {
    Ui ui;
    Screen screen;
    MessageLog messages;
    game::Client client;
    ScreenState state;

    Fixture() { screen.setMessages(&messages); }

    /// Собрать кадр без нажатий: только чтобы посмотреть, что нарисовано.
    ScreenAction draw(float mouseX = -100.0f, float mouseY = -100.0f) {
        UiInput input;
        input.mouseX = mouseX;
        input.mouseY = mouseY;
        ui.begin(input, 1600, 900);
        const ScreenAction action = screen.build(ui, client, state, /*now=*/1000);
        ui.end();
        return action;
    }

    bool hasLine(const std::string& needle) const {
        for (const std::string& line : ui.frame().lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

}  // namespace

TEST_CASE("экран: до подключения показывает, что происходит") {
    // Пустой чёрный экран человек читает как «игра сломалась». Строка
    // «подключаюсь» стоит одного вызова и снимает весь вопрос.
    Fixture fixture;
    fixture.draw();
    CHECK(fixture.hasLine("подключаюсь"));
}

TEST_CASE("интерфейс: кнопка срабатывает по отпусканию, а не по нажатию") {
    // Иначе промахнуться после нажатия нельзя, и любое случайное нажатие
    // становится необратимым — а среди кнопок здесь есть снос здания.
    Ui ui;
    const Rect box{100.0f, 100.0f, 200.0f, 40.0f};
    const uint32_t id = uiId("test");

    UiInput press;
    press.mouseX = 150.0f;
    press.mouseY = 120.0f;
    press.down = true;
    press.pressed = true;
    ui.begin(press, 800, 600);
    CHECK_FALSE(ui.button(id, box, "кнопка").clicked);
    ui.end();

    UiInput release = press;
    release.pressed = false;
    release.down = false;
    release.released = true;
    ui.begin(release, 800, 600);
    CHECK(ui.button(id, box, "кнопка").clicked);
    ui.end();
}

TEST_CASE("интерфейс: увели курсор после нажатия — щелчка нет") {
    Ui ui;
    const Rect box{100.0f, 100.0f, 200.0f, 40.0f};
    const uint32_t id = uiId("test");

    UiInput press;
    press.mouseX = 150.0f;
    press.mouseY = 120.0f;
    press.down = true;
    press.pressed = true;
    ui.begin(press, 800, 600);
    ui.button(id, box, "кнопка");
    ui.end();

    UiInput release;
    release.mouseX = 600.0f;   // ушли с кнопки
    release.mouseY = 400.0f;
    release.released = true;
    ui.begin(release, 800, 600);
    CHECK_FALSE(ui.button(id, box, "кнопка").clicked);
    ui.end();
}

TEST_CASE("интерфейс: мышь над панелью не доходит до мира") {
    // Без этого нажатие на кнопку заодно отдаёт приказ флоту в системе
    // под ней — и игрок теряет флот, нажимая «построить шахту».
    Ui ui;
    UiInput input;
    input.mouseX = 150.0f;
    input.mouseY = 120.0f;

    ui.begin(input, 800, 600);
    ui.button(uiId("test"), Rect{100.0f, 100.0f, 200.0f, 40.0f}, "кнопка");
    ui.end();
    CHECK(ui.wantsMouse());

    input.mouseX = 700.0f;
    ui.begin(input, 800, 600);
    ui.button(uiId("test"), Rect{100.0f, 100.0f, 200.0f, 40.0f}, "кнопка");
    ui.end();
    CHECK_FALSE(ui.wantsMouse());
}

TEST_CASE("интерфейс: подсказка ложится поверх всего") {
    Ui ui;
    UiInput input;
    input.mouseX = 150.0f;
    input.mouseY = 120.0f;

    ui.begin(input, 800, 600);
    ui.tooltip("это подсказка");
    ui.panel(Rect{0.0f, 0.0f, 400.0f, 300.0f});
    ui.end();

    // Подсказка объявлена ПЕРВОЙ, а нарисована последней: иначе панель,
    // объявленная позже, закрыла бы её.
    REQUIRE_FALSE(ui.frame().lines.empty());
    CHECK(ui.frame().lines.back() == "это подсказка");
}

TEST_CASE("интерфейс: подсказка не вылезает за край экрана") {
    Ui ui;
    UiInput input;
    input.mouseX = 795.0f;
    input.mouseY = 595.0f;

    ui.begin(input, 800, 600);
    ui.tooltip("подсказка у самого угла");
    ui.end();

    // Подсказка, наполовину вылезшая за монитор, — это подсказка,
    // которую не прочитать.
    for (const UiBatch& batch : ui.frame().batches) {
        for (const rhi::SpriteInstance& sprite : batch.sprites) {
            CHECK(sprite.x - sprite.halfWidth >= -1.0f);
            CHECK(sprite.y - sprite.halfHeight >= -1.0f);
            CHECK(sprite.x + sprite.halfWidth <= 801.0f);
            CHECK(sprite.y + sprite.halfHeight <= 601.0f);
        }
    }
}

TEST_CASE("интерфейс: рамка режется на девять частей") {
    // Углы не тянутся, края тянутся вдоль одной оси. Без этого фаска
    // растягивается вместе с панелью и превращается в размытое пятно.
    UiAtlas atlas;
    // Атласа в тесте нет, поэтому проверяем вырожденный случай: без
    // растяжки панель обязана остаться одним прямоугольником.
    Ui ui;
    UiInput input;
    ui.begin(input, 800, 600);
    ui.panel(Rect{10.0f, 10.0f, 300.0f, 200.0f});
    ui.end();
    CHECK(ui.frame().spriteCount() == 1);
}

TEST_CASE("журнал: сообщения гаснут и не копятся без предела") {
    MessageLog log;
    for (int i = 0; i < 20; ++i) {
        log.add("сообщение " + std::to_string(i), TextColor{}, /*now=*/0);
    }
    CHECK(log.entries().size() == MessageLog::kMaxVisible);

    log.update(MessageLog::kLifetime + 1);
    CHECK(log.entries().empty());
}

TEST_CASE("справочник: у каждого здания есть имя, значок и польза") {
    // Кнопка без значка — это кнопка, которую игрок читает, а не узнаёт.
    for (uint8_t building = 1; building < uint8_t(sim::Building::Count); ++building) {
        CAPTURE(int(building));
        CHECK(std::string(buildingName(building)) != "?");
        CHECK(buildingIcon(building) != nullptr);
        CHECK(std::string(buildingHint(building)).size() > 0);
    }
    for (uint8_t hull = 1; hull < uint8_t(sim::Hull::Count); ++hull) {
        CAPTURE(int(hull));
        CHECK(std::string(hullName(hull)) != "?");
        CHECK(hullIcon(hull) != nullptr);
    }
}

TEST_CASE("справочник: у каждой новости есть текст") {
    // Новость без текста — пустая строка в журнале: игрок видит, что
    // что-то случилось, и не может узнать что.
    for (uint8_t kind = 1; kind < uint8_t(game::NoticeKind::Count); ++kind) {
        CAPTURE(int(kind));
        CHECK_FALSE(noticeText(game::NoticeKind(kind)).empty());
    }
}

// ---------------------------------------------------------------------------
// Живой экран
// ---------------------------------------------------------------------------
//
// Всё, что ниже, проверяется на НАСТОЯЩЕМ клиенте с настоящей галактикой.
// Пустой клиент показывает только заставку «подключаюсь», и на нём нельзя
// проверить ни одной ошибки раскладки — а ровно они и оказались самыми
// дорогими: панель, у которой высота посчитана отдельно от содержимого,
// накрывает кнопкой последнюю строку списка, и это видно только глазами.

namespace {

/// Где лежит собранный атлас интерфейса.
///
/// Тест запускают и из корня репозитория, и из каталога сборки. Искать
/// в обоих местах дешевле, чем однажды получить «тест зелёный, потому
/// что он ничего не проверил».
std::string findUiManifest() {
    const char* candidates[] = {"assets/build/ui.json", "../assets/build/ui.json",
                                "../../assets/build/ui.json"};
    for (const char* path : candidates) {
        std::ifstream probe(path);
        if (probe) return path;
    }
    return {};
}

/// Сервер и клиент в одной памяти, без сокетов и без ожидания.
///
/// Свой, а не общий с pw_game: тесты интерфейса не имеют права зависеть
/// от файлов тестов другого слоя, иначе правка там ломает сборку здесь.
class LiveGame {
public:
    explicit LiveGame(uint32_t systems = 40) {
        game::ServerConfig config;
        config.galaxy.seed = 0xC0FFEE;
        config.galaxy.systemCount = systems;
        config.maxPlayers = 4;
        config.speed = 1;
        server_.start(config);
        client.connect(address_, "Михаил", now_);
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
    net::Address address_ = net::Address::loopback(20001);
    int64_t now_ = 0;
};

/// Экран, по которому можно щёлкать.
struct LiveFixture {
    LiveGame game;
    Ui ui;
    Screen screen;
    MessageLog messages;
    ScreenState state;
    int screenWidth = 1600;
    int screenHeight = 900;
    /// Экранное время. Двигается вручную: от него зависит и угасание
    /// сообщений, и то, держится ли ещё взведённая кнопка выхода.
    int64_t now = 1000;

    LiveFixture() {
        screen.setMessages(&messages);
        state.system = game.client.capital();
    }

    ScreenAction frame(float mouseX, float mouseY, bool down, bool pressed,
                       bool released) {
        UiInput input;
        input.mouseX = mouseX;
        input.mouseY = mouseY;
        input.down = down;
        input.pressed = pressed;
        input.released = released;
        ui.begin(input, screenWidth, screenHeight);
        const ScreenAction action = screen.build(ui, game.client, state, now);
        ui.end();
        return action;
    }

    /// Полный щелчок: нажали и отпустили, не сходя с места.
    ScreenAction click(float x, float y) {
        frame(x, y, /*down=*/true, /*pressed=*/true, /*released=*/false);
        return frame(x, y, /*down=*/false, /*pressed=*/false, /*released=*/true);
    }

    bool hasLine(const std::string& needle) const {
        for (const std::string& line : ui.frame().lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    bool hasPalette() const { return hasLine("Что построить"); }
};

}  // namespace

TEST_CASE("экран: последняя планета в списке не перекрыта кнопкой") {
    // ИМЕННО ЭТА ошибка и была в первой сборке: высоту панели считали
    // отдельно от её содержимого, и кнопка «Открыть систему» легла поверх
    // последней строки. Список из четырёх планет работал как список
    // из трёх, и понять это можно было только глазами на снимке.
    LiveFixture fixture;
    REQUIRE(fixture.game.client.ready());

    const size_t planets = fixture.game.client.planetsAt(fixture.state.system).size();
    REQUIRE(planets >= 2);

    // Проходим левый столбец сверху вниз и смотрим, что отвечает каждая
    // точка. Координаты панели тест не знает и знать не должен: он
    // проверяет ПОВЕДЕНИЕ, а не числа раскладки.
    std::vector<int> planetHit(planets, -1);
    int enterFrom = -1;
    for (int y = 40; y < 700; ++y) {
        const ScreenAction action = fixture.click(90.0f, float(y));
        if (action.kind == ActionKind::SelectPlanet && action.value < planets) {
            planetHit[action.value] = y;
        } else if (action.kind == ActionKind::EnterSystem && enterFrom < 0) {
            enterFrom = y;
        }
    }

    // Каждая планета кликабельна — включая последнюю.
    for (size_t index = 0; index < planets; ++index) {
        CAPTURE(index);
        CHECK(planetHit[index] >= 0);
    }
    // И кнопка входа лежит НИЖЕ последней планеты, а не поверх неё.
    REQUIRE(enterFrom >= 0);
    CHECK(planetHit[planets - 1] < enterFrom);
}

TEST_CASE("экран: пустой слот открывает палитру, палитра заказывает стройку") {
    // Путь новичка целиком: выбрать планету, выбрать слот, нажать здание.
    // Ни одной клавиши.
    LiveFixture fixture;
    REQUIRE(fixture.game.client.ready());

    const auto planets = fixture.game.client.planetsAt(fixture.state.system);
    REQUIRE_FALSE(planets.empty());
    const uint8_t mine = uint8_t(fixture.game.client.empire() & 0xFFu);
    REQUIRE(planets[0].owner == mine);

    // До выбора слота палитры нет.
    fixture.frame(-100.0f, -100.0f, false, false, false);
    CHECK_FALSE(fixture.hasPalette());

    fixture.state.slot = 0;
    fixture.frame(-100.0f, -100.0f, false, false, false);
    REQUIRE(fixture.hasPalette());

    // Ищем кнопку шахты и нажимаем её.
    bool ordered = false;
    for (int y = 40; y < 880 && !ordered; ++y) {
        for (int x = 20; x < 380; x += 6) {
            const ScreenAction action = fixture.click(float(x), float(y));
            if (action.kind != ActionKind::Build) continue;
            CHECK(action.slot == 0);
            CHECK(action.planet == planets[0].id);
            CHECK(action.value >= 1);
            ordered = true;
            break;
        }
    }
    CHECK(ordered);
}

TEST_CASE("экран: ничего не вылезает за пределы окна") {
    // Общая страховка от того же класса ошибок: что бы ни было выбрано,
    // ни один спрайт не уходит за край экрана. Панель, посчитавшая свою
    // высоту неверно, почти всегда сначала вылезает вниз.
    LiveFixture fixture;
    REQUIRE(fixture.game.client.ready());

    for (int slot = -1; slot < 3; ++slot) {
        for (int inSystem = 0; inSystem < 2; ++inSystem) {
            fixture.state.slot = slot < 0 ? uint8_t(0xFF) : uint8_t(slot);
            fixture.state.inSystem = inSystem != 0;
            // Курсор в углу: подсказка тоже обязана уложиться в экран.
            fixture.frame(float(fixture.screenWidth) - 4.0f,
                          float(fixture.screenHeight) - 4.0f, false, false, false);

            for (const UiBatch& batch : fixture.ui.frame().batches) {
                for (const rhi::SpriteInstance& sprite : batch.sprites) {
                    CAPTURE(slot);
                    CAPTURE(inSystem);
                    CHECK(sprite.x - sprite.halfWidth >= -1.0f);
                    CHECK(sprite.y - sprite.halfHeight >= -1.0f);
                    CHECK(sprite.x + sprite.halfWidth <= float(fixture.screenWidth) + 1.0f);
                    CHECK(sprite.y + sprite.halfHeight <=
                          float(fixture.screenHeight) + 1.0f);
                }
            }
        }
    }
}

TEST_CASE("экран: выход требует подтверждения") {
    // Кнопка выхода стоит в одной полосе с «показать всё». Выход
    // с одного нажатия — это партия, потерянная из-за дрогнувшей руки.
    LiveFixture fixture;
    REQUIRE(fixture.game.client.ready());

    const float y = float(fixture.screenHeight) - 20.0f;

    // Ищем кнопку: выход даёт только ВТОРОЙ щелчок подряд.
    float found = -1.0f;
    for (int x = fixture.screenWidth - 300; x < fixture.screenWidth - 4; x += 4) {
        fixture.now += 10000;                    // взвод спал
        fixture.click(float(x), y);              // первый щелчок взводит
        if (fixture.click(float(x), y).kind == ActionKind::Quit) {
            found = float(x);
            break;
        }
    }
    REQUIRE(found >= 0.0f);

    // А один щелчок после сброса выхода не даёт.
    fixture.now += 10000;
    CHECK(fixture.click(found, y).kind != ActionKind::Quit);

    // Взвод спадает сам: отошли на минуту — кнопка снова обычная.
    fixture.now += 60000;
    CHECK(fixture.click(found, y).kind != ActionKind::Quit);
}

TEST_CASE("журнал: повтор не плодит строки, а считает") {
    // Захват системы из четырёх планет иначе выдаёт четыре одинаковых
    // сообщения и вытесняет из журнала всё остальное.
    MessageLog log;
    const TextColor white;
    for (int i = 0; i < 4; ++i) log.add("планета взята", white, 100, 7, nullptr);
    REQUIRE(log.entries().size() == 1);
    CHECK(log.entries().front().count == 4);

    // Другая система — другая новость.
    log.add("планета взята", white, 100, 8, nullptr);
    CHECK(log.entries().size() == 2);

    // Повтор оживляет строку: она не гаснет, пока новости идут, —
    // а соседка того же возраста, которую не повторяли, гаснет.
    log.add("планета взята", white, 5000, 8, nullptr);
    CHECK(log.entries().back().bornAt == 5000);
    log.update(5000 + MessageLog::kLifetime - 1);
    REQUIRE(log.entries().size() == 1);
    CHECK(log.entries().front().system == 8);
}

TEST_CASE("атлас интерфейса: экран не просит того, чего в нём нет") {
    // Самая тихая поломка всего интерфейса: спрайт переименовали
    // в Blender, кнопка потеряла подложку — и об этом узнаёт игрок,
    // а не сборка. Здесь экран прогоняется во всех состояниях
    // на НАСТОЯЩЕМ атласе, и любой промах виден по имени.
    const std::string manifest = findUiManifest();
    if (manifest.empty()) {
        MESSAGE("атлас интерфейса не собран — пропускаю");
        return;
    }

    UiAtlas atlas;
    REQUIRE_MESSAGE(atlas.load(manifest), atlas.error());

    LiveFixture fixture;
    REQUIRE(fixture.game.client.ready());
    fixture.ui.setAtlas(&atlas);

    const auto planets = fixture.game.client.planetsAt(fixture.state.system);
    REQUIRE_FALSE(planets.empty());

    std::vector<std::string> missing;
    auto collect = [&]() {
        for (const std::string& name : fixture.ui.missing()) {
            if (std::find(missing.begin(), missing.end(), name) == missing.end()) {
                missing.push_back(name);
            }
        }
    };

    // Все новости — ради значков в журнале.
    for (uint8_t kind = 1; kind < uint8_t(game::NoticeKind::Count); ++kind) {
        fixture.messages.add(noticeText(game::NoticeKind(kind)), TextColor{},
                             fixture.now, 3, noticeIcon(game::NoticeKind(kind)));
    }

    // Все сочетания, которые вообще бывают на экране: карта и система,
    // слот выбран и нет, приказ взведён и нет, курсор на панели и мимо.
    for (int inSystem = 0; inSystem < 2; ++inSystem) {
        for (int slot = -1; slot < 3; ++slot) {
            for (int armed = 0; armed < 2; ++armed) {
                fixture.state.inSystem = inSystem != 0;
                fixture.state.slot = slot < 0 ? uint8_t(0xFF) : uint8_t(slot);
                fixture.state.awaitingMoveTarget = armed != 0;
                fixture.frame(90.0f, 120.0f, false, false, false);
                collect();
                fixture.frame(-100.0f, -100.0f, false, false, false);
                collect();
            }
        }
    }

    CAPTURE(missing.empty() ? std::string("—") : missing.front());
    CHECK(missing.empty());
}

TEST_CASE("атлас интерфейса: пропажа спрайта не молчит") {
    // Проверка самой проверки. Список промахов, который никогда
    // не наполняется, — это зелёный тест, не проверяющий ничего.
    const std::string manifest = findUiManifest();
    if (manifest.empty()) {
        MESSAGE("атлас интерфейса не собран — пропускаю");
        return;
    }
    UiAtlas atlas;
    REQUIRE_MESSAGE(atlas.load(manifest), atlas.error());

    Ui ui;
    ui.setAtlas(&atlas);
    ui.begin(UiInput{}, 800, 600);
    ui.panel(Rect{0.0f, 0.0f, 100.0f, 40.0f}, "такого спрайта нет");
    ui.icon(Rect{0.0f, 0.0f, 20.0f, 20.0f}, "и такого тоже нет");
    ui.panel(Rect{0.0f, 0.0f, 100.0f, 40.0f}, "panel");
    ui.end();

    REQUIRE(ui.missing().size() == 2);
    CHECK(ui.missing()[0] == "такого спрайта нет");
    CHECK(ui.missing()[1] == "и такого тоже нет");

    // Следующий кадр начинается с чистого списка.
    ui.begin(UiInput{}, 800, 600);
    ui.panel(Rect{0.0f, 0.0f, 100.0f, 40.0f}, "panel");
    ui.end();
    CHECK(ui.missing().empty());
}
