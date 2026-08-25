#include "pw/render/font.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace pw::render {

namespace {

std::string directoryOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

/// Значение числового поля json. Формат наш, разбор строгий.
bool numberField(const std::string& text, const std::string& key, long& out) {
    const std::string needle = "\"" + key + "\"";
    const size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    const size_t colon = text.find(':', at + needle.size());
    if (colon == std::string::npos) return false;

    size_t position = colon + 1;
    while (position < text.size() && (text[position] == ' ' || text[position] == '\n')) {
        ++position;
    }
    char* end = nullptr;
    out = std::strtol(text.c_str() + position, &end, 10);
    return end != text.c_str() + position;
}

bool stringField(const std::string& text, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\"";
    const size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    const size_t colon = text.find(':', at + needle.size());
    if (colon == std::string::npos) return false;
    const size_t open = text.find('"', colon);
    if (open == std::string::npos) return false;

    // Внутри строки могут быть экранированные кавычки: набор символов
    // содержит и обратную косую, и кавычку.
    std::string value;
    for (size_t i = open + 1; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == '"') { value.push_back('"'); ++i; continue; }
            if (next == '\\') { value.push_back('\\'); ++i; continue; }
            if (next == 'n') { value.push_back('\n'); ++i; continue; }
        }
        if (text[i] == '"') { out = value; return true; }
        value.push_back(text[i]);
    }
    return false;
}

}  // namespace

std::vector<uint32_t> decodeUtf8(const std::string& text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size();) {
        const uint8_t lead = uint8_t(text[i]);
        uint32_t code = 0;
        size_t length = 1;

        if (lead < 0x80) {
            code = lead;
        } else if ((lead & 0xE0u) == 0xC0u) {
            code = lead & 0x1Fu;
            length = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            code = lead & 0x0Fu;
            length = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            code = lead & 0x07u;
            length = 4;
        } else {
            // Одинокий продолжающий байт: строка битая. Пропускаем его
            // и идём дальше — надпись важнее строгости.
            ++i;
            continue;
        }

        if (i + length > text.size()) break;
        bool ok = true;
        for (size_t k = 1; k < length; ++k) {
            const uint8_t next = uint8_t(text[i + k]);
            if ((next & 0xC0u) != 0x80u) { ok = false; break; }
            code = (code << 6) | (next & 0x3Fu);
        }
        if (ok) out.push_back(code);
        i += length;
    }
    return out;
}

bool Font::load(const std::string& jsonPath) {
    glyphs_.clear();
    codes_.clear();
    pixels_.clear();
    error_.clear();

    std::ifstream file(jsonPath);
    if (!file) {
        error_ = "не удалось открыть " + jsonPath;
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    long columns = 0, cell = 0, width = 0, height = 0;
    std::string charset, texture;
    if (!numberField(text, "columns", columns) || !numberField(text, "cell", cell) ||
        !numberField(text, "width", width) || !numberField(text, "height", height) ||
        !stringField(text, "charset", charset) || !stringField(text, "texture", texture)) {
        error_ = "описание шрифта неполное";
        return false;
    }
    if (columns <= 0 || cell <= 0 || width <= 0 || height <= 0) {
        error_ = "описание шрифта содержит бессмысленные размеры";
        return false;
    }

    if (!readPng(directoryOf(jsonPath) + texture, pixels_, width_, height_)) {
        error_ = "не удалось прочитать " + directoryOf(jsonPath) + texture;
        return false;
    }
    cell_ = int(cell);

    const std::vector<uint32_t> codes = decodeUtf8(charset);
    codes_.reserve(codes.size());
    glyphs_.reserve(codes.size());

    const float inverseWidth = 1.0f / float(width_);
    const float inverseHeight = 1.0f / float(height_);
    for (size_t index = 0; index < codes.size(); ++index) {
        const long column = long(index) % columns;
        const long row = long(index) / columns;

        Glyph glyph;
        glyph.u0 = float(column * cell) * inverseWidth;
        glyph.v0 = float(row * cell) * inverseHeight;
        glyph.u1 = float((column + 1) * cell) * inverseWidth;
        glyph.v1 = float((row + 1) * cell) * inverseHeight;

        codes_.push_back(codes[index]);
        glyphs_.push_back(glyph);
    }

    if (glyphs_.empty()) {
        error_ = "в описании шрифта нет ни одного глифа";
        return false;
    }
    return true;
}

const Font::Glyph* Font::find(uint32_t code) const {
    for (size_t index = 0; index < codes_.size(); ++index) {
        if (codes_[index] == code) return &glyphs_[index];
    }
    return nullptr;
}

float Font::width(const std::string& utf8, float lineHeight) const {
    return float(decodeUtf8(utf8).size()) * advance(lineHeight);
}

void Font::layout(const std::string& utf8, float x, float y, float lineHeight,
                  const TextColor& color, std::vector<rhi::SpriteInstance>& out) const {
    if (!valid()) return;

    const float step = advance(lineHeight);
    float cursorX = x;
    float cursorY = y;

    for (uint32_t code : decodeUtf8(utf8)) {
        if (code == '\n') {
            cursorX = x;
            cursorY += lineHeight * 1.25f;
            continue;
        }
        const Glyph* glyph = find(code);
        if (glyph == nullptr) {
            // Неизвестный символ занимает место, но ничего не рисует:
            // строка не должна съезжать из-за одной буквы.
            cursorX += step;
            continue;
        }

        rhi::SpriteInstance sprite;
        // Клетка глифа квадратная, поэтому спрайт тоже квадратный, а шаг
        // курсора меньше — так буквы стоят вплотную, а не через пробел.
        sprite.halfWidth = lineHeight * 0.5f;
        sprite.halfHeight = lineHeight * 0.5f;
        sprite.x = cursorX + sprite.halfWidth;
        sprite.y = cursorY + sprite.halfHeight;
        sprite.u0 = glyph->u0;
        sprite.v0 = glyph->v0;
        sprite.u1 = glyph->u1;
        sprite.v1 = glyph->v1;
        sprite.r = color.r;
        sprite.g = color.g;
        sprite.b = color.b;
        sprite.a = color.a;
        out.push_back(sprite);

        cursorX += step;
    }
}

}  // namespace pw::render
