// pw_core — чтение PNG. Пояснения — в pw/core/png.h.
//
// Внутри две независимые части:
//
//   1. INFLATE (RFC 1951) — распаковка потока deflate. Кода немного, но
//      он весь про корректность на битых данных: поток приходит из файла,
//      то есть от кого угодно, и не имеет права ни выйти за буфер, ни
//      зациклиться, ни съесть память по заявленному в заголовке размеру.
//
//   2. Разбор PNG — контейнер, снятие построчных фильтров и приведение
//      к RGBA8.
//
// Поддерживается только то, что производит наш пайплайн: 8 бит на канал,
// без чересстрочной развёртки. Экзотика не читается наполовину —
// она отвергается.

#include "pw/core/png.h"

#include <cstdio>
#include <cstring>

namespace pw {
namespace {

// ---------------------------------------------------------------------------
// Битовый поток
// ---------------------------------------------------------------------------

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    /// Прочитать `count` бит, младшими вперёд. При выходе за конец — флаг.
    uint32_t bits(int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            if (bitCount_ == 0) {
                if (position_ >= size_) { failed_ = true; return 0; }
                current_ = data_[position_++];
                bitCount_ = 8;
            }
            value |= uint32_t(current_ & 1u) << i;
            current_ >>= 1;
            --bitCount_;
        }
        return value;
    }

    void alignToByte() { bitCount_ = 0; }

    bool copyBytes(std::vector<uint8_t>& out, size_t count) {
        if (position_ + count > size_) { failed_ = true; return false; }
        out.insert(out.end(), data_ + position_, data_ + position_ + count);
        position_ += count;
        return true;
    }

    bool failed() const { return failed_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t position_ = 0;
    uint8_t current_ = 0;
    int bitCount_ = 0;
    bool failed_ = false;
};

// ---------------------------------------------------------------------------
// Код Хаффмана
// ---------------------------------------------------------------------------

/// Канонический код Хаффмана, восстановленный из длин.
///
/// Устроен как в самой спецификации: счётчики по длинам и смещения. Быстрые
/// таблицы поиска здесь не нужны — атлас читается один раз за запуск.
struct Huffman {
    static constexpr int kMaxBits = 15;

    uint16_t counts[kMaxBits + 1] = {};
    std::vector<uint16_t> symbols;

    bool build(const uint8_t* lengths, size_t count) {
        std::memset(counts, 0, sizeof(counts));
        for (size_t i = 0; i < count; ++i) {
            if (lengths[i] > kMaxBits) return false;
            ++counts[lengths[i]];
        }
        // Все длины нулевые — пустой код. Допустим (например, дистанции
        // в потоке без ссылок назад), но декодировать по нему нельзя.
        counts[0] = 0;

        // Код обязан быть полным или неполным-но-однобитным. Переполненный
        // код означает битый поток.
        int left = 1;
        for (int length = 1; length <= kMaxBits; ++length) {
            left <<= 1;
            left -= counts[length];
            if (left < 0) return false;
        }

        // offsets[len] — куда класть первый символ длины len, то есть
        // сколько символов имеют длину МЕНЬШЕ len.
        uint16_t offsets[kMaxBits + 2] = {};
        for (int length = 1; length <= kMaxBits; ++length) {
            offsets[length + 1] = uint16_t(offsets[length] + counts[length]);
        }
        symbols.assign(count, 0);
        for (size_t i = 0; i < count; ++i) {
            // Индекс именно offsets[len], а не offsets[len + 1]. Первая
            // версия ошиблась здесь на единицу, и таблица символов
            // оказывалась сдвинутой — то есть распаковка выдавала мусор
            // на любом потоке с кодами Хаффмана.
            //
            // Проверка «записал и прочитал» этого не видела: наш writePng
            // пишет несжатые блоки, а они Хаффмана не трогают вовсе. Дефект
            // нашёлся только на настоящем файле из Blender.
            if (lengths[i] != 0) symbols[offsets[lengths[i]]++] = uint16_t(i);
        }
        return true;
    }

    /// Прочитать один символ. -1 — поток битый.
    int decode(BitReader& reader) const {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= kMaxBits; ++length) {
            code |= int(reader.bits(1));
            if (reader.failed()) return -1;
            const int count = counts[length];
            if (code - first < count) return symbols[size_t(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

// ---------------------------------------------------------------------------
// Inflate
// ---------------------------------------------------------------------------

constexpr uint16_t kLengthBase[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23,
                                    27, 31, 35, 43, 51, 59, 67, 83, 99,  115, 131, 163, 195, 227,
                                    258};
constexpr uint8_t kLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistanceBase[] = {1,    2,    3,    4,    5,    7,     9,     13,
                                      17,   25,   33,   49,   65,   97,    129,   193,
                                      257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                      4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistanceExtra[] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                      6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

/// Максимум, который мы согласны распаковать.
///
/// Заявленный размер приходит не из заголовка, а из самого потока, поэтому
/// ограничение ставим сами: без него подделанный файл заставил бы нас
/// выделять память, пока она не кончится.
constexpr size_t kMaxInflated = 256u * 1024u * 1024u;

bool inflateBlock(BitReader& reader, const Huffman& literals, const Huffman& distances,
                  std::vector<uint8_t>& out) {
    for (;;) {
        const int symbol = literals.decode(reader);
        if (symbol < 0) return false;

        if (symbol < 256) {
            if (out.size() >= kMaxInflated) return false;
            out.push_back(uint8_t(symbol));
            continue;
        }
        if (symbol == 256) return true;   // конец блока

        const int lengthIndex = symbol - 257;
        if (lengthIndex >= int(sizeof(kLengthBase) / sizeof(kLengthBase[0]))) return false;
        const size_t length =
            size_t(kLengthBase[lengthIndex]) + reader.bits(kLengthExtra[lengthIndex]);

        const int distanceSymbol = distances.decode(reader);
        if (distanceSymbol < 0) return false;
        if (distanceSymbol >= int(sizeof(kDistanceBase) / sizeof(kDistanceBase[0]))) return false;
        const size_t distance =
            size_t(kDistanceBase[distanceSymbol]) + reader.bits(kDistanceExtra[distanceSymbol]);
        if (reader.failed()) return false;

        // Ссылка назад дальше начала — битый поток. Без этой проверки
        // мы бы прочитали чужую память.
        if (distance == 0 || distance > out.size()) return false;
        if (out.size() + length > kMaxInflated) return false;

        // Копируем побайтно намеренно: длина может превышать расстояние,
        // и тогда копия читает то, что сама только что записала. Это не
        // ошибка, а штатный способ deflate кодировать повторы.
        const size_t start = out.size() - distance;
        for (size_t i = 0; i < length; ++i) out.push_back(out[start + i]);
    }
}

bool buildFixedTrees(Huffman& literals, Huffman& distances) {
    uint8_t literalLengths[288];
    for (int i = 0; i < 144; ++i) literalLengths[i] = 8;
    for (int i = 144; i < 256; ++i) literalLengths[i] = 9;
    for (int i = 256; i < 280; ++i) literalLengths[i] = 7;
    for (int i = 280; i < 288; ++i) literalLengths[i] = 8;

    uint8_t distanceLengths[30];
    for (int i = 0; i < 30; ++i) distanceLengths[i] = 5;

    return literals.build(literalLengths, 288) && distances.build(distanceLengths, 30);
}

bool buildDynamicTrees(BitReader& reader, Huffman& literals, Huffman& distances) {
    const int literalCount = int(reader.bits(5)) + 257;
    const int distanceCount = int(reader.bits(5)) + 1;
    const int codeLengthCount = int(reader.bits(4)) + 4;
    if (reader.failed()) return false;
    if (literalCount > 288 || distanceCount > 30) return false;

    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                       11, 4,  12, 3, 13, 2, 14, 1, 15};
    uint8_t codeLengths[19] = {};
    for (int i = 0; i < codeLengthCount; ++i) codeLengths[kOrder[i]] = uint8_t(reader.bits(3));
    if (reader.failed()) return false;

    Huffman codeTree;
    if (!codeTree.build(codeLengths, 19)) return false;

    uint8_t lengths[288 + 30] = {};
    int index = 0;
    while (index < literalCount + distanceCount) {
        const int symbol = codeTree.decode(reader);
        if (symbol < 0) return false;

        if (symbol < 16) {
            lengths[index++] = uint8_t(symbol);
            continue;
        }

        int repeat = 0;
        uint8_t value = 0;
        if (symbol == 16) {
            if (index == 0) return false;   // повторять нечего
            value = lengths[index - 1];
            repeat = 3 + int(reader.bits(2));
        } else if (symbol == 17) {
            repeat = 3 + int(reader.bits(3));
        } else {
            repeat = 11 + int(reader.bits(7));
        }
        if (reader.failed()) return false;
        if (index + repeat > literalCount + distanceCount) return false;
        for (int i = 0; i < repeat; ++i) lengths[index++] = value;
    }

    return literals.build(lengths, size_t(literalCount)) &&
           distances.build(lengths + literalCount, size_t(distanceCount));
}

bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    BitReader reader(data, size);

    for (;;) {
        const uint32_t last = reader.bits(1);
        const uint32_t type = reader.bits(2);
        if (reader.failed()) return false;

        if (type == 0) {
            // Несжатый блок. Именно такие пишет наш writePng.
            reader.alignToByte();
            const uint32_t length = reader.bits(16);
            const uint32_t inverse = reader.bits(16);
            if (reader.failed()) return false;
            if ((length ^ 0xFFFFu) != inverse) return false;
            if (out.size() + length > kMaxInflated) return false;
            if (!reader.copyBytes(out, length)) return false;
        } else if (type == 1 || type == 2) {
            Huffman literals, distances;
            const bool ready = type == 1 ? buildFixedTrees(literals, distances)
                                         : buildDynamicTrees(reader, literals, distances);
            if (!ready) return false;
            if (!inflateBlock(reader, literals, distances, out)) return false;
        } else {
            return false;   // тип 3 зарезервирован — значит поток битый
        }

        if (last) return true;
    }
}

// ---------------------------------------------------------------------------
// Разбор PNG
// ---------------------------------------------------------------------------

uint32_t readBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
           uint32_t(p[3]);
}

/// Восстановить строку по номеру фильтра (спецификация PNG, глава 9).
void unfilterRow(uint8_t filter, uint8_t* row, const uint8_t* previous, size_t width,
                 size_t stride) {
    switch (filter) {
        case 0:   // None
            break;
        case 1:   // Sub — слева
            for (size_t i = stride; i < width; ++i) row[i] = uint8_t(row[i] + row[i - stride]);
            break;
        case 2:   // Up — сверху
            if (previous != nullptr) {
                for (size_t i = 0; i < width; ++i) row[i] = uint8_t(row[i] + previous[i]);
            }
            break;
        case 3: {  // Average — среднее слева и сверху
            for (size_t i = 0; i < width; ++i) {
                const int left = i >= stride ? row[i - stride] : 0;
                const int up = previous != nullptr ? previous[i] : 0;
                row[i] = uint8_t(row[i] + (left + up) / 2);
            }
            break;
        }
        case 4: {  // Paeth — предсказание по трём соседям
            for (size_t i = 0; i < width; ++i) {
                const int left = i >= stride ? row[i - stride] : 0;
                const int up = previous != nullptr ? previous[i] : 0;
                const int upLeft = (previous != nullptr && i >= stride) ? previous[i - stride] : 0;

                const int estimate = left + up - upLeft;
                const int distLeft = estimate > left ? estimate - left : left - estimate;
                const int distUp = estimate > up ? estimate - up : up - estimate;
                const int distUpLeft = estimate > upLeft ? estimate - upLeft : upLeft - estimate;

                int predicted = upLeft;
                if (distLeft <= distUp && distLeft <= distUpLeft) predicted = left;
                else if (distUp <= distUpLeft) predicted = up;

                row[i] = uint8_t(row[i] + predicted);
            }
            break;
        }
        default:
            break;   // неизвестный фильтр отсеян раньше
    }
}

}  // namespace

bool decodePng(const uint8_t* data, size_t size, std::vector<Rgba8>& pixels, int& width,
               int& height) {
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (size < 8 || std::memcmp(data, kSignature, 8) != 0) return false;

    width = 0;
    height = 0;
    uint8_t bitDepth = 0, colorType = 0, interlace = 0;
    std::vector<uint8_t> compressed;
    bool sawHeader = false;

    size_t position = 8;
    while (position + 8 <= size) {
        const uint32_t length = readBE32(data + position);
        const char* tag = reinterpret_cast<const char*>(data + position + 4);
        position += 8;
        // Длина куска приходит из файла: без проверки она увела бы нас
        // за конец буфера ещё до всякой распаковки.
        if (length > size || position + length + 4 > size) return false;

        if (std::memcmp(tag, "IHDR", 4) == 0) {
            if (length != 13) return false;
            width = int(readBE32(data + position));
            height = int(readBE32(data + position + 4));
            bitDepth = data[position + 8];
            colorType = data[position + 9];
            interlace = data[position + 12];
            sawHeader = true;

            if (width <= 0 || height <= 0) return false;
            // Ограничение на размер: 16384 на сторону перекрывает любой
            // разумный атлас, а произвольное число из файла — нет.
            if (width > 16384 || height > 16384) return false;
            if (bitDepth != 8) return false;
            if (interlace != 0) return false;
            if (colorType != 0 && colorType != 2 && colorType != 4 && colorType != 6) {
                return false;
            }
        } else if (std::memcmp(tag, "IDAT", 4) == 0) {
            if (!sawHeader) return false;
            compressed.insert(compressed.end(), data + position, data + position + length);
        } else if (std::memcmp(tag, "IEND", 4) == 0) {
            break;
        }

        position += length + 4;   // тело плюс контрольная сумма
    }

    if (!sawHeader || compressed.size() < 2) return false;

    // Заголовок zlib: два байта, дальше поток deflate.
    const uint8_t method = compressed[0] & 0x0Fu;
    if (method != 8) return false;
    if ((compressed[1] & 0x20u) != 0) return false;   // словарь не поддерживаем

    std::vector<uint8_t> raw;
    if (!inflate(compressed.data() + 2, compressed.size() - 2, raw)) return false;

    const size_t channels = colorType == 0 ? 1 : colorType == 2 ? 3 : colorType == 4 ? 2 : 4;
    const size_t rowBytes = size_t(width) * channels;
    const size_t expected = (rowBytes + 1) * size_t(height);
    if (raw.size() < expected) return false;

    pixels.assign(size_t(width) * size_t(height), Rgba8{});

    std::vector<uint8_t> previous(rowBytes, 0);
    std::vector<uint8_t> current(rowBytes, 0);

    for (int y = 0; y < height; ++y) {
        const size_t offset = size_t(y) * (rowBytes + 1);
        const uint8_t filter = raw[offset];
        if (filter > 4) return false;

        std::memcpy(current.data(), raw.data() + offset + 1, rowBytes);
        unfilterRow(filter, current.data(), y == 0 ? nullptr : previous.data(), rowBytes,
                    channels);

        for (int x = 0; x < width; ++x) {
            const uint8_t* source = current.data() + size_t(x) * channels;
            Rgba8& out = pixels[size_t(y) * size_t(width) + size_t(x)];
            switch (colorType) {
                case 0:   // серый
                    out = Rgba8{source[0], source[0], source[0], 255};
                    break;
                case 2:   // RGB
                    out = Rgba8{source[0], source[1], source[2], 255};
                    break;
                case 4:   // серый с альфой
                    out = Rgba8{source[0], source[0], source[0], source[1]};
                    break;
                default:  // RGBA
                    out = Rgba8{source[0], source[1], source[2], source[3]};
                    break;
            }
        }
        previous.swap(current);
    }
    return true;
}

bool readPng(const std::string& path, std::vector<Rgba8>& pixels, int& width, int& height) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }

    // Скобки здесь читались бы компилятором как объявление функции —
    // тот самый most vexing parse.
    std::vector<uint8_t> data;
    data.resize(size_t(size));
    const size_t got = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (got != data.size()) return false;

    return decodePng(data.data(), data.size(), pixels, width, height);
}

}  // namespace pw
