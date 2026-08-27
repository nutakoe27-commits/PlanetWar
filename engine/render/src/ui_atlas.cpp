#include "pw/render/ui_atlas.h"

#include <fstream>
#include <sstream>

#include "json.h"

namespace pw::render {

bool UiAtlas::load(const std::string& jsonPath) {
    sprites_.clear();
    pixels_.clear();
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
        error_ = "в манифесте интерфейса нет размера атласа: " + jsonPath;
        return false;
    }

    std::string textureName;
    if (!json.string("texture", textureName)) {
        error_ = "в манифесте интерфейса не назван файл текстуры: " + jsonPath;
        return false;
    }
    if (!readPng(directoryOf(jsonPath) + textureName, pixels_, width_, height_)) {
        error_ = "не удалось прочитать " + directoryOf(jsonPath) + textureName;
        return false;
    }

    size_t cursor = json.find("sprites");
    if (cursor == std::string::npos) {
        error_ = "в манифесте интерфейса нет списка спрайтов: " + jsonPath;
        return false;
    }

    const float side = float(atlasSize);
    while (true) {
        const size_t entry = json.find("name", cursor);
        if (entry == std::string::npos) break;

        UiSprite sprite;
        if (!json.string("name", sprite.name, cursor)) break;

        long x = 0, y = 0, w = 0, h = 0, border = 0;
        if (!json.number("x", x, entry) || !json.number("y", y, entry) ||
            !json.number("w", w, entry) || !json.number("h", h, entry)) {
            break;
        }
        json.number("border", border, entry);

        sprite.x = int(x);
        sprite.y = int(y);
        sprite.width = int(w);
        sprite.height = int(h);
        sprite.border = int(border);
        sprite.u0 = float(x) / side;
        sprite.v0 = float(y) / side;
        sprite.u1 = float(x + w) / side;
        sprite.v1 = float(y + h) / side;

        sprites_.push_back(std::move(sprite));
        cursor = entry + 1;
    }

    if (sprites_.empty()) {
        error_ = "в манифесте интерфейса не нашлось ни одного спрайта: " + jsonPath;
        return false;
    }
    return true;
}

const UiSprite* UiAtlas::find(const std::string& name) const {
    for (const UiSprite& sprite : sprites_) {
        if (sprite.name == name) return &sprite;
    }
    return nullptr;
}

}  // namespace pw::render
