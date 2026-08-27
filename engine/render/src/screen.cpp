#include "pw/render/screen.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "pw/sim/production.h"
#include "pw/sim/season.h"

namespace pw::render {

namespace {

std::string number(int64_t value) { return std::to_string(value); }

/// Число с разделителем тысяч.
///
/// «12 480» читается с одного взгляда, «12480» приходится считать глазами
/// по цифрам. На панели, куда смотрят раз в секунду, это разница между
/// «увидел» и «прочитал».
std::string grouped(int64_t value) {
    const bool negative = value < 0;
    std::string digits = std::to_string(negative ? -value : value);
    std::string out;
    int counter = 0;
    for (size_t i = digits.size(); i-- > 0;) {
        out.insert(out.begin(), digits[i]);
        if (++counter % 3 == 0 && i > 0) out.insert(out.begin(), ' ');
    }
    return negative ? "-" + out : out;
}

/// Срок в человеческом виде.
std::string duration(int64_t seconds) {
    if (seconds < 60) return number(seconds) + " с";
    const int64_t minutes = seconds / 60;
    if (minutes < 60) return number(minutes) + " мин";
    return number(minutes / 60) + " ч " + number(minutes % 60) + " мин";
}

constexpr uint32_t kNoSystem = 0xFFFFFFFFu;
constexpr uint8_t kNoSlot = 0xFF;

}  // namespace

// ---------------------------------------------------------------------------
// Справочник
// ---------------------------------------------------------------------------

const char* buildingName(uint8_t building) {
    switch (sim::Building(building)) {
        case sim::Building::None:       return "пусто";
        case sim::Building::Mine:       return "шахта";
        case sim::Building::PowerPlant: return "электростанция";
        case sim::Building::Foundry:    return "литейная";
        case sim::Building::Laboratory: return "лаборатория";
        case sim::Building::TradeHub:   return "торговый узел";
        case sim::Building::Fortress:   return "крепость";
        case sim::Building::Shipyard:   return "верфь";
        case sim::Building::SupplyDepot:     return "узел снабжения";
        case sim::Building::ShieldGenerator: return "планетарный щит";
        case sim::Building::Drydock:         return "ремонтный док";
        case sim::Building::Habitat:         return "хабитат";
        case sim::Building::Garrison:        return "гарнизон";
        default:                        return "?";
    }
}

const char* buildingNameAccusative(uint8_t building) {
    switch (sim::Building(building)) {
        case sim::Building::None:       return "пустой слот";
        case sim::Building::Mine:       return "шахту";
        case sim::Building::PowerPlant: return "электростанцию";
        case sim::Building::Foundry:    return "литейную";
        case sim::Building::Laboratory: return "лабораторию";
        case sim::Building::TradeHub:   return "торговый узел";
        case sim::Building::Fortress:   return "крепость";
        case sim::Building::Shipyard:   return "верфь";
        case sim::Building::SupplyDepot:     return "узел снабжения";
        case sim::Building::ShieldGenerator: return "планетарный щит";
        case sim::Building::Drydock:         return "ремонтный док";
        case sim::Building::Habitat:         return "хабитат";
        case sim::Building::Garrison:        return "гарнизон";
        default:                        return "?";
    }
}

const char* buildingIcon(uint8_t building) {
    switch (sim::Building(building)) {
        case sim::Building::Mine:       return "bld_mine";
        case sim::Building::PowerPlant: return "bld_power";
        case sim::Building::Foundry:    return "bld_foundry";
        case sim::Building::Laboratory: return "bld_lab";
        case sim::Building::TradeHub:   return "bld_trade";
        case sim::Building::Fortress:   return "bld_fortress";
        case sim::Building::Shipyard:   return "bld_shipyard";
        case sim::Building::SupplyDepot:     return "bld_depot";
        case sim::Building::ShieldGenerator: return "bld_shield";
        case sim::Building::Drydock:         return "bld_drydock";
        case sim::Building::Habitat:         return "bld_habitat";
        case sim::Building::Garrison:        return "bld_garrison";
        default:                        return nullptr;
    }
}

const char* buildingHint(uint8_t building) {
    switch (sim::Building(building)) {
        case sim::Building::Mine:       return "минералы — основа всего";
        case sim::Building::PowerPlant: return "энергия на содержание";
        case sim::Building::Foundry:    return "сплавы из минералов, других источников нет";
        case sim::Building::Laboratory: return "исследования";
        case sim::Building::TradeHub:   return "энергия и влияние";
        case sim::Building::Fortress:   return "поднимает потолок обороны ЭТОЙ планеты";
        case sim::Building::Shipyard:   return "без неё система не строит флот";
        case sim::Building::SupplyDepot:
            return "дешевле содержать флот — плата за дальнюю экспансию";
        case sim::Building::ShieldGenerator:
            return "растягивает осаду ЭТОЙ планеты, а не поднимает оборону";
        case sim::Building::Drydock:
            return "чинит ваш флот в бою здесь, открывает титанов";
        case sim::Building::Habitat:
            return "+2 слота застройки на этой планете";
        case sim::Building::Garrison:
            return "оборона возвращается втрое быстрее после осады";
        default:                        return "";
    }
}

const char* hullName(uint8_t hull) {
    switch (sim::Hull(hull)) {
        case sim::Hull::Corvette:   return "корвет";
        case sim::Hull::Tender:     return "тендер";
        case sim::Hull::Destroyer:  return "эсминец";
        case sim::Hull::Carrier:    return "носитель";
        case sim::Hull::Cruiser:    return "крейсер";
        case sim::Hull::Monitor:    return "монитор";
        case sim::Hull::Battleship: return "линкор";
        case sim::Hull::Titan:      return "титан";
        default:                    return "?";
    }
}

const char* hullNameAccusative(uint8_t hull) {
    switch (sim::Hull(hull)) {
        case sim::Hull::Corvette:   return "корвет";
        case sim::Hull::Tender:     return "тендер";
        case sim::Hull::Destroyer:  return "эсминец";
        case sim::Hull::Carrier:    return "носитель";
        case sim::Hull::Cruiser:    return "крейсер";
        case sim::Hull::Monitor:    return "монитор";
        case sim::Hull::Battleship: return "линкор";
        case sim::Hull::Titan:      return "титан";
        default:                    return "?";
    }
}

const char* hullHint(uint8_t hull) {
    switch (sim::Hull(hull)) {
        case sim::Hull::Corvette:
            return "дёшев и быстр, принимает потери на себя";
        case sim::Hull::Tender:
            return "не стреляет, но весь отряд теряет меньше кораблей";
        case sim::Hull::Destroyer:
            return "рабочая лошадь линии";
        case sim::Hull::Carrier:
            return "машины бьют с любой дистанции, но их сбивает ПРО";
        case sim::Hull::Cruiser:
            return "тяжёлый корабль линии";
        case sim::Hull::Monitor:
            return "в бою посредственен, осаду ломает вдвое быстрее";
        case sim::Hull::Battleship:
            return "концентрация огня в одной цели";
        case sim::Hull::Titan:
            return "венец сезона: нужна верфь И ремонтный док";
        default:
            return "";
    }
}

const char* hullIcon(uint8_t hull) {
    switch (sim::Hull(hull)) {
        case sim::Hull::Corvette:   return "hull_corvette";
        case sim::Hull::Tender:     return "hull_tender";
        case sim::Hull::Destroyer:  return "hull_destroyer";
        case sim::Hull::Carrier:    return "hull_carrier";
        case sim::Hull::Cruiser:    return "hull_cruiser";
        case sim::Hull::Monitor:    return "hull_monitor";
        case sim::Hull::Battleship: return "hull_battleship";
        case sim::Hull::Titan:      return "hull_titan";
        default:                    return nullptr;
    }
}

const char* starName(uint8_t starClass) {
    switch (sim::StarClass(starClass)) {
        case sim::StarClass::Red:       return "красный карлик";
        case sim::StarClass::Yellow:    return "жёлтая звезда";
        case sim::StarClass::Blue:      return "голубой гигант";
        case sim::StarClass::Neutron:   return "нейтронная";
        case sim::StarClass::BlackHole: return "чёрная дыра";
        default:                        return "?";
    }
}

const char* planetClassName(uint8_t planetClass) {
    switch (sim::PlanetClass(planetClass)) {
        case sim::PlanetClass::Barren:       return "выжженная";
        case sim::PlanetClass::Desert:       return "пустынная";
        case sim::PlanetClass::Ocean:        return "океаническая";
        case sim::PlanetClass::Volcanic:     return "вулканическая";
        case sim::PlanetClass::GasGiant:     return "газовый гигант";
        case sim::PlanetClass::AsteroidBelt: return "пояс астероидов";
        case sim::PlanetClass::Station:      return "станция";
        default:                             return "?";
    }
}

const char* starHint(uint8_t starClass) {
    switch (sim::StarClass(starClass)) {
        case sim::StarClass::Red:
            return "красный карлик: планет мало, зато и соседям он неинтересен";
        case sim::StarClass::Yellow:
            return "жёлтая звезда: обычная система, планет до шести";
        case sim::StarClass::Blue:
            return "голубой гигант: много планет и много слотов — за такие воюют";
        case sim::StarClass::Neutron:
            return "нейтронная: планет мало, но система редкая";
        case sim::StarClass::BlackHole:
            return "чёрная дыра: планет нет, только станция — но станция стоит дорого";
        default:
            return "";
    }
}

/// Что сказать про планету при наведении.
///
/// НЕ ПОВТОРЯТЬ ТО, ЧТО ВИДНО. Длину полосы обороны игрок видит и так;
/// подсказка отвечает на другой вопрос — что это значит и что теперь
/// делать. Подсказка, дублирующая надпись рядом, — это шум, который учит
/// игрока не наводить курсор вообще.
std::string planetHint(const game::Client::PlanetInfo& planet, uint8_t mine) {
    if (planet.siegeEmpire != 0xFF) {
        return "идёт осада: " + number(planet.siegeProgress) +
               "% обороны сбито · приведите флот или потеряете планету";
    }
    if (planet.owner == 0xFF) {
        return std::string(planetClassName(planet.planetClass)) + " · ничья · слотов " +
               number(planet.slots) + " · приведите сюда флот, и она станет вашей";
    }
    if (planet.owner != mine) {
        return std::string(planetClassName(planet.planetClass)) + " · чужая · слотов " +
               number(planet.slots) + " · оборона " + number(planet.readiness) +
               "% — её надо сбить флотом, прежде чем планета перейдёт к вам";
    }
    if (planet.building()) {
        return std::string("строится ") + buildingNameAccusative(planet.buildBuilding) +
               ", готово на " + number(planet.buildPercent) + "% · оборона " +
               number(planet.readiness) + "%";
    }
    return std::string("ваша · слотов ") + number(planet.slots) + ", свободно " +
           number(planet.freeSlots()) + " · оборона " + number(planet.readiness) +
           "% из 100 — упадёт до нуля, и планету заберут";
}

const char* stageHint(sim::SeasonStage stage) {
    switch (stage) {
        case sim::SeasonStage::Expansion:
            return "Расширение: чужие дома неприкосновенны, занимайте ничьи планеты";
        case sim::SeasonStage::Conflict:
            return "Конфликт: воевать можно со всеми и везде";
        case sim::SeasonStage::Crisis:
            return "Кризис: из ядра галактики идёт общий враг, и чем вы крупнее, тем сильнее";
        case sim::SeasonStage::Final:
            return "Финал: захваты заморожены, считается престиж";
        default:
            return "";
    }
}

std::string noticeText(game::NoticeKind kind) {
    switch (kind) {
        case game::NoticeKind::SystemCaptured: return "система захвачена";
        case game::NoticeKind::SystemLost:     return "система потеряна";
        case game::NoticeKind::BattleWon:      return "бой выигран";
        case game::NoticeKind::BattleLost:     return "бой проигран";
        case game::NoticeKind::BattleDraw:     return "бой без победителя";
        case game::NoticeKind::FleetDestroyed: return "флот уничтожен";
        case game::NoticeKind::OrderRejected:  return "приказ отвергнут";
        case game::NoticeKind::PlanetSieged:   return "ОСАДА ПЛАНЕТЫ";
        case game::NoticeKind::PlanetLost:     return "планета потеряна";
        case game::NoticeKind::PlanetCaptured: return "планета взята";
        default:                               return "";
    }
}

const char* noticeIcon(game::NoticeKind kind) {
    switch (kind) {
        case game::NoticeKind::PlanetSieged:   return "icon_siege";
        case game::NoticeKind::PlanetLost:
        case game::NoticeKind::SystemLost:
        case game::NoticeKind::PlanetCaptured:
        case game::NoticeKind::SystemCaptured: return "icon_planet";
        case game::NoticeKind::BattleWon:
        case game::NoticeKind::BattleLost:
        case game::NoticeKind::BattleDraw:
        case game::NoticeKind::FleetDestroyed: return "icon_fleet";
        case game::NoticeKind::OrderRejected:  return "icon_close";
        default:                               return nullptr;
    }
}

bool noticeIsBad(game::NoticeKind kind) {
    switch (kind) {
        case game::NoticeKind::SystemLost:
        case game::NoticeKind::PlanetLost:
        case game::NoticeKind::PlanetSieged:
        case game::NoticeKind::BattleLost:
        case game::NoticeKind::BattleDraw:
        case game::NoticeKind::FleetDestroyed:
        case game::NoticeKind::OrderRejected:  return true;
        default:                               return false;
    }
}

// ---------------------------------------------------------------------------
// Журнал
// ---------------------------------------------------------------------------

void MessageLog::add(const std::string& text, const TextColor& color, int64_t now,
                     uint32_t system, const char* icon) {
    // Повтор не плодит строку, а добавляет счётчик и оживляет старую.
    // Захват системы из четырёх планет иначе выдаёт четыре одинаковых
    // сообщения и вытесняет из журнала всё остальное — в том числе то,
    // ради чего игрок туда смотрит.
    if (!entries_.empty() && entries_.back().text == text &&
        entries_.back().system == system) {
        ++entries_.back().count;
        entries_.back().bornAt = now;
        return;
    }
    entries_.push_back(Entry{text, color, now, system, icon, 1});
    while (entries_.size() > kMaxVisible) entries_.erase(entries_.begin());
}

void MessageLog::update(int64_t now) {
    while (!entries_.empty() && now - entries_.front().bornAt > kLifetime) {
        entries_.erase(entries_.begin());
    }
}


// ---------------------------------------------------------------------------
// Столбец
// ---------------------------------------------------------------------------

namespace {

/// Раскладка панели: сначала объявляем, из чего она состоит, потом
/// разливаем строки сверху вниз.
///
/// ЗАЧЕМ. В немедленном режиме рамка рисуется ДО содержимого, а значит её
/// высоту приходится знать заранее. Когда высоту панели и раскладку её
/// содержимого считают порознь, однажды правят одно и забывают другое —
/// и содержимое вылезает за рамку. Ровно это и случилось в первой сборке:
/// кнопка «Открыть систему» легла поверх последней планеты, а строка
/// четвёртой планеты оказалась обрезана. Здесь такое невозможно
/// по построению: высота панели — это сумма ТЕХ ЖЕ чисел, которыми потом
/// раскладываются строки.
class Column {
public:
    explicit Column(float pad) : pad_(pad) {}

    /// Объявить строку высотой `height` и отступ `gap` после неё.
    void row(float height, float gap = 0.0f) {
        rows_.push_back(Row{height, gap});
        content_ += height + gap;
    }
    /// Высота панели вместе с полями.
    float height() const { return content_ + pad_ * 2.0f; }

    /// Начать разлив внутри панели.
    void place(const Rect& panel) {
        x_ = panel.x + pad_;
        y_ = panel.y + pad_;
        width_ = panel.w - pad_ * 2.0f;
        cursor_ = 0;
    }
    /// Следующая объявленная строка.
    Rect next() {
        if (cursor_ >= rows_.size()) return Rect{x_, y_, width_, 0.0f};
        const Row row = rows_[cursor_++];
        const Rect out{x_, y_, width_, row.height};
        y_ += row.height + row.gap;
        return out;
    }

private:
    struct Row {
        float height;
        float gap;
    };
    float pad_;
    float content_ = 0.0f;
    std::vector<Row> rows_;
    size_t cursor_ = 0;
    float x_ = 0.0f, y_ = 0.0f, width_ = 0.0f;
};

/// Прямоугольник, сдвинутый вниз до базовой линии текста.
///
/// Надпись рисуется от левого верхнего угла, а строки в панели заданы
/// как полосы. Без этой поправки текст липнет к верхней границе полосы,
/// и строки идут неровно — глаз это ловит, даже не понимая, что не так.
Rect textLine(const Rect& r, float lineHeight) {
    return Rect{r.x, r.y + (r.h - lineHeight) * 0.5f, r.w, lineHeight};
}

/// Цвет владельца: свой — зелёный, чужой — красный, ничей — серый.
TextColor ownerTint(const UiTheme& theme, uint8_t owner, uint8_t mine) {
    if (owner == 0xFF) return theme.textDim;
    return owner == mine ? theme.textGood : theme.textBad;
}

/// Цвет полосы по её заполненности.
TextColor barTint(float value) {
    if (value > 0.6f) return TextColor{0.42f, 0.72f, 0.52f, 1.0f};
    if (value > 0.3f) return TextColor{0.85f, 0.66f, 0.28f, 1.0f};
    return TextColor{0.82f, 0.34f, 0.34f, 1.0f};
}

}  // namespace

// ---------------------------------------------------------------------------
// Верхняя панель: ресурсы
// ---------------------------------------------------------------------------

ScreenAction Screen::topBar(Ui& ui, const game::Client& client) const {
    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const float height = line + unit * 2.0f;
    const Rect bar{0.0f, 0.0f, float(ui.screenWidth()), height};
    ui.panel(bar, "panel");

    const auto& empire = client.view().empire;
    struct Entry {
        const char* icon;
        const char* name;
        int64_t value;
    };
    // Подсказка отвечает на «зачем оно мне», а не называет ресурс.
    // Название игрок и так угадает по значку со второго раза; чего он
    // не угадает никогда — на что этот ресурс тратится и откуда берётся.
    const Entry entries[] = {
        {"res_minerals",
         "МИНЕРАЛЫ · платят за здания · дают шахты",
         empire.minerals.floorToInt()},
        {"res_alloys",
         "СПЛАВЫ · платят за корабли · дают ТОЛЬКО литейные, из минералов",
         empire.alloys.floorToInt()},
        {"res_energy",
         "ЭНЕРГИЯ · уходит на содержание зданий и флота · дают электростанции",
         empire.energy.floorToInt()},
        {"res_research",
         "ИССЛЕДОВАНИЯ · счёт науки в престиже · дают лаборатории",
         empire.research.floorToInt()},
        {"res_influence",
         "ВЛИЯНИЕ · счёт дипломатии в престиже · дают торговые узлы",
         empire.influence.floorToInt()},
    };

    const float iconSize = line * 1.25f;
    float x = unit * 1.5f;
    for (size_t index = 0; index < sizeof(entries) / sizeof(entries[0]); ++index) {
        const Entry& entry = entries[index];
        const std::string value = grouped(entry.value);
        const float width = iconSize + unit * 0.6f + ui.textWidth(value);

        const Rect cell{x, unit * 0.5f, width, height - unit};
        // Наведение объясняет, что это за число. Иконка узнаётся быстрее
        // подписи, но узнаётся не с первого раза — подсказка закрывает
        // именно первый раз.
        if (ui.hotspot(uiId("res", uint32_t(index)), cell).hovered) {
            ui.tooltip(entry.name);
        }

        ui.icon(Rect{x, (height - iconSize) * 0.5f, iconSize, iconSize}, entry.icon);
        ui.text(x + iconSize + unit * 0.6f, (height - line) * 0.5f, value,
                ui.theme().text);
        x += width + unit * 2.0f;
    }

    // --- стадия сезона ---
    //
    // По центру верхней полосы, а не в углу: это единственное число
    // на экране, которое одинаково у всех игроков сервера, и вокруг него
    // строится вся подготовка. «До Конфликта сорок минут» меняет то,
    // что игрок строит прямо сейчас, сильнее любой другой цифры здесь.
    const auto stage = sim::SeasonStage(empire.stage);
    const TextColor stageColor = stage == sim::SeasonStage::Expansion ? ui.theme().textGood
                                 : stage == sim::SeasonStage::Crisis  ? ui.theme().textBad
                                 : stage == sim::SeasonStage::Final   ? ui.theme().textWarn
                                                                      : ui.theme().textAccent;
    const std::string stageText =
        std::string(sim::stageName(stage)) +
        (empire.stageSecondsLeft > 0 ? " · " + duration(empire.stageSecondsLeft)
                                     : std::string());
    const float stageWidth = ui.textWidth(stageText);
    const Rect stageBox{(float(ui.screenWidth()) - stageWidth) * 0.5f, 0.0f, stageWidth,
                        height};
    ui.textCentered(stageBox, stageText, stageColor);
    if (ui.hotspot(uiId("season"), stageBox.inset(-unit)).hovered) {
        ui.tooltip(stageHint(stage));
    }

    // Связь. Показывается всегда: в MMO игрок обязан отличать «сервер
    // тормозит» от «я плохо играю».
    const std::string link = number(client.roundTrip()) + " мс · потери " +
                             number(client.lossPercent()) + "%";
    const TextColor quality = client.lossPercent() > 15  ? ui.theme().textBad
                              : client.lossPercent() > 5 ? ui.theme().textWarn
                                                         : ui.theme().textDim;
    ui.textRight(Rect{0.0f, 0.0f, float(ui.screenWidth()) - unit * 1.5f, height}, link,
                 quality);
    return {};
}

// ---------------------------------------------------------------------------
// Панель системы: карточки планет
// ---------------------------------------------------------------------------

ScreenAction Screen::systemPanel(Ui& ui, const game::Client& client,
                                 const ScreenState& state, float top, float width,
                                 float& bottom) const {
    ScreenAction action;
    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const Rect at{unit * 1.5f, top, width, 0.0f};

    // Система не выбрана — панель всё равно есть, но объясняет, что делать.
    // Пустой угол экрана человек читает как «здесь ничего не бывает»
    // и перестаёт туда смотреть.
    if (state.system >= client.galaxy().systemCount()) {
        Column column(unit);
        column.row(line * 1.3f, unit * 0.4f);
        column.row(line * 1.3f);

        const Rect panel{at.x, at.y, width, column.height()};
        ui.panel(panel, "panel");
        column.place(panel);

        const Rect titleRow = column.next();
        ui.text(titleRow.x, titleRow.y, "Система не выбрана", ui.theme().text);
        const Rect hintRow = column.next();
        ui.text(hintRow.x, hintRow.y, "щёлкните звезду на карте", ui.theme().textDim);

        bottom = panel.bottom();
        return action;
    }

    const float row = line * 2.4f;
    const auto planets = client.planetsAt(state.system);
    const auto& view = client.view().systems[state.system];
    const uint8_t mine = uint8_t(client.empire() & 0xFFu);

    Column column(unit);
    column.row(line * 1.25f);                        // «Система N»
    column.row(line * 1.25f, unit * 0.8f);           // класс звезды и владелец
    for (size_t index = 0; index < planets.size(); ++index) column.row(row);
    column.row(line * 2.0f, 0.0f);                   // войти или вернуться
    // Отступ перед кнопкой объявляем отдельно, чтобы он не прилипал
    // к последней планете, когда планет ноль.
    const Rect panel{at.x, at.y, width, column.height() + unit * 0.6f};
    ui.panel(panel, "panel");
    column.place(panel);

    // --- шапка ---
    const Rect titleRow = column.next();
    ui.text(titleRow.x, titleRow.y, "Система " + number(state.system), ui.theme().text);

    const char* ownership = view.owner == 0xFF     ? "ничья"
                            : view.owner == mine   ? "ваша"
                                                   : "чужая";
    const std::string counter =
        number(view.ownedPlanets) + " / " + number(view.totalPlanets);
    const Rect counterBox{titleRow.right() - ui.textWidth(counter) - unit * 0.5f,
                          titleRow.y, ui.textWidth(counter) + unit, titleRow.h};
    ui.textRight(textLine(titleRow, line), counter,
                 ownerTint(ui.theme(), view.owner, mine));
    if (ui.hotspot(uiId("system-count"), counterBox).hovered) {
        // «4 / 4» само по себе не значит ничего: игрок обязан узнать,
        // что это, ровно один раз и больше не вспоминать.
        ui.tooltip("ваших планет в системе " + number(view.ownedPlanets) + " из " +
                   number(view.totalPlanets) +
                   " · система принадлежит тому, у кого их больше");
    }

    const Rect subRow = column.next();
    const std::string subtitle =
        std::string(starName(client.galaxy().starClass(state.system))) + " · " + ownership;
    ui.text(subRow.x, subRow.y, subtitle, ui.theme().textDim);
    if (ui.hotspot(uiId("system-star"),
                   Rect{subRow.x, subRow.y, ui.textWidth(subtitle), subRow.h})
            .hovered) {
        ui.tooltip(starHint(client.galaxy().starClass(state.system)));
    }

    // --- строки планет ---
    //
    // Строка — это кнопка. Игрок выбирает планету щелчком по ней,
    // а не перебором клавишей: перебор требует помнить, где ты сейчас,
    // и на шести планетах это уже усилие.
    for (size_t index = 0; index < planets.size(); ++index) {
        const auto& planet = planets[index];
        const bool selected = index == state.planetIndex;
        const Rect box = column.next();
        const Rect card{box.x, box.y, box.w, box.h - 2.0f};

        // Тонкая линия между строками. Глаз группирует то, что разделено,
        // охотнее того, что просто расставлено с отступами: без линии
        // список планет висел в пустоте.
        if (index > 0) ui.separator(Rect{card.x + unit, card.y - 1.0f, card.w - unit * 2.0f, 1.0f});

        if (selected) ui.panel(card, "button_accent", 0.55f);
        const ButtonResult hit = ui.hotspot(uiId("planet-row", uint32_t(index)), card);
        if (hit.hovered && !selected) ui.panel(card, "slot_hover", 0.5f);
        if (hit.clicked) {
            action.kind = ActionKind::SelectPlanet;
            action.value = uint32_t(index);
        }

        const float iconSize = card.h * 0.62f;
        const EmpireColor& empire = empireColor(planet.owner);
        ui.icon(Rect{card.x + unit * 0.5f, card.y + (card.h - iconSize) * 0.5f, iconSize,
                     iconSize},
                "icon_planet",
                planet.owner == 0xFF ? TextColor{0.55f, 0.58f, 0.63f, 1.0f}
                                     : TextColor{empire.r, empire.g, empire.b, 1.0f});

        // Полоса обороны справа. Её ширину резервируем ДО текста, иначе
        // длинное название планеты залезет под полосу.
        const float barWidth = unit * 5.0f;
        const float textX = card.x + unit * 1.0f + iconSize;

        // Имя планеты ярче её состояния ВСЕГДА, а не только у выбранной.
        // Первая версия гасила всю невыбранную строку целиком, и список
        // из шести планет читался как шесть одинаковых серых полос.
        ui.text(textX, card.y + unit * 0.15f,
                number(int64_t(index) + 1) + ". " + planetClassName(planet.planetClass),
                ui.theme().text);

        // Вторая строка карточки — состояние: что строится, идёт ли осада,
        // цела ли оборона. Именно это решает, стоит ли сюда лезть.
        std::string status;
        TextColor statusColor = ui.theme().textDim;
        if (planet.siegeEmpire != 0xFF) {
            status = "ОСАДА " + number(planet.siegeProgress) + "%";
            statusColor = ui.theme().textBad;
        } else if (planet.building()) {
            status = std::string(planet.buildPaid != 0 ? "строится " : "ждёт минералов: ") +
                     buildingName(planet.buildBuilding);
            if (planet.buildPaid != 0) status += " " + number(planet.buildPercent) + "%";
            statusColor = planet.buildPaid != 0 ? ui.theme().textGood : ui.theme().textWarn;
        } else if (planet.owner == mine) {
            status = "свободно слотов " + number(planet.freeSlots()) + " из " +
                     number(planet.slots);
        } else {
            status = planet.owner == 0xFF ? "ничья" : "чужая";
        }
        ui.text(textX, card.y + unit * 0.15f + line * 1.0f, status, statusColor);

        if (planet.owner != 0xFF) {
            const Rect bar{card.right() - unit * 0.8f - barWidth,
                           card.y + card.h * 0.5f - 3.0f, barWidth, 6.0f};
            const float value = float(planet.readiness) / 100.0f;
            ui.progress(bar, value, barTint(value));
        }

        // ПОДСКАЗКА ОБЪЯСНЯЕТ, А НЕ ПОВТОРЯЕТ. «Оборона 78%» игрок и так
        // видит по длине полосы; знать ему надо, ЧТО ЭТО ЗНАЧИТ и что
        // с этим делать.
        if (hit.hovered) ui.tooltip(planetHint(planet, mine));
    }

    // --- вход в систему ---
    const Rect enter = column.next();
    if (state.inSystem) {
        if (ui.iconButton(uiId("leave-system"), enter, "icon_back", "К карте галактики")) {
            action.kind = ActionKind::LeaveSystem;
        }
    } else if (ui.iconButton(uiId("enter-system"), enter, "icon_enter",
                             "Открыть систему", ButtonStyle::Accent)) {
        action.kind = ActionKind::EnterSystem;
    }

    bottom = panel.bottom();
    return action;
}

// ---------------------------------------------------------------------------
// Панель планеты: слоты и палитра застройки
// ---------------------------------------------------------------------------

ScreenAction Screen::planetPanel(Ui& ui, const game::Client& client,
                                 const ScreenState& state, float top, float width,
                                 float& bottom) const {
    ScreenAction action;
    bottom = top;
    if (state.system >= client.galaxy().systemCount()) return action;

    const auto planets = client.planetsAt(state.system);
    if (state.planetIndex >= planets.size()) return action;
    const auto& planet = planets[state.planetIndex];

    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const uint8_t mine = uint8_t(client.empire() & 0xFFu);
    const bool own = planet.owner == mine;

    // Палитра открывается только когда выбран пустой слот: восемь кнопок,
    // висящих всё время, — это восемь кнопок, которые игрок перестаёт
    // замечать.
    const bool slotPicked = own && state.slot != kNoSlot && state.slot < planet.slots &&
                            !(planet.building() && planet.buildSlot == state.slot);
    const bool paletteOpen =
        slotPicked && planet.buildings[state.slot] == uint8_t(sim::Building::None);
    // Занятый слот показывает не палитру, а само здание и кнопку сноса.
    // Крестик в углу ячейки был шириной в шестнадцать пикселей: на нём
    // не читался ни значок, ни то, что это вообще кнопка — он выглядел
    // красной кляксой поверх иконки.
    const bool detailOpen = slotPicked && !paletteOpen;

    const uint8_t slots = std::min<uint8_t>(planet.slots, sim::kMaxSlots);
    const float gridPad = unit * 0.5f;
    const int columns = std::max(1, std::min(6, int(slots)));
    const int gridRows = (int(slots) + columns - 1) / columns;
    // Ячейка вычисляется из ШИРИНЫ ПАНЕЛИ, а не наоборот: панель стоит
    // в столбце и её ширину задаёт столбец. Сетка обязана в неё влезть.
    const float cell =
        std::min(line * 2.7f,
                 (width - unit * 2.0f - gridPad * float(columns - 1)) / float(columns));

    const int paletteColumns = 2;
    const int paletteRows =
        (int(sim::Building::Count) - 1 + paletteColumns - 1) / paletteColumns;
    const float paletteRowHeight = line * 2.1f;

    Column column(unit);
    column.row(line * 1.25f);                          // «Планета K · класс»
    column.row(line * 1.35f, unit * 0.7f);             // состояние
    for (int r = 0; r < gridRows; ++r) column.row(cell, r + 1 < gridRows ? gridPad : 0.0f);
    if (paletteOpen) {
        column.row(line * 1.6f, unit * 0.2f);          // «Что построить»
        for (int r = 0; r < paletteRows; ++r) {
            column.row(paletteRowHeight - gridPad, r + 1 < paletteRows ? gridPad : 0.0f);
        }
    } else if (detailOpen) {
        column.row(line * 1.5f, unit * 0.1f);          // здание и чем полезно
        column.row(line * 2.0f);                       // «Снести»
    }

    const Rect panel{unit * 1.5f, top, width, column.height()};
    ui.panel(panel, "panel");
    column.place(panel);

    // --- шапка ---
    const Rect titleRow = column.next();
    ui.text(titleRow.x, titleRow.y,
            "Планета " + number(int64_t(state.planetIndex) + 1) + " · " +
                planetClassName(planet.planetClass),
            ui.theme().text);

    // --- состояние ---
    const Rect statusRow = column.next();
    if (!own) {
        ui.text(statusRow.x, statusRow.y + (statusRow.h - line) * 0.5f,
                planet.owner == 0xFF ? "ничья — приведите сюда флот"
                                     : "чужая — сначала возьмите её",
                ui.theme().textDim);
    } else if (planet.building()) {
        const float cancelWidth = line * 4.6f;
        const Rect cancel{statusRow.right() - cancelWidth, statusRow.y, cancelWidth,
                          statusRow.h};
        if (ui.button(uiId("cancel-build"), cancel, "отменить", ButtonStyle::Danger)) {
            action.kind = ActionKind::CancelBuild;
            action.planet = planet.id;
        }

        const std::string label =
            std::string(buildingName(planet.buildBuilding)) +
            (planet.buildQueued > 0 ? " +" + number(planet.buildQueued) : std::string());
        ui.text(statusRow.x, statusRow.y,
                label + (planet.buildPaid != 0 ? " " + number(planet.buildPercent) + "%"
                                               : " — ждёт минералов"),
                planet.buildPaid != 0 ? ui.theme().textGood : ui.theme().textWarn);

        const Rect bar{statusRow.x, statusRow.bottom() - line * 0.45f,
                       cancel.x - statusRow.x - unit, line * 0.35f};
        ui.progress(bar, float(planet.buildPercent) / 100.0f,
                    planet.buildPaid != 0 ? TextColor{0.42f, 0.72f, 0.52f, 1.0f}
                                          : TextColor{0.85f, 0.66f, 0.28f, 1.0f});
    } else if (slotPicked) {
        // Слот уже выбран — звать щёлкать по слоту незачем: подсказка,
        // спорящая с тем, что игрок только что сделал, читается как
        // сбой, а не как совет.
        ui.text(statusRow.x, statusRow.y + (statusRow.h - line) * 0.5f,
                "слот " + number(int64_t(state.slot) + 1) + " из " +
                    number(planet.slots) + " · свободно " + number(planet.freeSlots()),
                ui.theme().textDim);
    } else {
        ui.text(statusRow.x, statusRow.y + (statusRow.h - line) * 0.5f,
                planet.freeSlots() > 0
                    ? "щёлкните пустой слот, чтобы построить"
                    : "все слоты заняты — сносите или стройте на другой",
                planet.freeSlots() > 0 ? ui.theme().textAccent : ui.theme().textDim);
    }

    // --- сетка слотов ---
    for (int gridRow = 0; gridRow < gridRows; ++gridRow) {
        const Rect strip = column.next();
        for (int col = 0; col < columns; ++col) {
            const int index = gridRow * columns + col;
            if (index >= int(slots)) break;
            const uint8_t slot = uint8_t(index);
            const Rect box{strip.x + float(col) * (cell + gridPad), strip.y, cell, cell};

            const uint8_t building = planet.buildings[slot];
            const bool constructing = planet.building() && planet.buildSlot == slot;
            const bool empty = !constructing && building == uint8_t(sim::Building::None);
            // У пустой ячейки — плюс. Без него шесть тёмных квадратов
            // читаются как рамка, а не как «сюда можно нажать»: игрок
            // видит место под здание, только когда ему это СКАЗАЛИ.
            const char* sprite = constructing ? buildingIcon(planet.buildBuilding)
                                 : empty      ? nullptr
                                              : buildingIcon(building);

            const ButtonResult hit =
                ui.slot(uiId("slot", slot), box, sprite, state.slot == slot, own);

            // Плюс в пустой ячейке — приглашение, а не заголовок: в полную
            // величину пять плюсов подряд кричат громче самих построек,
            // ради которых игрок сюда смотрит.
            if (empty && own) {
                // Плюс приглушён, но ВИДЕН. В прошлой правке я увёл его
                // в 0.55 прозрачности, и на снимке он почти исчез: игрок
                // видел шесть тёмных квадратов, то есть ровно то, от чего
                // плюс и должен был спасти.
                const float mark = cell * 0.34f;
                ui.icon(Rect{box.x + (cell - mark) * 0.5f, box.y + (cell - mark) * 0.5f,
                             mark, mark},
                        "icon_plus",
                        hit.hovered ? TextColor{0.80f, 0.90f, 1.00f, 1.0f}
                                    : TextColor{0.60f, 0.70f, 0.84f, 0.80f});
            }
            if (hit.clicked) {
                action.kind = ActionKind::SelectSlot;
                action.slot = state.slot == slot ? kNoSlot : slot;
            }
            if (hit.hovered) {
                if (constructing) {
                    ui.tooltip(std::string("строится ") +
                               buildingName(planet.buildBuilding) + " · " +
                               number(planet.buildPercent) + "%");
                } else if (!empty) {
                    ui.tooltip(std::string(buildingName(building)) + " · " +
                               buildingHint(building));
                } else if (own) {
                    ui.tooltip("пустой слот — щёлкните, чтобы выбрать постройку");
                }
            }

            // Полоса хода стройки прямо в ячейке: игрок смотрит на слот,
            // а не на подпись рядом, и ответ обязан быть там же.
            if (constructing) {
                const Rect bar{box.x + 3.0f, box.bottom() - 6.0f, box.w - 6.0f, 3.0f};
                ui.progress(bar, float(planet.buildPercent) / 100.0f,
                            planet.buildPaid != 0
                                ? TextColor{0.42f, 0.72f, 0.52f, 1.0f}
                                : TextColor{0.85f, 0.66f, 0.28f, 1.0f});
            }

        }
    }

    // --- палитра застройки ---
    if (paletteOpen) {
        const Rect head = column.next();
        ui.text(head.x, head.y + (head.h - line) * 0.5f,
                "Что построить в слоте " + number(int64_t(state.slot) + 1),
                ui.theme().textAccent);

        const int64_t minerals = client.view().empire.minerals.floorToInt();
        for (int paletteRow = 0; paletteRow < paletteRows; ++paletteRow) {
            const Rect strip = column.next();
            const float buttonWidth = (strip.w - gridPad) * 0.5f;
            for (int col = 0; col < paletteColumns; ++col) {
                const int index = paletteRow * paletteColumns + col;
                if (index >= int(sim::Building::Count) - 1) break;
                const uint8_t building = uint8_t(index + 1);
                const Rect box{strip.x + float(col) * (buttonWidth + gridPad), strip.y,
                               buttonWidth, strip.h};

                const int64_t cost = int64_t(sim::buildingCost(sim::Building(building)));
                const bool affordable = minerals >= cost;
                const ButtonResult hit = ui.iconButton(
                    uiId("build", building), box, buildingIcon(building),
                    buildingName(building),
                    affordable ? ButtonStyle::Normal : ButtonStyle::Quiet, affordable);
                if (hit.hovered) {
                    ui.tooltip(std::string(buildingHint(building)) + " · " +
                               number(cost) + " минералов · " +
                               duration(cost * sim::kBuildSecondsPerMineral) +
                               (affordable ? "" : " · не хватает минералов"));
                }
                if (hit.clicked) {
                    action.kind = ActionKind::Build;
                    action.value = building;
                    action.planet = planet.id;
                    action.slot = state.slot;
                }
            }
        }
    }

    // --- выбранное здание: что это и как снести ---
    if (detailOpen) {
        const uint8_t building = planet.buildings[state.slot];
        const Rect what = column.next();
        ui.text(what.x, what.y + (what.h - line) * 0.5f,
                std::string(buildingName(building)) + " — " + buildingHint(building),
                ui.theme().textDim);

        // Снос — кнопка во всю ширину и с прямой надписью, а не значок
        // в углу ячейки. Действие необратимо, и оно обязано выглядеть так,
        // чтобы на него не нажали мимоходом.
        const Rect kill = column.next();
        const ButtonResult hit = ui.iconButton(uiId("demolish", state.slot), kill,
                                               "icon_demolish",
                                               "Снести " + std::string(buildingNameAccusative(building)),
                                               ButtonStyle::Danger);
        if (hit.hovered) ui.tooltip("здание пропадёт, минералы не вернутся");
        if (hit.clicked) {
            action.kind = ActionKind::Demolish;
            action.planet = planet.id;
            action.slot = state.slot;
        }
    }

    bottom = panel.bottom();
    return action;
}

// ---------------------------------------------------------------------------
// Панель флота: состав и приказы
// ---------------------------------------------------------------------------

ScreenAction Screen::fleetPanel(Ui& ui, const game::Client& client,
                                const ScreenState& state, float top, float width) const {
    ScreenAction action;
    if (state.system >= client.galaxy().systemCount()) return action;

    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const auto fleets = client.fleetsAt(state.system);

    // Верфь считается по СВОИМ планетам системы: без неё заказ корабля
    // будет отвергнут, и игрок обязан видеть это ДО нажатия, а не после.
    const uint8_t mine = uint8_t(client.empire() & 0xFFu);
    uint32_t shipyards = 0;
    for (const auto& planet : client.planetsAt(state.system)) {
        if (planet.owner != mine) continue;
        for (uint8_t slot = 0; slot < planet.slots && slot < sim::kMaxSlots; ++slot) {
            if (planet.buildings[slot] == uint8_t(sim::Building::Shipyard)) ++shipyards;
        }
    }

    const float fleetRow = line * 2.2f;
    const int hullRows = (int(sim::Hull::Count) - 1 + 1) / 2;
    const float hullRowHeight = line * 2.0f;

    Column column(unit);
    column.row(line * 1.25f, unit * 0.6f);            // «Флот в системе»
    if (fleets.empty()) {
        column.row(line * 1.3f, unit * 0.8f);
    } else {
        for (size_t i = 0; i < fleets.size(); ++i) column.row(fleetRow);
        column.row(line * 2.0f, unit * 0.8f);         // «Отправить флот»
    }
    column.row(line * 1.25f, unit * 0.5f);            // «Заказать корабль»
    if (shipyards == 0) {
        column.row(line * 1.9f);
    } else {
        for (int r = 0; r < hullRows; ++r) {
            column.row(hullRowHeight, r + 1 < hullRows ? unit * 0.5f : 0.0f);
        }
    }

    const Rect panel{float(ui.screenWidth()) - width - unit * 1.5f, top, width,
                     column.height()};
    ui.panel(panel, "panel");
    column.place(panel);

    const Rect titleRow = column.next();
    ui.text(titleRow.x, titleRow.y, "Флот в системе", ui.theme().text);

    if (fleets.empty()) {
        const Rect empty = column.next();
        ui.text(empty.x, empty.y, "здесь нет ваших кораблей", ui.theme().textDim);
    } else {
        for (size_t index = 0; index < fleets.size(); ++index) {
            const uint32_t id = fleets[index];
            const auto& fleet = client.view().fleets.at(id);
            const bool selected = id == state.fleet;
            const Rect strip = column.next();
            const Rect card{strip.x, strip.y, strip.w, strip.h - 2.0f};

            if (selected) ui.panel(card, "button_accent", 0.55f);
            const ButtonResult hit = ui.hotspot(uiId("fleet-row", uint32_t(index)), card);
            if (hit.hovered && !selected) ui.panel(card, "slot_hover", 0.5f);
            if (hit.clicked) {
                action.kind = ActionKind::SelectFleet;
                action.value = id;
            }

            const float iconSize = card.h * 0.6f;
            ui.icon(Rect{card.x + unit * 0.5f, card.y + (card.h - iconSize) * 0.5f,
                         iconSize, iconSize},
                    "icon_fleet");
            ui.text(card.x + unit + iconSize, card.y + (card.h - line) * 0.5f,
                    number(sim::fleetTonnage(fleet.composition)) + " т",
                    selected ? ui.theme().text : ui.theme().textDim);

            // Состав — значками корпусов, а не строкой «8/2/0/0/…». Восемь
            // чисел через косые игрок обязан расшифровывать каждый раз,
            // а корвет от титана отличает силуэт.
            //
            // Показываем ТРИ САМЫХ МНОГОЧИСЛЕННЫХ класса. Все восемь в узкой
            // строке не помещаются, а список «чего во флоте больше всего»
            // и есть ответ на вопрос, зачем этот отряд собран.
            std::vector<std::pair<uint32_t, uint8_t>> present;
            std::string full;
            for (uint8_t hull = 1; hull < uint8_t(sim::Hull::Count); ++hull) {
                const uint32_t count = fleet.composition[sim::Hull(hull)];
                if (count == 0) continue;
                present.emplace_back(count, hull);
                if (!full.empty()) full += ", ";
                full += std::string(hullName(hull)) + " " + number(count);
            }
            std::sort(present.begin(), present.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

            const float mark = card.h * 0.55f;
            float hx = card.right() - unit * 0.5f;
            const size_t shown = std::min<size_t>(present.size(), 3);
            if (present.size() > shown) {
                const std::string more = "+" + number(int64_t(present.size() - shown));
                hx -= ui.textWidth(more);
                ui.text(hx, card.y + (card.h - line) * 0.5f, more, ui.theme().textDim);
                hx -= unit * 0.7f;
            }
            for (size_t slot = shown; slot-- > 0;) {
                const std::string value = number(present[slot].first);
                hx -= ui.textWidth(value);
                ui.text(hx, card.y + (card.h - line) * 0.5f, value,
                        selected ? ui.theme().text : ui.theme().textDim);
                hx -= mark + unit * 0.15f;
                ui.icon(Rect{hx, card.y + (card.h - mark) * 0.5f, mark, mark},
                        hullIcon(present[slot].second));
                hx -= unit * 0.7f;
            }
            if (hit.hovered && !full.empty()) ui.tooltip(full);
        }

        // Приказ движения: нажали — следующий щелчок по карте задаёт цель.
        // Двухшаговый приказ, а не перетаскивание: перетаскивание требует
        // держать кнопку через полкарты, и на длинном пути его срывает
        // любое дрожание руки.
        const Rect move = column.next();
        const bool armed = state.awaitingMoveTarget;
        const bool picked = state.fleet != 0xFFFFFFFFu;
        const ButtonResult hit =
            ui.iconButton(uiId("order-move"), move, armed ? "icon_close" : "icon_enter",
                          armed ? "Отмена — укажите цель" : "Отправить флот",
                          armed ? ButtonStyle::Danger : ButtonStyle::Accent, picked);
        if (hit.hovered) {
            ui.tooltip(!picked ? "сначала выберите флот в списке выше"
                       : armed  ? "отменить приказ · пока он взведён, щелчок по карте задаёт цель"
                                : "нажмите, потом щёлкните систему на карте — флот пойдёт "
                                  "туда и начнёт занимать её планеты");
        }
        if (hit.clicked) action.kind = armed ? ActionKind::CancelMove : ActionKind::BeginMove;
    }

    // --- заказ кораблей ---
    const Rect orderTitle = column.next();
    ui.text(orderTitle.x, orderTitle.y, "Заказать корабль", ui.theme().text);

    if (shipyards == 0) {
        // Причина — на месте кнопок, а не подписью под ними: игрок ищет
        // ответ там, где ждал действие.
        const Rect box = column.next();
        const ButtonResult hit = ui.iconButton(uiId("no-shipyard"), box, "bld_shipyard",
                                               "нужна верфь", ButtonStyle::Quiet, false);
        if (hit.hovered) {
            ui.tooltip("постройте верфь на своей планете в этой системе");
        }
    } else {
        const int64_t alloys = client.view().empire.alloys.floorToInt();
        for (int hullRow = 0; hullRow < hullRows; ++hullRow) {
            const Rect strip = column.next();
            const float buttonWidth = (strip.w - unit * 0.5f) * 0.5f;
            for (int col = 0; col < 2; ++col) {
                const int index = hullRow * 2 + col;
                if (index >= int(sim::Hull::Count) - 1) break;
                const uint8_t hull = uint8_t(index + 1);
                const Rect box{strip.x + float(col) * (buttonWidth + unit * 0.5f), strip.y,
                               buttonWidth, strip.h};

                const int64_t cost = int64_t(sim::hullCost(sim::Hull(hull)));
                const bool affordable = alloys >= cost;
                const ButtonResult hit = ui.iconButton(
                    uiId("ship", hull), box, hullIcon(hull), hullName(hull),
                    affordable ? ButtonStyle::Normal : ButtonStyle::Quiet, affordable);
                // Имя на кнопке, цена — при наведении. Восемь корпусов с одними
                // числами вместо названий читаются как прайс-лист, а игрок
                // выбирает не цену, а роль.
                if (hit.hovered) {
                    ui.tooltip(std::string(hullHint(hull)) + " · " + number(cost) +
                               " сплавов" + (affordable ? "" : " · не хватает сплавов"));
                }
                if (hit.clicked) {
                    action.kind = ActionKind::OrderShip;
                    action.value = hull;
                }
            }
        }
    }
    return action;
}

// ---------------------------------------------------------------------------
// Журнал событий
// ---------------------------------------------------------------------------

ScreenAction Screen::messagePanel(Ui& ui, int64_t now, float top, float left,
                                  float right) const {
    ScreenAction action;
    if (messages_ == nullptr || messages_->entries().empty()) return action;

    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const float rowHeight = line * 1.7f;

    float widest = line * 12.0f;
    for (const MessageLog::Entry& entry : messages_->entries()) {
        const std::string full =
            entry.count > 1 ? entry.text + "  ×" + number(entry.count) : entry.text;
        widest = std::max(widest, ui.textWidth(full) + line * 3.0f);
    }
    // Между столбцами, а не по центру экрана: журнал, наехавший на панель
    // системы, закрывает именно то, ради чего игрок туда смотрит.
    widest = std::min(widest, std::max(line * 12.0f, right - left - unit * 2.0f));

    // Подложка гаснет вместе с содержимым. Иначе последние секунды жизни
    // журнала выглядят как пустая тёмная коробка посреди экрана: текст уже
    // прозрачный, а рамка ещё нет.
    auto fade = [&](const MessageLog::Entry& entry) {
        const int64_t age = now - entry.bornAt;
        const int64_t fadeFrom = MessageLog::kLifetime * 2 / 3;
        if (now <= 0 || age <= fadeFrom) return 1.0f;
        return std::clamp(float(MessageLog::kLifetime - age) /
                              float(MessageLog::kLifetime - fadeFrom),
                          0.0f, 1.0f);
    };
    float strongest = 0.0f;
    for (const MessageLog::Entry& entry : messages_->entries()) {
        strongest = std::max(strongest, fade(entry));
    }

    const float height = rowHeight * float(messages_->entries().size()) + unit;
    const Rect panel{(left + right - widest) * 0.5f, top, widest, height};
    ui.panel(panel, "panel_dark", 0.85f * strongest);

    float y = panel.y + unit * 0.5f;
    size_t index = 0;
    for (const MessageLog::Entry& entry : messages_->entries()) {
        // Гаснет к концу жизни: угасающая строка не отвлекает, но ещё
        // читается, если игрок обернулся.
        TextColor color = entry.color;
        color.a *= fade(entry);

        const Rect box{panel.x + unit * 0.5f, y, panel.w - unit, rowHeight - 2.0f};
        if (entry.system != kNoSystem) {
            // Новость о том, что где-то идёт осада, бесполезна, если до
            // этого «где-то» надо ещё доскроллить вручную.
            const ButtonResult hit = ui.hotspot(uiId("message", uint32_t(index)), box);
            if (hit.hovered) {
                ui.panel(box, "slot_hover", 0.5f);
                ui.tooltip("щёлкните — камера перейдёт к системе " +
                           number(entry.system) + ", где это случилось");
            }
            if (hit.clicked) {
                action.kind = ActionKind::FocusSystem;
                action.value = entry.system;
            }
        }

        const float iconSize = rowHeight * 0.7f;
        if (entry.icon != nullptr) {
            ui.icon(Rect{box.x + unit * 0.5f, y + (rowHeight - iconSize) * 0.5f, iconSize,
                         iconSize},
                    entry.icon, color);
        }
        ui.text(box.x + unit + iconSize, y + (rowHeight - line) * 0.5f,
                entry.count > 1 ? entry.text + "  ×" + number(entry.count) : entry.text,
                color);
        y += rowHeight;
        ++index;
    }
    return action;
}

// ---------------------------------------------------------------------------
// Нижняя строка: вид, подсказка, выход
// ---------------------------------------------------------------------------

ScreenAction Screen::bottomBar(Ui& ui, const game::Client& client,
                               const ScreenState& state, int64_t now) {
    ScreenAction action;
    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const float height = line + unit * 2.2f;
    const Rect bar{0.0f, float(ui.screenHeight()) - height, float(ui.screenWidth()),
                   height};
    // Полоса во всю ширину, а не кнопка в углу и подпись отдельно посреди
    // пустоты. Всё, что относится к экрану целиком, живёт здесь — и глазу
    // не приходится искать это по углам.
    ui.panel(bar, "panel");

    const Rect reset{unit * 1.5f, bar.y + unit * 0.6f, line * 9.0f, height - unit * 1.2f};
    const ButtonResult fit = ui.iconButton(uiId("reset-view"), reset, "icon_planet",
                                           state.inSystem ? "Вся система" : "Вся галактика");
    if (fit.hovered) {
        ui.tooltip(state.inSystem
                       ? "отвести камеру так, чтобы стали видны все орбиты"
                       : "показать галактику целиком — вы увидите, где чьи владения");
    }
    if (fit.clicked) action.kind = ActionKind::ResetView;

    // Выход — тоже мышью. Второй щелчок подтверждает: выход по одному
    // нажатию рядом с «показать всё» — это потерянная партия из-за
    // дрогнувшей руки.
    const bool armed = quitArmedAt_ != 0 && now - quitArmedAt_ < 4000;
    if (!armed) quitArmedAt_ = 0;
    const float quitWidth = line * (armed ? 9.0f : 5.5f);
    const Rect quit{bar.right() - unit * 1.5f - quitWidth, reset.y, quitWidth, reset.h};
    const ButtonResult leave =
        ui.iconButton(uiId("quit"), quit, "icon_close", armed ? "точно выйти?" : "Выход",
                      armed ? ButtonStyle::Danger : ButtonStyle::Normal);
    if (leave.hovered && !armed) ui.tooltip("закрыть игру");
    if (leave.clicked) {
        if (armed) {
            action.kind = ActionKind::Quit;
        } else {
            quitArmedAt_ = now;
        }
    }

    // Престиж — между кнопками, слева от подсказки. Это счёт за сезон,
    // и он обязан быть на виду постоянно: игра, в которой не видно, за что
    // играешь, читается как бесцельная возня. Пять треков в подсказке,
    // сумма на полосе — потому что решение принимают по сумме, а разбирают
    // по трекам.
    const auto& empire = client.view().empire;
    const std::string score = "престиж " + grouped(empire.prestigeTotal());
    const float scoreWidth = ui.textWidth(score) + unit * 2.0f;
    const Rect scoreBox{reset.right() + unit * 1.5f, bar.y, scoreWidth, height};
    ui.textCentered(scoreBox, score, ui.theme().textAccent);
    if (ui.hotspot(uiId("prestige"), scoreBox).hovered) {
        ui.tooltip("территория " + number(empire.prestigeTerritory) +
                   " · экономика " + number(empire.prestigeEconomy) +
                   " · наука " + number(empire.prestigeScience) +
                   " · война " + number(empire.prestigeWar) +
                   " · дипломатия " + number(empire.prestigeDiplomacy));
    }

    // Строка состояния: что игра ждёт от игрока прямо сейчас. Одна,
    // короткая и всегда на одном месте — чтобы её не приходилось искать.
    std::string hint;
    TextColor hintColor = ui.theme().textAccent;
    if (!client.ready()) {
        hint = "подключаюсь к серверу...";
        hintColor = ui.theme().textDim;
    } else if (state.awaitingMoveTarget) {
        hint = "щёлкните систему на карте — туда пойдёт флот";
        hintColor = ui.theme().textWarn;
    } else if (state.system >= client.galaxy().systemCount()) {
        hint = "щёлкните звезду, чтобы посмотреть систему";
    } else if (!state.inSystem) {
        hint = "двойной щелчок по звезде открывает систему";
        hintColor = ui.theme().textDim;
    } else {
        hint = "правая кнопка вращает вид, колесо приближает";
        hintColor = ui.theme().textDim;
    }
    ui.textCentered(Rect{scoreBox.right(), bar.y, quit.x - scoreBox.right(), height}, hint,
                    hintColor);
    return action;
}

// ---------------------------------------------------------------------------
// Сборка
// ---------------------------------------------------------------------------

ScreenAction Screen::build(Ui& ui, const game::Client& client, const ScreenState& state,
                           int64_t now) {
    ScreenAction action;

    if (!client.ready()) {
        const float line = ui.lineHeight();
        const Rect box{(float(ui.screenWidth()) - line * 16.0f) * 0.5f,
                       (float(ui.screenHeight()) - line * 3.0f) * 0.5f, line * 16.0f,
                       line * 3.0f};
        ui.panel(box, "panel");
        ui.textCentered(box, "подключаюсь к серверу...", ui.theme().text);
        return action;
    }

    // Порядок сборки — это порядок отрисовки: то, что объявлено позже,
    // ложится сверху. Панели идут раньше журнала, журнал раньше подсказки.
    auto take = [&action](ScreenAction candidate) {
        if (candidate && !action) action = candidate;
    };

    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const float top = line + unit * 2.0f + unit * 1.2f;

    // Два столбца, между ними мир. Слева — «что я выбрал»: система, потом
    // планета, потом застройка. Это порядок, в котором человек думает,
    // и потому панели идут именно так, а не разбросаны по углам.
    const float leftWidth = std::min(float(ui.screenWidth()) * 0.26f, line * 21.0f);
    const float rightWidth = std::min(float(ui.screenWidth()) * 0.23f, line * 19.0f);

    take(topBar(ui, client));

    float leftBottom = top;
    take(systemPanel(ui, client, state, top, leftWidth, leftBottom));
    float planetBottom = leftBottom;
    take(planetPanel(ui, client, state, leftBottom + unit * 1.2f, leftWidth, planetBottom));

    take(fleetPanel(ui, client, state, top, rightWidth));
    take(messagePanel(ui, now, top, unit * 1.5f + leftWidth,
                      float(ui.screenWidth()) - rightWidth - unit * 1.5f));
    take(bottomBar(ui, client, state, now));
    return action;
}

}  // namespace pw::render
