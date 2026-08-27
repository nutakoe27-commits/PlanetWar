#pragma once

// Экран игры: панели, карточки, кнопки.
//
// ПРАВИЛО, ИЗ КОТОРОГО СЛЕДУЕТ ВСЁ ОСТАЛЬНОЕ: любое действие делается
// мышью. Клавиши остаются, но ни одно действие не требует их знать.
// Игрок, впервые открывший игру, обязан суметь построить шахту, не читая
// подсказок и не угадывая, что цифра «1» что-то значит.
//
// Отсюда устройство: экран не «показывает состояние», он ВОЗВРАЩАЕТ
// НАМЕРЕНИЕ. Нажали «построить шахту» — вернулось намерение построить
// шахту, а приказ отправит вызывающий. Экран не знает ни про сеть,
// ни про правила, и потому проверяется без сервера: подсунуть состояние,
// сказать «мышь щёлкнула сюда» и прочитать, какое намерение вернулось.
//
// На экране лежит то, без чего нельзя принять решение ПРЯМО СЕЙЧАС.
// Ресурсы — можно ли строить. Планета — что с ней делать. Флот — чем
// воевать. История, статистика и дипломатия — это окна, а окна работа
// Фазы 3.

#include <cstdint>
#include <string>
#include <vector>

#include "pw/game/client.h"
#include "pw/sim/season.h"
#include "pw/render/map_view.h"
#include "pw/render/ui.h"

namespace pw::render {

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
    static constexpr size_t kMaxVisible = 5;
    static constexpr int64_t kLifetime = 8000;

    void add(const std::string& text, const TextColor& color, int64_t now,
             uint32_t system = 0xFFFFFFFFu, const char* icon = nullptr);
    /// Убрать погасшие. Вызывать каждый кадр.
    void update(int64_t now);

    struct Entry {
        std::string text;
        TextColor color;
        int64_t bornAt = 0;
        /// Куда смотреть. Щелчок по сообщению наводит карту сюда —
        /// новость о том, что где-то идёт осада, бесполезна, если до этого
        /// «где-то» надо ещё доскроллить вручную.
        uint32_t system = 0xFFFFFFFFu;
        const char* icon = nullptr;
        /// Сколько раз новость повторилась подряд. Три одинаковых строки
        /// подряд — это не три новости, а одна: захват системы с тремя
        /// планетами не должен съедать весь журнал.
        uint32_t count = 1;
    };
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------
// Намерения
// ---------------------------------------------------------------------------

enum class ActionKind : uint8_t {
    None,
    /// Открыть выбранную систему в объёме и вернуться на карту.
    EnterSystem,
    LeaveSystem,
    /// Выбрать планету по порядковому номеру в системе (`value`).
    SelectPlanet,
    /// Выбрать слот застройки (`slot`). 0xFF снимает выбор.
    SelectSlot,
    /// Построить `value` (Building) в слоте `slot` на планете `planet`.
    Build,
    /// Снести здание в слоте `slot` на планете `planet`.
    Demolish,
    /// Отменить текущую стройку на планете `planet`.
    CancelBuild,
    /// Заказать корабль `value` (Hull) в выбранной системе.
    OrderShip,
    /// Выбрать флот с идентификатором `value`.
    SelectFleet,
    /// Взвести приказ: следующий щелчок по карте задаст цель.
    BeginMove,
    /// Снять взведённый приказ.
    CancelMove,
    /// Навести камеру на систему `value`.
    FocusSystem,
    /// Показать всю галактику или всю систему целиком.
    ResetView,
    /// Выйти из игры. Приходит только со второго щелчка: первый взводит.
    Quit,
    Count,
};

/// Имя намерения для отчётов прогона.
///
/// Нужно не игроку, а проверке: обход экрана обязан уметь сказать, ДО ЧЕГО
/// он добрался. Прогон, который жив, но ни разу не нажал на кнопку, ничего
/// про кнопку не доказывает.
const char* actionName(ActionKind kind);

struct ScreenAction {
    ActionKind kind = ActionKind::None;
    uint32_t value = 0;
    uint32_t planet = 0;
    uint8_t slot = 0xFF;

    explicit operator bool() const { return kind != ActionKind::None; }
};

/// Что игрок сейчас выбрал и в каком виде находится.
struct ScreenState {
    /// Игрок внутри системы или на карте галактики.
    bool inSystem = false;
    uint32_t system = 0xFFFFFFFFu;
    /// Порядковый номер планеты в системе, а не номер сущности: человек
    /// думает «вторая планета отсюда», а не «планета 507».
    uint32_t planetIndex = 0;
    /// Выбранный слот застройки. 0xFF — не выбран, палитра скрыта.
    uint8_t slot = 0xFF;
    uint32_t fleet = 0xFFFFFFFFu;
    /// Приказ взведён: следующий щелчок по карте — это цель для флота.
    bool awaitingMoveTarget = false;
};

// ---------------------------------------------------------------------------
// Справочник интерфейса
// ---------------------------------------------------------------------------

/// Имя здания. Именительный падеж: «шахта», «верфь».
const char* buildingName(uint8_t building);
/// Имя здания в винительном падеже: «снести шахту», «строю верфь».
///
/// Отдельной функцией, а не склейкой с «Снести »: «Снести шахта» —
/// это первое, что видит человек, и по этой строчке он делает вывод обо
/// всей игре. Русский требует падежей, и интерфейс обязан их знать.
const char* buildingNameAccusative(uint8_t building);
/// Спрайт значка здания в атласе интерфейса.
const char* buildingIcon(uint8_t building);
/// Чем здание полезно. Одна строка: это подсказка, а не справка.
const char* buildingHint(uint8_t building);
const char* hullName(uint8_t hull);
/// Имя корпуса в винительном падеже: «заказан линкор», «строю носитель».
const char* hullNameAccusative(uint8_t hull);
/// Чем корпус полезен. Одна строка: у флота из восьми классов игрок обязан
/// понимать роль каждого, не открывая справочник.
const char* hullHint(uint8_t hull);
const char* hullIcon(uint8_t hull);
const char* starName(uint8_t starClass);
/// Чем этот класс светила ценен. Одна строка для подсказки.
const char* starHint(uint8_t starClass);
const char* planetClassName(uint8_t planetClass);
/// Что означает стадия сезона. Одна строка для подсказки.
const char* stageHint(sim::SeasonStage stage);
std::string noticeText(game::NoticeKind kind);
/// Значок новости. Иконка узнаётся быстрее строки: игрок понимает,
/// что случилось, ещё не прочитав текст.
const char* noticeIcon(game::NoticeKind kind);
/// Плохая ли новость. От этого зависит цвет строки в журнале.
bool noticeIsBad(game::NoticeKind kind);

class Screen {
public:
    void setMessages(const MessageLog* messages) { messages_ = messages; }

    /// Собрать экран и вернуть намерение игрока.
    ///
    /// Намерение за кадр ровно одно: два нажатия в одном кадре означали бы,
    /// что игрок физически нажал две кнопки одновременно, а такого нет.
    ScreenAction build(Ui& ui, const game::Client& client, const ScreenState& state,
                       int64_t now);

private:
    const MessageLog* messages_ = nullptr;
    /// Когда взвели «Выход». Второй щелчок подтверждает, через несколько
    /// секунд взвод спадает сам: выход по одному нажатию рядом с кнопкой
    /// «показать всё» — это потерянная партия из-за дрогнувшей руки.
    int64_t quitArmedAt_ = 0;

    ScreenAction topBar(Ui& ui, const game::Client& client) const;
    /// Панели левого столбца. Каждая возвращает свою нижнюю границу
    /// в `bottom`, и следующая начинается от неё: столбец растёт сверху
    /// вниз, а не расставляется по заранее угаданным координатам.
    ScreenAction systemPanel(Ui& ui, const game::Client& client,
                             const ScreenState& state, float top, float width,
                             float& bottom) const;
    ScreenAction planetPanel(Ui& ui, const game::Client& client,
                             const ScreenState& state, float top, float width,
                             float& bottom) const;
    ScreenAction fleetPanel(Ui& ui, const game::Client& client,
                            const ScreenState& state, float top, float width) const;
    ScreenAction messagePanel(Ui& ui, int64_t now, float top, float left,
                              float right) const;
    ScreenAction bottomBar(Ui& ui, const game::Client& client, const ScreenState& state,
                           int64_t now);
};

}  // namespace pw::render
