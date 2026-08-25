// pw_rhi — запись PNG без внешних зависимостей.
//
// Нужна ровно для одного: сохранить кадр, отрисованный в безголовом режиме,
// чтобы его можно было посмотреть или сравнить с эталоном в CI.
//
// Тянуть ради этого библиотеку сжатия незачем. PNG допускает поток deflate
// из «сохранённых» блоков — то есть вообще без сжатия. Файл выходит крупнее,
// но это отладочный артефакт, а не игровой ресурс: игровые текстуры пекутся
// Blender'ом и сжимаются им же.

#include "pw/rhi/rhi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pw::rhi {
namespace {

uint32_t crc32Of(const uint8_t* data, size_t length, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < length; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

void pushBE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 24));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

void pushChunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& body) {
    pushBE32(out, uint32_t(body.size()));
    const size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), body.begin(), body.end());
    const uint32_t crc = crc32Of(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu;
    pushBE32(out, crc);
}

}  // namespace

bool writePng(const std::string& path, const std::vector<Rgba8>& pixels, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (pixels.size() < size_t(width) * size_t(height)) return false;

    // Сырые данные: перед каждой строкой байт фильтра. Ноль — «без фильтра».
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const Rgba8* row = pixels.data() + size_t(y) * size_t(width);
        for (int x = 0; x < width; ++x) {
            raw.push_back(row[x].r);
            raw.push_back(row[x].g);
            raw.push_back(row[x].b);
            raw.push_back(row[x].a);
        }
    }

    // Поток zlib из несжатых блоков.
    std::vector<uint8_t> zlib;
    zlib.push_back(0x78);  // метод сжатия deflate, окно 32 КБ
    zlib.push_back(0x01);  // контрольные биты заголовка

    constexpr size_t kBlock = 65535;
    for (size_t offset = 0; offset < raw.size(); offset += kBlock) {
        const size_t length = std::min(kBlock, raw.size() - offset);
        const bool last = (offset + length) >= raw.size();
        zlib.push_back(uint8_t(last ? 1 : 0));
        zlib.push_back(uint8_t(length & 0xFFu));
        zlib.push_back(uint8_t(length >> 8));
        zlib.push_back(uint8_t(~length & 0xFFu));
        zlib.push_back(uint8_t((~length >> 8) & 0xFFu));
        zlib.insert(zlib.end(), raw.begin() + long(offset), raw.begin() + long(offset + length));
    }

    // Контрольная сумма adler32 несжатых данных.
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    pushBE32(zlib, (b << 16) | a);

    std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    pushBE32(ihdr, uint32_t(width));
    pushBE32(ihdr, uint32_t(height));
    ihdr.push_back(8);  // бит на канал
    ihdr.push_back(6);  // цвет с альфой
    ihdr.push_back(0);  // сжатие
    ihdr.push_back(0);  // фильтрация
    ihdr.push_back(0);  // без чересстрочности
    pushChunk(file, "IHDR", ihdr);
    pushChunk(file, "IDAT", zlib);
    pushChunk(file, "IEND", {});

    std::FILE* handle = std::fopen(path.c_str(), "wb");
    if (!handle) return false;
    const size_t written = std::fwrite(file.data(), 1, file.size(), handle);
    std::fclose(handle);
    return written == file.size();
}

}  // namespace pw::rhi
