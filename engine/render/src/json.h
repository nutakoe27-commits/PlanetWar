#pragma once

// Внутренние мелочи pw_render: разбор json и работа с путями.
//
// Заголовок ВНУТРЕННИЙ: лежит в src/, наружу не ставится. Существует
// затем, чтобы атлас и вид системы читали свои манифесты одним и тем же
// разбором, а не двумя похожими копиями, которые однажды разойдутся.

#include <cstdlib>
#include <string>

namespace pw::render {

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

inline std::string directoryOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}


}  // namespace pw::render
