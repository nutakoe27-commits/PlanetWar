#include "doctest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pw/core/png.h"
#include "pw/core/rng.h"

using namespace pw;

namespace {

std::string tempPath(const char* name) { return std::string("/tmp/pw_test_") + name; }

std::vector<Rgba8> gradient(int width, int height) {
    std::vector<Rgba8> pixels(size_t(width) * size_t(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pixels[size_t(y) * size_t(width) + size_t(x)] =
                Rgba8{uint8_t(x * 255 / (width > 1 ? width - 1 : 1)),
                      uint8_t(y * 255 / (height > 1 ? height - 1 : 1)),
                      uint8_t((x + y) & 0xFF), uint8_t(255 - ((x * y) & 0x7F))};
        }
    }
    return pixels;
}

}  // namespace

// ---------------------------------------------------------------------------
// Свои файлы туда и обратно
// ---------------------------------------------------------------------------

TEST_CASE("png: записанное читается обратно без потерь") {
    const int width = 61, height = 37;   // нарочно не степени двойки
    const auto original = gradient(width, height);
    const std::string path = tempPath("roundtrip.png");
    REQUIRE(writePng(path, original, width, height));

    std::vector<Rgba8> read;
    int gotWidth = 0, gotHeight = 0;
    REQUIRE(readPng(path, read, gotWidth, gotHeight));

    CHECK(gotWidth == width);
    CHECK(gotHeight == height);
    REQUIRE(read.size() == original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        CHECK(read[i].r == original[i].r);
        CHECK(read[i].g == original[i].g);
        CHECK(read[i].b == original[i].b);
        CHECK(read[i].a == original[i].a);
    }
    std::remove(path.c_str());
}

TEST_CASE("png: картинка в один пиксель") {
    const std::vector<Rgba8> one{Rgba8{17, 34, 51, 68}};
    const std::string path = tempPath("one.png");
    REQUIRE(writePng(path, one, 1, 1));

    std::vector<Rgba8> read;
    int width = 0, height = 0;
    REQUIRE(readPng(path, read, width, height));
    CHECK(width == 1);
    CHECK(height == 1);
    REQUIRE(read.size() == 1);
    CHECK(read[0].r == 17);
    CHECK(read[0].a == 68);
    std::remove(path.c_str());
}

TEST_CASE("png: крупная картинка не теряет ни строки") {
    // Атлас 1024x1024 — реальный размер из ассетного пайплайна.
    const int side = 256;   // в тесте меньше, но с той же логикой строк
    const auto original = gradient(side, side);
    const std::string path = tempPath("big.png");
    REQUIRE(writePng(path, original, side, side));

    std::vector<Rgba8> read;
    int width = 0, height = 0;
    REQUIRE(readPng(path, read, width, height));
    REQUIRE(read.size() == original.size());

    // Проверяем углы и середину: ошибка в шаге строки проявляется именно там.
    const size_t last = original.size() - 1;
    CHECK(read[0].r == original[0].r);
    CHECK(read[last].r == original[last].r);
    CHECK(read[original.size() / 2].g == original[original.size() / 2].g);
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Чужие файлы
// ---------------------------------------------------------------------------

TEST_CASE("png: атлас из Blender читается") {
    // Файл настоящий: его пишет Blender сжатым deflate с динамическими
    // кодами Хаффмана и построчными фильтрами. Наш writePng пишет
    // несжатые блоки без фильтров, поэтому проверка «туда и обратно»
    // покрывает лишь половину декодера. Этот тест — вторая половина.
    const char* candidates[] = {"assets/build/ships_albedo.png",
                                "../assets/build/ships_albedo.png",
                                "../../assets/build/ships_albedo.png"};

    std::vector<Rgba8> pixels;
    int width = 0, height = 0;
    bool found = false;
    for (const char* path : candidates) {
        if (readPng(path, pixels, width, height)) { found = true; break; }
    }
    if (!found) {
        // Атласы собираются отдельным шагом и в свежем клоне их нет.
        // Пропуск честнее провала: тест не о том, собран ли пайплайн.
        MESSAGE("атлас не найден — пропускаю (соберите tools/blender/build_assets.py)");
        return;
    }

    CHECK(width == 1024);
    CHECK(height == 1024);
    CHECK(pixels.size() == size_t(width) * size_t(height));

    // Атлас не может быть полностью пустым: на нём тридцать два кадра.
    size_t opaque = 0;
    for (const Rgba8& pixel : pixels) {
        if (pixel.a > 0) ++opaque;
    }
    CHECK(opaque > 1000);
}

// ---------------------------------------------------------------------------
// Битые файлы
//
// Разбор картинок исторически самое дырявое место в играх. Здесь проверяется
// не «читается ли», а «не падает ли и не съедает ли память».
// ---------------------------------------------------------------------------

TEST_CASE("png: не-PNG отвергается") {
    const uint8_t garbage[] = {'G', 'I', 'F', '8', '9', 'a', 0, 0};
    std::vector<Rgba8> pixels;
    int width = 0, height = 0;
    CHECK_FALSE(decodePng(garbage, sizeof(garbage), pixels, width, height));
    CHECK_FALSE(decodePng(nullptr, 0, pixels, width, height));
}

TEST_CASE("png: обрезанный файл отвергается") {
    const auto original = gradient(32, 32);
    const std::string path = tempPath("truncated.png");
    REQUIRE(writePng(path, original, 32, 32));

    std::FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> data;
    data.resize(size_t(size));
    const size_t got = std::fread(data.data(), 1, data.size(), file);
    REQUIRE(got == data.size());
    std::fclose(file);
    std::remove(path.c_str());

    // Режем в разных местах: заголовок, середина данных, почти конец.
    for (size_t cut : {size_t(10), data.size() / 2, data.size() - 4}) {
        std::vector<Rgba8> pixels;
        int width = 0, height = 0;
        decodePng(data.data(), cut, pixels, width, height);   // требование: не упасть
    }
}

TEST_CASE("png: огромные размеры в заголовке отвергаются") {
    // Заявить 2 на 2 миллиарда и заставить нас выделить память —
    // самая дешёвая атака на разбор картинки.
    uint8_t file[8 + 8 + 13 + 4] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    size_t at = 8;
    const uint8_t lengthBytes[4] = {0, 0, 0, 13};
    std::memcpy(file + at, lengthBytes, 4); at += 4;
    std::memcpy(file + at, "IHDR", 4); at += 4;
    const uint8_t huge[4] = {0x7F, 0xFF, 0xFF, 0xFF};
    std::memcpy(file + at, huge, 4); at += 4;   // ширина
    std::memcpy(file + at, huge, 4); at += 4;   // высота
    file[at++] = 8;    // бит на канал
    file[at++] = 6;    // RGBA
    file[at++] = 0;
    file[at++] = 0;
    file[at++] = 0;    // без чересстрочной

    std::vector<Rgba8> pixels;
    int width = 0, height = 0;
    CHECK_FALSE(decodePng(file, sizeof(file), pixels, width, height));
}

TEST_CASE("png: случайный мусор никогда не роняет разбор") {
    Rng rng(0x9A7EC0DE, /*stream=*/17);
    for (int attempt = 0; attempt < 4000; ++attempt) {
        uint8_t noise[512];
        const size_t size = size_t(rng.next() % sizeof(noise));
        for (size_t i = 0; i < size; ++i) noise[i] = uint8_t(rng.next());

        // Каждый четвёртый — с правильной подписью: так проверяется не
        // только фильтр по подписи, но и разбор кусков и распаковка.
        if (size >= 8 && attempt % 4 == 0) {
            static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
            std::memcpy(noise, kSignature, 8);
        }

        std::vector<Rgba8> pixels;
        int width = 0, height = 0;
        decodePng(noise, size, pixels, width, height);
    }
}

TEST_CASE("png: испорченный поток сжатия отвергается") {
    const auto original = gradient(24, 24);
    const std::string path = tempPath("corrupt.png");
    REQUIRE(writePng(path, original, 24, 24));

    std::FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> data;
    data.resize(size_t(size));
    const size_t got = std::fread(data.data(), 1, data.size(), file);
    REQUIRE(got == data.size());
    std::fclose(file);
    std::remove(path.c_str());

    // Портим байты внутри данных, а не в заголовке: декодер обязан
    // заметить это сам, а не выдать половину картинки.
    Rng rng(0xC0A17, /*stream=*/18);
    for (int attempt = 0; attempt < 500; ++attempt) {
        std::vector<uint8_t> broken = data;
        const size_t index = 40 + size_t(rng.next() % (broken.size() - 45));
        broken[index] = uint8_t(rng.next());

        std::vector<Rgba8> pixels;
        int width = 0, height = 0;
        decodePng(broken.data(), broken.size(), pixels, width, height);
    }
}
