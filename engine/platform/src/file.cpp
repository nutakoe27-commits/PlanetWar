#include "pw/platform/file.h"

#include <SDL3/SDL.h>

namespace pw {

std::vector<uint8_t> readFile(const std::string& path, bool* ok) {
    if (ok) *ok = false;
    std::vector<uint8_t> data;

    // Через SDL, а не через fopen: на Android ассеты лежат внутри apk и
    // обычным файлом не являются. Один путь чтения на все платформы.
    size_t size = 0;
    void* raw = SDL_LoadFile(path.c_str(), &size);
    if (!raw) return data;

    const auto* bytes = static_cast<const uint8_t*>(raw);
    data.assign(bytes, bytes + size);
    SDL_free(raw);
    if (ok) *ok = true;
    return data;
}

std::string readTextFile(const std::string& path, bool* ok) {
    const std::vector<uint8_t> bytes = readFile(path, ok);
    return std::string(bytes.begin(), bytes.end());
}

bool writeFile(const std::string& path, const void* data, size_t bytes) {
    SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "wb");
    if (!stream) return false;
    const size_t written = SDL_WriteIO(stream, data, bytes);
    SDL_CloseIO(stream);
    return written == bytes;
}

std::string basePath() {
    const char* path = SDL_GetBasePath();
    return path ? std::string(path) : std::string();
}

std::string prefPath(const std::string& app) {
    char* path = SDL_GetPrefPath("PlanetWar", app.c_str());
    if (!path) return std::string();
    std::string result(path);
    SDL_free(path);
    return result;
}

bool fileExists(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info);
}

}  // namespace pw
