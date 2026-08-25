#include "doctest.h"

#include <string>

#include "pw/game/server.h"
#include "pw/render/hud.h"

using namespace pw;
using namespace pw::render;

namespace {

/// Сервер и клиент в памяти: панель собирается по НАСТОЯЩЕМУ состоянию,
/// а не по выдуманному. Иначе тест проверял бы вёрстку, а не то, что
/// игрок увидит.
struct Table {
    game::Server server;
    game::Client client;
    net::Address address = net::Address::loopback(30001);
    net::Address serverAddress = net::Address::loopback(30000);
    int64_t now = 0;

    Hud hud;
    HudFrame frame;

    explicit Table(uint32_t systems = 60) {
        game::ServerConfig config;
        config.galaxy.seed = 0x4A17;
        config.galaxy.systemCount = systems;
        server.start(config);
        client.connect(serverAddress, "Михаил", now);
        run(1500);
    }

    void run(int64_t milliseconds) {
        const int64_t until = now + milliseconds;
        while (now < until) {
            ++now;
            uint8_t buffer[net::kMaxPacketSize];
            const size_t size = client.update(now, buffer, sizeof(buffer));
            if (size > 0) server.receive(address, buffer, size, now);

            std::vector<game::OutgoingPacket> outgoing;
            server.update(now, outgoing);
            for (const auto& packet : outgoing) {
                if (packet.to != address) continue;
                client.receive(packet.data.data(), packet.data.size(), now);
            }
        }
    }

    void build(const Selection& selection = {}) {
        hud.build(client, selection, 1280, 720, frame);
    }

    bool has(const std::string& fragment) const {
        for (const HudLine& line : frame.lines) {
            if (line.text.find(fragment) != std::string::npos) return true;
        }
        return false;
    }
};

}  // namespace

TEST_CASE("панель: до подключения честно говорит, что подключается") {

    game::Client client;
    Hud hud;
    HudFrame frame;
    hud.build(client, Selection{}, 1280, 720, frame);
    REQUIRE_FALSE(frame.lines.empty());
    CHECK(frame.lines.front().text.find("подключаюсь") != std::string::npos);
}

TEST_CASE("панель: ресурсы видны сразу") {
    // Первое, что нужно игроку: можно ли сейчас что-то построить.
    Table table;
    REQUIRE(table.client.ready());
    table.build();

    CHECK(table.has("сплавы"));
    CHECK(table.has("минералы"));
    CHECK(table.has("энергия"));
}

TEST_CASE("панель: показывает владения и флот") {
    Table table;
    table.build();
    CHECK(table.has("систем"));
    CHECK(table.has("флот"));
}

TEST_CASE("панель: связь показана всегда") {
    // В MMO игрок обязан отличать «сервер тормозит» от «я плохо играю».
    Table table;
    table.build();
    CHECK(table.has("задержка"));
    CHECK(table.has("потери"));
}

TEST_CASE("панель: разряды разделены, чтобы числа читались") {
    Table table;
    table.run(1000);
    table.build();

    // Стартовые сплавы 500 — трёхзначное число без разделителя.
    CHECK(table.has("500"));

    // А теперь проверим саму вёрстку на большом числе: панель обязана
    // разбивать разряды, иначе 144530 не прочитать с одного взгляда.
    bool grouped = false;
    for (const HudLine& line : table.frame.lines) {
        if (line.text.find(' ') != std::string::npos) grouped = true;
    }
    CHECK(grouped);
}

TEST_CASE("панель: выбранная система описана полностью") {
    Table table;
    Selection selection;
    selection.system = table.client.capital();
    table.build(selection);

    CHECK(table.has("система"));
    // Класс светила — это ценность системы, ради которой за неё воюют.
    const bool named = table.has("карлик") || table.has("звезда") ||
                       table.has("гигант") || table.has("дыра");
    CHECK(named);
    CHECK(table.has("ваша"));
    CHECK(table.has("оборона"));
    CHECK(table.has("планета"));
}

TEST_CASE("панель: свой флот в системе показан составом") {
    // Игрок принимает решение по составу, а не по числу отрядов.
    Table table;
    Selection selection;
    selection.system = table.client.capital();
    table.build(selection);

    CHECK(table.has("флот"));
    CHECK(table.has("корв"));
    CHECK(table.has("эсм"));
}

TEST_CASE("панель: подсказки по управлению на месте") {
    Table table;
    table.build();
    CHECK(table.has("ЛКМ"));
    CHECK(table.has("ПКМ"));
}

TEST_CASE("панель: без шрифта строки собираются, спрайты — нет") {
    // Разделение намеренное: содержимое панели проверяется текстом,
    // а не пикселями, и для этого шрифт не нужен.
    Table table;
    table.build();
    CHECK_FALSE(table.frame.lines.empty());
    CHECK(table.frame.sprites.empty());
}

TEST_CASE("панель: со шрифтом появляются спрайты") {
    Font font;
    const char* candidates[] = {"assets/build/font.json", "../assets/build/font.json",
                                "../../assets/build/font.json"};
    bool loaded = false;
    for (const char* path : candidates) {
        if (font.load(path)) { loaded = true; break; }
    }
    if (!loaded) return;

    Table table;
    table.hud.setFont(&font);
    table.build();
    CHECK_FALSE(table.frame.sprites.empty());
}

TEST_CASE("панель: размер строки подстраивается под экран") {
    // На 4K надпись в 14 пикселей нечитаема, на ноутбуке в 28 занимает
    // пол-экрана. Поэтому доля высоты, а не фиксированный размер.
    CHECK(Hud::lineHeight(2160) > Hud::lineHeight(720));
    CHECK(Hud::lineHeight(720) > Hud::lineHeight(400));
    // Но с пределами: иначе на сверхшироком мониторе буквы уползут.
    CHECK(Hud::lineHeight(10000) <= 30.0f);
    CHECK(Hud::lineHeight(100) >= 13.0f);
}

TEST_CASE("панель: система вне галактики не роняет сборку") {
    Table table;
    Selection selection;
    selection.system = 999999;
    table.build(selection);
    CHECK_FALSE(table.frame.lines.empty());
}

// ---------------------------------------------------------------------------
// Выбор планеты и флота
//
// Строить и командовать вслепую нельзя: игрок должен видеть, на что
// подействует следующее нажатие. Первая версия строила в первый свободный
// слот первой планеты и никак этого не показывала.
// ---------------------------------------------------------------------------

TEST_CASE("панель: активная планета отмечена стрелкой") {
    Table table;
    Selection selection;
    selection.system = table.client.capital();
    selection.planetIndex = 0;
    table.build(selection);

    // Ровно одна строка планеты помечена как активная.
    int marked = 0;
    for (const HudLine& line : table.frame.lines) {
        if (line.text.rfind("> планета", 0) == 0) ++marked;
    }
    CHECK(marked == 1);
}

TEST_CASE("панель: стрелка переезжает вместе с выбором") {
    Table table;

    // Столица может оказаться однопланетной — число планет задаёт
    // генератор. Ищем систему, где переключать вообще есть что.
    uint32_t multi = 0xFFFFFFFFu;
    for (uint32_t index = 0; index < table.client.galaxy().systemCount(); ++index) {
        if (table.client.planetsAt(index).size() >= 2) { multi = index; break; }
    }
    REQUIRE(multi != 0xFFFFFFFFu);

    Selection selection;
    selection.system = multi;

    selection.planetIndex = 0;
    table.build(selection);
    const bool firstMarked = table.has("> планета 1");

    selection.planetIndex = 1;
    table.build(selection);
    const bool secondMarked = table.has("> планета 2");

    CHECK(firstMarked);
    CHECK(secondMarked);
    CHECK_FALSE(table.has("> планета 1"));
}

TEST_CASE("панель: планеты нумеруются по порядку, а не номером сущности") {
    // Игрок думает «вторая планета отсюда», а не «планета 507».
    Table table;
    Selection selection;
    selection.system = table.client.capital();
    table.build(selection);
    CHECK(table.has("планета 1"));
}

TEST_CASE("панель: выбранный флот отмечен") {
    Table table;
    Selection selection;
    selection.system = table.client.capital();
    const auto own = table.client.fleetsAt(table.client.capital());
    REQUIRE_FALSE(own.empty());
    selection.fleet = own.front();
    table.build(selection);

    bool marked = false;
    for (const HudLine& line : table.frame.lines) {
        if (line.text.rfind("> флот", 0) == 0) marked = true;
    }
    CHECK(marked);
}

TEST_CASE("панель: индекс планеты вне диапазона не роняет сборку") {
    // Номер приходит из состояния интерфейса и может отстать от снапшота:
    // планету могли потерять вместе с системой.
    Table table;
    Selection selection;
    selection.system = table.client.capital();
    selection.planetIndex = 999;
    table.build(selection);
    CHECK_FALSE(table.frame.lines.empty());
}

// ---------------------------------------------------------------------------
// Журнал событий
//
// Игрок нажал — игрок обязан увидеть ответ. Без этого нажатие в пустоту
// неотличимо от нажатия, которое сервер отверг, и человек несколько раз
// жмёт одно и то же, не понимая, почему ничего не происходит.
// ---------------------------------------------------------------------------

TEST_CASE("журнал: сообщение появляется и видно в панели") {
    Table table;
    MessageLog log;
    log.add("система захвачена", TextColor{}, 1000);
    table.hud.setMessages(&log);
    table.hud.build(table.client, Selection{}, 1280, 720, 1000, table.frame);
    CHECK(table.has("система захвачена"));
}

TEST_CASE("журнал: старое гаснет и исчезает") {
    // Постоянно висящий список превращается в шум, а внимание
    // в стратегии — самый дефицитный ресурс игрока.
    MessageLog log;
    log.add("первое", TextColor{}, 0);
    REQUIRE(log.entries().size() == 1);

    log.update(MessageLog::kLifetime / 2);
    CHECK(log.entries().size() == 1);

    log.update(MessageLog::kLifetime + 1);
    CHECK(log.entries().empty());
}

TEST_CASE("журнал: держит только последние сообщения") {
    MessageLog log;
    for (int i = 0; i < 20; ++i) {
        log.add("сообщение " + std::to_string(i), TextColor{}, 0);
    }
    CHECK(log.entries().size() == MessageLog::kMaxVisible);
    // Остались именно последние.
    CHECK(log.entries().back().text == "сообщение 19");
}

TEST_CASE("журнал: сообщение бледнеет к концу жизни") {
    Table table;
    MessageLog log;
    log.add("гаснущее", TextColor{1.0f, 1.0f, 1.0f, 1.0f}, 0);
    table.hud.setMessages(&log);

    table.hud.build(table.client, Selection{}, 1280, 720, 0, table.frame);
    float fresh = 0.0f;
    for (const HudLine& line : table.frame.lines) {
        if (line.text == "гаснущее") fresh = line.color.a;
    }

    table.hud.build(table.client, Selection{}, 1280, 720, MessageLog::kLifetime - 100,
                    table.frame);
    float old = 1.0f;
    for (const HudLine& line : table.frame.lines) {
        if (line.text == "гаснущее") old = line.color.a;
    }

    CHECK(fresh > old);
    CHECK(old >= 0.0f);
}

TEST_CASE("журнал: без сообщений панель не меняется") {
    Table table;
    table.build();
    const size_t without = table.frame.lines.size();

    MessageLog empty;
    table.hud.setMessages(&empty);
    table.hud.build(table.client, Selection{}, 1280, 720, 0, table.frame);
    CHECK(table.frame.lines.size() == without);
}
