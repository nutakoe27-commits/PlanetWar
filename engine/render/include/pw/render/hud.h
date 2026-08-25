#pragma once

// Панели на экране.
//
// Собирает надписи и подложки в спрайты, ничего не зная ни про Vulkan,
// ни про окно — ровно как MapView. Поэтому интерфейс тоже проверяется
// БЕЗ ВИДЕОКАРТЫ: тест кладёт состояние, просит собрать панель и читает,
// какие строки в ней оказались.
//
// Что показывать, решено по одному правилу: на экране лежит то, без чего
// нельзя принять решение прямо сейчас. Ресурсы — можно ли строить.
// Система под курсором — стоит ли туда идти. Флот — чем воевать.
// Всё остальное (история, статистика, дипломатия) — это окна, а они
// работа Фазы 3.

#include <string>
#include <vector>

#include "pw/game/client.h"
#include "pw/render/font.h"
#include "pw/render/map_view.h"

namespace pw::render {

/// Строка панели: текст и цвет.
struct HudLine {
    std::string text;
    TextColor color;
};

/// Собранная панель.
struct HudFrame {
    std::vector<rhi::SpriteInstance> sprites;
    /// Строки как текст — для проверок и для отладочного вывода.
    std::vector<HudLine> lines;

    void clear() {
        sprites.clear();
        lines.clear();
    }
};

/// Короткий журнал событий на экране.
///
/// Игрок нажал — игрок обязан увидеть ответ. Без этого нажатие в пустоту
/// неотличимо от нажатия, которое сервер отверг, и человек несколько раз
/// жмёт одно и то же, не понимая, почему ничего не происходит.
///
/// Сообщения гаснут: постоянно висящий список превращается в шум, а
/// внимание в стратегии — самый дефицитный ресурс игрока.
class MessageLog {
public:
    /// Сколько сообщений держать на экране одновременно.
    static constexpr size_t kMaxVisible = 5;
    /// Сколько миллисекунд живёт сообщение.
    static constexpr int64_t kLifetime = 8000;

    void add(const std::string& text, const TextColor& color, int64_t now);
    /// Убрать погасшие. Вызывать каждый кадр.
    void update(int64_t now);

    struct Entry {
        std::string text;
        TextColor color;
        int64_t bornAt = 0;
    };
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

class Hud {
public:
    void setFont(const Font* font) { font_ = font; }
    void setMessages(const MessageLog* messages) { messages_ = messages; }

    /// Собрать панели для текущего состояния.
    ///
    /// `screenWidth`/`screenHeight` — в пикселях кадрового буфера.
    void build(const game::Client& client, const Selection& selection, int screenWidth,
               int screenHeight, HudFrame& out) const;

    /// То же, но со временем — нужно, чтобы сообщения гасли.
    void build(const game::Client& client, const Selection& selection, int screenWidth,
               int screenHeight, int64_t now, HudFrame& out) const;

    /// Высота строки в пикселях. Растёт с экраном: на 4K надпись в 14
    /// пикселей нечитаема, а на ноутбуке в 28 занимает пол-экрана.
    static float lineHeight(int screenHeight);

private:
    const Font* font_ = nullptr;
    const MessageLog* messages_ = nullptr;

    void push(HudFrame& out, const std::string& text, float x, float y, float height,
              const TextColor& color) const;
};

}  // namespace pw::render
