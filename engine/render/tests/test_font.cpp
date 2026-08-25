#include "doctest.h"

#include <string>

#include "pw/core/rng.h"
#include "pw/render/font.h"

using namespace pw;
using namespace pw::render;

namespace {

bool loadFont(Font& font) {
    const char* candidates[] = {"assets/build/font.json", "../assets/build/font.json",
                                "../../assets/build/font.json"};
    for (const char* path : candidates) {
        if (font.load(path)) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// UTF-8
//
// Строки интерфейса русские, а полагаться на локаль нельзя: она разная
// на разных машинах, и надписи расползались бы у части игроков.
// ---------------------------------------------------------------------------

TEST_CASE("utf8: латиница разбирается") {
    const auto codes = decodeUtf8("Hello");
    REQUIRE(codes.size() == 5);
    CHECK(codes[0] == 'H');
    CHECK(codes[4] == 'o');
}

TEST_CASE("utf8: кириллица разбирается") {
    const auto codes = decodeUtf8("Флот");
    REQUIRE(codes.size() == 4);
    CHECK(codes[0] == 0x0424);   // Ф
    CHECK(codes[1] == 0x043B);   // л
    CHECK(codes[2] == 0x043E);   // о
    CHECK(codes[3] == 0x0442);   // т
}

TEST_CASE("utf8: смешанная строка сохраняет порядок") {
    const auto codes = decodeUtf8("сплавы 144 530");
    CHECK(codes.size() == 14);
    CHECK(codes[6] == ' ');
    CHECK(codes[7] == '1');
}

TEST_CASE("utf8: битая последовательность не роняет разбор") {
    // Строка может прийти из сети — например, имя игрока.
    const char broken[] = {char(0xD0), char(0x41), char(0xFF), char(0xC3), 0};
    const auto codes = decodeUtf8(broken);
    CHECK(codes.size() <= 4);   // требование одно: не упасть и не зациклиться
}

TEST_CASE("utf8: случайный мусор не роняет разбор") {
    Rng rng(0x11F8, /*stream=*/51);
    for (int attempt = 0; attempt < 5000; ++attempt) {
        std::string noise;
        const size_t size = size_t(rng.next() % 64);
        for (size_t i = 0; i < size; ++i) noise.push_back(char(rng.next()));
        decodeUtf8(noise);
    }
}

// ---------------------------------------------------------------------------
// Шрифт
// ---------------------------------------------------------------------------

TEST_CASE("шрифт: описание и текстура читаются") {
    Font font;
    if (!loadFont(font)) {
        MESSAGE("шрифт не собран — пропускаю (tools/blender/build_assets.py)");
        return;
    }
    CHECK(font.valid());
    CHECK(font.textureWidth() > 0);
    CHECK(font.textureHeight() > 0);
    CHECK(font.pixels().size() ==
          size_t(font.textureWidth()) * size_t(font.textureHeight()));
}

TEST_CASE("шрифт: у каждой русской буквы есть глиф") {
    // Без этого часть надписей молча превращалась бы в пробелы, и игрок
    // не увидел бы половину интерфейса.
    Font font;
    if (!loadFont(font)) return;

    const std::string alphabet =
        "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдеёжзийклмнопрстуфхцчшщъыьэюя";
    std::vector<rhi::SpriteInstance> sprites;
    font.layout(alphabet, 0.0f, 0.0f, 16.0f, TextColor{}, sprites);

    // Каждая буква дала спрайт — значит ни одна не потерялась.
    CHECK(sprites.size() == decodeUtf8(alphabet).size());
}

TEST_CASE("шрифт: цифры и знаки на месте") {
    Font font;
    if (!loadFont(font)) return;

    const std::string text = "0123456789 .,:;%-+()·×—";
    std::vector<rhi::SpriteInstance> sprites;
    font.layout(text, 0.0f, 0.0f, 16.0f, TextColor{}, sprites);
    CHECK(sprites.size() == decodeUtf8(text).size());
}

TEST_CASE("шрифт: глифы не перекрываются в атласе") {
    Font font;
    if (!loadFont(font)) return;

    std::vector<rhi::SpriteInstance> a, b;
    font.layout("А", 0.0f, 0.0f, 16.0f, TextColor{}, a);
    font.layout("Б", 0.0f, 0.0f, 16.0f, TextColor{}, b);
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);
    // Разные буквы берут разные куски текстуры.
    CHECK((a[0].u0 != b[0].u0 || a[0].v0 != b[0].v0));
}

TEST_CASE("шрифт: строка растёт вправо ровным шагом") {
    // Шрифт моноширинный: на этом держится и вёрстка панелей, и то,
    // что счётчики ресурсов не дёргаются при смене цифры.
    Font font;
    if (!loadFont(font)) return;

    std::vector<rhi::SpriteInstance> sprites;
    font.layout("ААААА", 100.0f, 50.0f, 20.0f, TextColor{}, sprites);
    REQUIRE(sprites.size() == 5);

    const float step = sprites[1].x - sprites[0].x;
    CHECK(step > 0.0f);
    for (size_t i = 2; i < sprites.size(); ++i) {
        CHECK(sprites[i].x - sprites[i - 1].x == doctest::Approx(step));
        CHECK(sprites[i].y == doctest::Approx(sprites[0].y));
    }
    CHECK(font.width("ААААА", 20.0f) == doctest::Approx(step * 5.0f));
}

TEST_CASE("шрифт: перевод строки опускает курсор и возвращает влево") {
    Font font;
    if (!loadFont(font)) return;

    std::vector<rhi::SpriteInstance> sprites;
    font.layout("АБ\nВГ", 10.0f, 20.0f, 16.0f, TextColor{}, sprites);
    REQUIRE(sprites.size() == 4);

    CHECK(sprites[2].x == doctest::Approx(sprites[0].x));
    CHECK(sprites[2].y > sprites[0].y);
}

TEST_CASE("шрифт: неизвестный символ занимает место, но не рисуется") {
    // Строка не должна съезжать из-за одной буквы, которой нет в наборе.
    Font font;
    if (!loadFont(font)) return;

    std::vector<rhi::SpriteInstance> known, mixed;
    font.layout("АБВ", 0.0f, 0.0f, 16.0f, TextColor{}, known);
    font.layout("А中В", 0.0f, 0.0f, 16.0f, TextColor{}, mixed);

    REQUIRE(known.size() == 3);
    REQUIRE(mixed.size() == 2);   // иероглиф не нарисован
    // Но третья буква стоит там же, где стояла бы.
    CHECK(mixed[1].x == doctest::Approx(known[2].x));
}

TEST_CASE("шрифт: цвет надписи задаётся тоном") {
    Font font;
    if (!loadFont(font)) return;

    std::vector<rhi::SpriteInstance> sprites;
    font.layout("тест", 0.0f, 0.0f, 16.0f, TextColor{0.9f, 0.2f, 0.1f, 0.8f}, sprites);
    REQUIRE_FALSE(sprites.empty());
    CHECK(sprites[0].r == doctest::Approx(0.9f));
    CHECK(sprites[0].g == doctest::Approx(0.2f));
    CHECK(sprites[0].a == doctest::Approx(0.8f));
}

TEST_CASE("шрифт: отсутствующий файл даёт внятную ошибку") {
    Font font;
    CHECK_FALSE(font.load("такого/файла/нет.json"));
    CHECK_FALSE(font.error().empty());
    CHECK_FALSE(font.valid());

    // И раскладка на незагруженном шрифте ничего не рисует, а не падает.
    std::vector<rhi::SpriteInstance> sprites;
    font.layout("тест", 0.0f, 0.0f, 16.0f, TextColor{}, sprites);
    CHECK(sprites.empty());
}
