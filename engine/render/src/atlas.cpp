#include "pw/render/atlas.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace pw::render {

namespace {

/// Крошечный разбор json ровно под наш формат.
///
/// Полноценная библиотека здесь была бы лишней зависимостью ради одного
/// файла, который пишем мы сами и который не меняется. Разбор строгий:
/// всё, чего мы не ждём, отвергается, а не угадывается.
class Json {
public:
    explicit Json(const std::string& text) : text_(text) {}

    /// Найти число по имени поля, начиная с позиции `from`.
    bool number(const std::string& key, long& out, size_t from = 0) const {
        const size_t at = find(key, from);
        if (at == std::string::npos) return false;
        return parseNumber(at, out);
    }

    /// Найти строку по имени поля.
    bool string(const std::string& key, std::string& out, size_t from = 0) const {
        const size_t at = find(key, from);
        if (at == std::string::npos) return false;

        const size_t open = text_.find('"', at);
        if (open == std::string::npos) return false;
        const size_t close = text_.find('"', open + 1);
        if (close == std::string::npos) return false;
        out = text_.substr(open + 1, close - open - 1);
        return true;
    }

    /// Позиция значения поля `key` после `from`.
    size_t find(const std::string& key, size_t from = 0) const {
        const std::string needle = "\"" + key + "\"";
        const size_t at = text_.find(needle, from);
        if (at == std::string::npos) return std::string::npos;
        const size_t colon = text_.find(':', at + needle.size());
        return colon == std::string::npos ? std::string::npos : colon + 1;
    }

    const std::string& text() const { return text_; }

private:
    bool parseNumber(size_t at, long& out) const {
        while (at < text_.size() && (text_[at] == ' ' || text_[at] == '\n')) ++at;
        if (at >= text_.size()) return false;
        char* end = nullptr;
        out = std::strtol(text_.c_str() + at, &end, 10);
        return end != text_.c_str() + at;
    }

    std::string text_;
};

std::string directoryOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

}  // namespace

bool Atlas::load(const std::string& jsonPath) {
    frames_.clear();
    albedo_.clear();
    accentMask_.clear();
    error_.clear();

    std::ifstream file(jsonPath);
    if (!file) {
        error_ = "не удалось открыть " + jsonPath;
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const Json json(buffer.str());

    long atlasSize = 0;
    if (!json.number("atlas_size", atlasSize) || atlasSize <= 0) {
        error_ = "в описании атласа нет размера";
        return false;
    }
    long steps = 1;
    json.number("rotation_steps", steps);
    rotationSteps_ = uint32_t(steps > 0 ? steps : 1);

    std::string albedoName, maskName;
    const size_t textures = json.find("textures");
    if (textures == std::string::npos || !json.string("albedo", albedoName, textures)) {
        error_ = "в описании атласа нет текстуры альбедо";
        return false;
    }
    json.string("accent_mask", maskName, textures);

    const std::string directory = directoryOf(jsonPath);
    if (!readPng(directory + albedoName, albedo_, width_, height_)) {
        error_ = "не удалось прочитать " + directory + albedoName;
        return false;
    }
    if (!maskName.empty()) {
        int maskWidth = 0, maskHeight = 0;
        if (readPng(directory + maskName, accentMask_, maskWidth, maskHeight)) {
            // Маска обязана совпадать с альбедо: иначе цвет империи ляжет
            // не на те пиксели, и корабли будут раскрашены как попало.
            if (maskWidth != width_ || maskHeight != height_) accentMask_.clear();
        }
    }

    // Кадры. Идём по списку последовательно: имена полей внутри одного
    // кадра одинаковы, и искать их можно только по возрастанию позиции.
    size_t cursor = json.find("frames");
    if (cursor == std::string::npos) {
        error_ = "в описании атласа нет кадров";
        return false;
    }

    const float inverseSize = 1.0f / float(atlasSize);
    for (;;) {
        const size_t hullAt = json.find("hull", cursor);
        if (hullAt == std::string::npos) break;

        AtlasFrame frame;
        if (!json.string("hull", frame.hull, cursor)) break;

        long rotation = 0, x = 0, y = 0, w = 0, h = 0;
        if (!json.number("rotation", rotation, hullAt)) break;
        if (!json.number("x", x, hullAt)) break;
        if (!json.number("y", y, hullAt)) break;
        if (!json.number("w", w, hullAt)) break;
        if (!json.number("h", h, hullAt)) break;

        frame.rotation = uint32_t(rotation);
        frame.x = int(x);
        frame.y = int(y);
        frame.width = int(w);
        frame.height = int(h);
        frame.u0 = float(x) * inverseSize;
        frame.v0 = float(y) * inverseSize;
        frame.u1 = float(x + w) * inverseSize;
        frame.v1 = float(y + h) * inverseSize;
        frames_.push_back(frame);

        cursor = json.find("h", hullAt);
        if (cursor == std::string::npos) break;
    }

    if (frames_.empty()) {
        error_ = "в описании атласа нет ни одного кадра";
        return false;
    }
    return true;
}

const AtlasFrame* Atlas::frame(const std::string& hull) const {
    for (const AtlasFrame& frame : frames_) {
        if (frame.hull == hull) return &frame;
    }
    return nullptr;
}

const AtlasFrame* Atlas::frame(const std::string& hull, float rotationTurns) const {
    if (rotationSteps_ == 0) return frame(hull);

    // Приводим угол к ближайшему испечённому направлению. Держать кадр
    // на каждый градус означало бы атлас в сорок пять раз больше.
    float normalized = rotationTurns - std::floor(rotationTurns);
    const uint32_t step =
        uint32_t(std::lround(normalized * float(rotationSteps_))) % rotationSteps_;

    for (const AtlasFrame& frame : frames_) {
        if (frame.hull == hull && frame.rotation == step) return &frame;
    }
    return frame(hull);
}

std::vector<Rgba8> Atlas::composite() const {
    std::vector<Rgba8> out = albedo_;
    if (accentMask_.size() != out.size()) return out;

    // Маска акцента отмечает, где корпус должен принять цвет империи.
    // Там мы оставляем яркий белый — шейдер умножит его на цвет игрока;
    // остальное остаётся серым металлом самой модели.
    for (size_t i = 0; i < out.size(); ++i) {
        const uint8_t accent = accentMask_[i].r;
        if (accent == 0) continue;
        const float weight = float(accent) / 255.0f;
        out[i].r = uint8_t(float(out[i].r) * (1.0f - weight) + 255.0f * weight);
        out[i].g = uint8_t(float(out[i].g) * (1.0f - weight) + 255.0f * weight);
        out[i].b = uint8_t(float(out[i].b) * (1.0f - weight) + 255.0f * weight);
    }
    return out;
}

}  // namespace pw::render
