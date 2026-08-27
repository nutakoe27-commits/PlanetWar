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
    // ВЕРХНЯЯ ПОЛОСА — ГЛАВНЫЙ ПРИБОР ИГРЫ.
    //
    // Она устроена как в Stellaris, и это не подражание, а вывод из того,
    // как ей пользуются. Игрок смотрит сюда не «иногда», а постоянно,
    // краем глаза, не отрываясь от карты. Значит:
    //
    //   1. Полоса идёт ВО ВСЮ ШИРИНУ и прижата к самому краю. Панель
    //      с полями и зазором глаз воспринимает как отдельное окно
    //      и вынужден на неё наводиться; полоса по краю читается
    //      периферийным зрением, без фокусировки.
    //   2. У каждого ресурса ДВА числа: запас и приход. Запас отвечает
    //      «сколько сейчас», приход — «что будет дальше», и решения
    //      принимаются про дальше. Империя с двадцатью тысячами сплавов
    //      и минусом по энергии проигрывает, и по одному запасу
    //      этого не видно.
    //   3. Ресурсы разбиты на ГРУППЫ с разделителями: производство,
    //      наука и влияние. Пять чисел в ряд читаются как пять чисел,
    //      две группы по два-три — как две мысли.
    //   4. Слева герб и имя империи, справа стадия сезона и престиж.
    //      Края полосы — самые дешёвые для глаза места, туда идёт то,
    //      что нужно реже.
    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const float height = line * 2.55f + unit * 0.5f;
    const float width = float(ui.screenWidth());

    // Заливка, а не плитка. У полосы во всю ширину нет видимых концов,
    // значит и срезанные углы ей не нужны; а плитку с углами пришлось бы
    // выпускать за края экрана, нарушая проверку «ничего не вылезает
    // за пределы окна». Оформление не стоит ослабленной проверки.
    ui.fill(Rect{0.0f, 0.0f, width, height}, ui.theme().barFill);
    // Светящаяся черта под полосой: она и отделяет прибор от карты.
    ui.fill(Rect{0.0f, height - 1.0f, width, 1.0f}, ui.theme().edge);

    const auto& empire = client.view().empire;

    // --- герб и имя империи ---
    const float crest = line * 1.7f;
    float x = unit * 1.4f;
    const EmpireColor& mineColor = empireColor(client.empire());
    ui.icon(Rect{x, (height - crest) * 0.5f, crest, crest}, "icon_crest",
            TextColor{mineColor.r, mineColor.g, mineColor.b, 1.0f});
    x += crest + unit * 0.8f;

    // Имя рядом с гербом. Обрезается по ширине, а не растягивает полосу:
    // длинное имя не имеет права сдвинуть ресурсы, ради которых сюда
    // и смотрят.
    const float nameLimit = line * 7.5f;
    std::string title = client.name();
    if (ui.textWidth(title) > nameLimit) {
        while (title.size() > 1 && ui.textWidth(title + "…") > nameLimit) {
            // По байтам с оглядкой на UTF-8: обрубить середину буквы
            // значит нарисовать мусор.
            do {
                title.pop_back();
            } while (!title.empty() && (uint8_t(title.back()) & 0xC0u) == 0x80u);
        }
        title += "…";
    }
    const float nameWidth = std::min(nameLimit, ui.textWidth(title));
    ui.text(x, (height - line) * 0.5f, title, ui.theme().text);
    if (ui.hotspot(uiId("crest"),
                   Rect{unit, 0.0f, crest + nameWidth + unit * 2.0f, height})
            .hovered) {
        ui.tooltip(client.name() + " · ваша империя · цвет герба совпадает "
                   "с цветом ваших систем на карте");
    }
    x += nameWidth + unit * 1.6f;
    ui.fill(Rect{x - unit, height * 0.22f, 1.0f, height * 0.56f}, ui.theme().edgeDim);
    x += unit * 0.2f;

    // --- ресурсы ---
    struct Entry {
        const char* icon;
        const char* label;
        const char* hint;
        int64_t value;
        fx income;
        bool group;   // после этой записи идёт разделитель
    };
    const Entry entries[] = {
        {"res_minerals", "МИНЕРАЛЫ",
         "МИНЕРАЛЫ · платят за здания · дают шахты",
         empire.minerals.floorToInt(), empire.mineralsIncome, false},
        {"res_alloys", "СПЛАВЫ",
         "СПЛАВЫ · платят за корабли · дают ТОЛЬКО литейные, из минералов",
         empire.alloys.floorToInt(), empire.alloysIncome, false},
        {"res_energy", "ЭНЕРГИЯ",
         "ЭНЕРГИЯ · уходит на содержание зданий и флота · дают электростанции",
         empire.energy.floorToInt(), empire.energyIncome, true},
        {"res_research", "НАУКА",
         "ИССЛЕДОВАНИЯ · счёт науки в престиже · дают лаборатории",
         empire.research.floorToInt(), empire.researchIncome, false},
        {"res_influence", "ВЛИЯНИЕ",
         "ВЛИЯНИЕ · счёт дипломатии в престиже · дают торговые узлы",
         empire.influence.floorToInt(), empire.influenceIncome, true},
    };

    const float iconSize = line * 1.5f;
    const float top = (height - line * 2.0f) * 0.5f;
    for (size_t index = 0; index < sizeof(entries) / sizeof(entries[0]); ++index) {
        const Entry& entry = entries[index];

        // Приход показывается ЗА МИНУТУ, а не за секунду. За секунду это
        // дробь вроде «0,4», а дробь на приборе, в который смотрят
        // мельком, не читается — её надо расшифровывать.
        const int64_t perMinute = (entry.income * fx::fromInt(60)).roundToInt();
        const std::string stock = grouped(entry.value);
        const std::string rate = (perMinute > 0 ? "+" : "") + grouped(perMinute);

        const float column = std::max(ui.textWidth(stock), ui.textWidth(rate));
        const float cellWidth = iconSize + unit * 0.7f + column;
        const Rect cell{x, 0.0f, cellWidth + unit * 0.8f, height};

        if (ui.hotspot(uiId("res", uint32_t(index)), cell).hovered) {
            ui.fill(cell.inset(unit * 0.3f), ui.theme().rowHover);
            std::string hint = entry.hint;
            hint += perMinute == 0   ? " · сейчас ни прихода, ни расхода"
                    : perMinute > 0  ? " · сейчас плюс " + grouped(perMinute) + " в минуту"
                                     : " · сейчас МИНУС " + grouped(-perMinute) + " в минуту";
            ui.tooltip(hint);
        }

        ui.icon(Rect{x, (height - iconSize) * 0.5f, iconSize, iconSize}, entry.icon);

        const float textX = x + iconSize + unit * 0.7f;
        ui.text(textX, top, stock, ui.theme().text);
        // Ноль — не хорошо и не плохо, он серый. Зелёный ноль читался бы
        // как «всё в порядке», а стоящая экономика в порядке не бывает.
        const TextColor rateColor = perMinute > 0   ? ui.theme().textGood
                                    : perMinute < 0 ? ui.theme().textBad
                                                    : ui.theme().textDim;
        ui.text(textX, top + line, rate, rateColor);

        x += cellWidth + unit * 2.0f;
        if (entry.group) {
            ui.fill(Rect{x - unit, height * 0.22f, 1.0f, height * 0.56f},
                    ui.theme().edgeDim);
            x += unit;
        }
    }

    // Простаивающие литейные — прямо в полосе, а не в подсказке.
    //
    // Это единственная ошибка экономики, которую игрок делает раз за разом:
    // строит литейные, забывает шахты, и сплавы перестают расти без всякого
    // предупреждения. Полоса обязана сказать об этом сама.
    const int64_t idle = (empire.foundryIdle * fx::fromInt(60)).roundToInt();
    if (idle > 0) {
        const std::string warn = "-" + grouped(idle);
        const float alertSize = line * 1.2f;
        const Rect box{x, 0.0f, alertSize + unit * 0.5f + ui.textWidth(warn) + unit,
                       height};
        ui.icon(Rect{x, (height - alertSize) * 0.5f, alertSize, alertSize}, "icon_alert");
        ui.text(x + alertSize + unit * 0.5f, (height - line) * 0.5f, warn,
                ui.theme().textWarn);
        if (ui.hotspot(uiId("idle-foundry"), box).hovered) {
            ui.tooltip("литейным не хватает минералов: столько сплавов в минуту "
                       "вы НЕ получаете · постройте шахты");
        }
        x += box.w;
    }

    // --- сводка по империи ---
    //
    // Систем, планет, тоннажа. Три числа, которых нет больше нигде: они
    // отвечают на «насколько я вообще большой» — вопрос, который игрок
    // задаёт себе раз в несколько минут и на который до сих пор мог
    // ответить только пересчитав список вручную.
    //
    // Место — середина полосы, между ресурсами и таймером. Ресурсы
    // меняются каждую секунду, сводка каждые несколько минут, стадия раз
    // в час: слева направо по убыванию частоты, и глаз сам запоминает,
    // куда смотреть за чем.
    {
        const uint8_t mine = uint8_t(client.empire() & 0xFFu);
        uint32_t systems = 0;
        uint32_t planets = 0;
        for (const auto& system : client.view().systems) {
            if (system.owner == mine) ++systems;
            planets += system.owner == mine ? system.ownedPlanets : 0;
        }
        uint32_t tonnage = 0;
        for (const auto& [id, fleet] : client.view().fleets) {
            if (fleet.empire == mine) tonnage += sim::fleetTonnage(fleet.composition);
        }

        struct Fact {
            const char* icon;
            std::string value;
            const char* hint;
        };
        const Fact facts[] = {
            {"icon_star", number(systems),
             "систем под вашим контролем · система считается вашей, пока в ней "
             "есть хоть одна ваша планета"},
            {"icon_planet", number(planets),
             "ваших планет · именно планеты, а не системы, дают ресурсы "
             "и очки территории"},
            {"icon_fleet", number(tonnage) + " т",
             "весь ваш тоннаж · содержание флота растёт вместе с ним"},
        };
        const float factIcon = line * 1.25f;
        for (size_t i = 0; i < sizeof(facts) / sizeof(facts[0]); ++i) {
            const float factWidth = factIcon + unit * 0.5f + ui.textWidth(facts[i].value);
            const Rect cell{x, 0.0f, factWidth + unit, height};
            if (ui.hotspot(uiId("fact", uint32_t(i)), cell).hovered) {
                ui.fill(cell.inset(unit * 0.3f), ui.theme().rowHover);
                ui.tooltip(facts[i].hint);
            }
            ui.icon(Rect{x, (height - factIcon) * 0.5f, factIcon, factIcon},
                    facts[i].icon);
            ui.text(x + factIcon + unit * 0.5f, (height - line) * 0.5f, facts[i].value,
                    ui.theme().textDim);
            x += factWidth + unit * 1.6f;
        }
    }

    // --- правый край: стадия сезона и престиж ---
    //
    // Стадия — единственное число на экране, одинаковое у всех игроков
    // сервера, и вокруг него строится вся подготовка. «До Конфликта
    // сорок минут» меняет то, что игрок делает прямо сейчас, сильнее
    // любой другой цифры здесь.
    const auto stage = sim::SeasonStage(empire.stage);
    const TextColor stageColor = stage == sim::SeasonStage::Expansion ? ui.theme().textGood
                                 : stage == sim::SeasonStage::Crisis  ? ui.theme().textBad
                                 : stage == sim::SeasonStage::Final   ? ui.theme().textWarn
                                                                      : ui.theme().textAccent;

    const std::string prestige = grouped(empire.prestigeTotal());
    const float prestigeIcon = line * 1.35f;
    const float prestigeWidth = prestigeIcon + unit * 0.6f + ui.textWidth(prestige);
    float right = width - unit * 1.4f - prestigeWidth;
    ui.icon(Rect{right, (height - prestigeIcon) * 0.5f, prestigeIcon, prestigeIcon},
            "icon_prestige");
    ui.text(right + prestigeIcon + unit * 0.6f, (height - line) * 0.5f, prestige,
            ui.theme().textWarn);
    if (ui.hotspot(uiId("prestige-top"),
                   Rect{right - unit * 0.5f, 0.0f, prestigeWidth + unit, height})
            .hovered) {
        ui.tooltip("престиж: территория " + number(empire.prestigeTerritory) +
                   " · экономика " + number(empire.prestigeEconomy) + " · наука " +
                   number(empire.prestigeScience) + " · война " +
                   number(empire.prestigeWar) + " · дипломатия " +
                   number(empire.prestigeDiplomacy) +
                   " · пять независимых счётчиков, выиграть можно любым");
    }

    right -= unit * 2.0f;
    ui.fill(Rect{right, height * 0.22f, 1.0f, height * 0.56f}, ui.theme().edgeDim);
    right -= unit * 1.6f;

    const std::string stageName = sim::stageName(stage);
    const std::string stageLeft =
        empire.stageSecondsLeft > 0 ? "ещё " + duration(empire.stageSecondsLeft)
                                    : std::string("сезон закрывается");
    const float stageWidth =
        std::max(ui.textWidth(stageName), ui.textWidth(stageLeft)) + line * 1.5f + unit;
    right -= stageWidth;

    const float clock = line * 1.35f;
    ui.icon(Rect{right, (height - clock) * 0.5f, clock, clock}, "icon_clock");
    ui.text(right + clock + unit * 0.6f, top, stageName, stageColor);
    ui.text(right + clock + unit * 0.6f, top + line, stageLeft, ui.theme().textDim);
    if (ui.hotspot(uiId("season"), Rect{right - unit, 0.0f, stageWidth + unit * 2.0f,
                                        height})
            .hovered) {
        ui.tooltip(stageHint(stage));
    }

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
        ui.panel(panel, "hud_panel");
        column.place(panel);

        const Rect titleRow = column.next();
        ui.panelTitle(panel, titleRow.bottom() - panel.y, "Система не выбрана");
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
    ui.panel(panel, "hud_panel");
    column.place(panel);

    // --- шапка ---
    const Rect titleRow = column.next();
    const char* ownership = view.owner == 0xFF     ? "ничья"
                            : view.owner == mine   ? "ваша"
                                                   : "чужая";
    const std::string counter =
        number(view.ownedPlanets) + " / " + number(view.totalPlanets);
    const TextColor counterTint = ownerTint(ui.theme(), view.owner, mine);
    const Rect band = ui.panelTitle(panel, titleRow.bottom() - panel.y,
                                    "Система " + number(state.system), counter,
                                    &counterTint);
    const Rect counterBox{band.right() - ui.textWidth(counter) - unit * 1.5f, band.y,
                          ui.textWidth(counter) + unit * 2.0f, band.h};
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

        const ButtonResult hit = ui.hotspot(uiId("planet-row", uint32_t(index)), card);
        ui.listRow(card, hit.hovered, selected);
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

    const float paletteRowHeight = line * 2.1f;

    Column column(unit);
    column.row(line * 1.25f);                          // «Планета K · класс»
    column.row(line * 1.35f, unit * 0.7f);             // состояние
    for (int r = 0; r < gridRows; ++r) column.row(cell, r + 1 < gridRows ? gridPad : 0.0f);
    // ПАЛИТРА ЗАСТРОЙКИ В СТОЛБЕЦ НЕ ВХОДИТ.
    //
    // Двенадцать построек в два столбца — это шесть строк, полторы сотни
    // пикселей, которые появляются и исчезают по щелчку. Пока палитра
    // жила внутри панели, весь левый столбец на её высоту и прыгал,
    // а на экране 1024x600 нижняя панель уезжала за нижний край —
    // проверка «ничего не вылезает за пределы окна» поймала это первой.
    //
    // Теперь палитра — отдельное окно рядом со столбцом, поверх карты.
    // Так же она устроена и в Stellaris, и по той же причине: выбор
    // из дюжины вариантов не обязан помещаться в колонку, ширина
    // которой выбрана для совсем другого.
    if (detailOpen) {
        column.row(line * 1.5f, unit * 0.1f);          // здание и чем полезно
        column.row(line * 2.0f);                       // «Снести»
    }

    const Rect panel{unit * 1.5f, top, width, column.height()};
    ui.panel(panel, "hud_panel");
    column.place(panel);

    // --- шапка ---
    const Rect titleRow = column.next();
    ui.panelTitle(panel, titleRow.bottom() - panel.y,
                  "Планета " + number(int64_t(state.planetIndex) + 1),
                  planetClassName(planet.planetClass));

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

    // --- палитра застройки: отдельное окно рядом со столбцом ---
    if (paletteOpen) {
        const int64_t minerals = client.view().empire.minerals.floorToInt();
        const int palColumns = 2;
        const int palRows =
            (int(sim::Building::Count) - 1 + palColumns - 1) / palColumns;
        const float palWidth = width * 1.15f;

        Column pal(unit);
        pal.row(line * 1.5f, unit * 0.4f);             // «Что построить»
        for (int r = 0; r < palRows; ++r) {
            pal.row(paletteRowHeight - gridPad, r + 1 < palRows ? gridPad : 0.0f);
        }

        // Окно встаёт справа от столбца и НЕ ВЫЛЕЗАЕТ за низ экрана:
        // если не помещается снизу — поднимается. Окно, часть которого
        // за кадром, это не «почти помещается», это недоступные кнопки.
        const float limit = float(ui.screenHeight()) - line * 3.6f - unit * 2.0f;
        const float palY =
            std::min(panel.y, std::max(unit, limit - pal.height()));
        const Rect box{panel.right() + unit, palY, palWidth, pal.height()};
        ui.panel(box, "hud_panel");
        pal.place(box);

        const Rect head = pal.next();
        ui.panelTitle(box, head.bottom() - box.y,
                      "Что построить в слоте " + number(int64_t(state.slot) + 1));

        for (int paletteRow = 0; paletteRow < palRows; ++paletteRow) {
            const Rect strip = pal.next();
            const float buttonWidth = (strip.w - gridPad) * 0.5f;
            for (int col = 0; col < palColumns; ++col) {
                const int index = paletteRow * palColumns + col;
                if (index >= int(sim::Building::Count) - 1) break;
                const uint8_t building = uint8_t(index + 1);
                const Rect cellBox{strip.x + float(col) * (buttonWidth + gridPad), strip.y,
                                   buttonWidth, strip.h};

                const int64_t cost = int64_t(sim::buildingCost(sim::Building(building)));
                const bool affordable = minerals >= cost;
                const ButtonResult hit = ui.iconButton(
                    uiId("build", building), cellBox, buildingIcon(building),
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

    // Панель флота переехала в ЛЕВЫЙ столбец, к системе и планете.
    //
    // Раньше она стояла справа, и правый край экрана делили между собой
    // два разных занятия: разглядывание выбранного и просмотр списка.
    // Теперь справа только список — «что у меня есть», а слева всё
    // про выбранное: система, планета в ней, флот в ней же. Одна мысль —
    // один столбец.
    const Rect panel{unit * 1.5f, top, width, column.height()};
    ui.panel(panel, "hud_panel");
    column.place(panel);

    const Rect titleRow = column.next();
    ui.panelTitle(panel, titleRow.bottom() - panel.y, "Флот в системе");

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

            const ButtonResult hit = ui.hotspot(uiId("fleet-row", uint32_t(index)), card);
            ui.listRow(card, hit.hovered, selected);
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
    // СТОПКА КАРТОЧЕК СПРАВА, а не журнал посреди экрана.
    //
    // Журнал стоял по центру над картой, гас целиком и в последние секунды
    // выглядел как призрак: полупрозрачный текст поверх звёзд, который
    // уже не прочесть, но ещё видно. Хуже того, он занимал середину —
    // самое дорогое место экрана — ради сообщений, которые игрок обычно
    // просто провожает взглядом.
    //
    // Теперь это стопка отдельных карточек под верхней полосой, слева
    // от списка. Так же устроены оповещения в Stellaris, и по той же
    // причине: у новости должна быть своя карточка с подложкой, иначе
    // на пёстром фоне её не прочитать, а середина экрана нужна карте.
    ScreenAction action;
    if (messages_ == nullptr || messages_->entries().empty()) return action;

    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const float rowHeight = line * 2.0f;

    float widest = line * 12.0f;
    for (const MessageLog::Entry& entry : messages_->entries()) {
        const std::string full =
            entry.count > 1 ? entry.text + "  ×" + number(entry.count) : entry.text;
        widest = std::max(widest, ui.textWidth(full) + line * 3.4f);
    }
    widest = std::min(widest, std::max(line * 12.0f, right - left - unit * 2.0f));

    // Гаснет только текст и только в последнюю треть жизни, а подложка
    // держится почти до конца. Карточка, у которой подложка гаснет
    // вместе с текстом, последние секунды выглядит грязным пятном.
    auto fade = [&](const MessageLog::Entry& entry) {
        const int64_t age = now - entry.bornAt;
        const int64_t fadeFrom = MessageLog::kLifetime * 2 / 3;
        if (now <= 0 || age <= fadeFrom) return 1.0f;
        return std::clamp(float(MessageLog::kLifetime - age) /
                              float(MessageLog::kLifetime - fadeFrom),
                          0.0f, 1.0f);
    };

    float y = top;
    size_t index = 0;
    for (const MessageLog::Entry& entry : messages_->entries()) {
        const float alpha = fade(entry);
        const Rect card{right - widest, y, widest, rowHeight - 2.0f};

        // Подложка отдельной карточкой: новость обязана читаться поверх
        // звёздного неба, а небо местами светлее любой панели.
        ui.panel(card, "hud_panel", std::min(1.0f, 0.55f + alpha * 0.45f));
        // Цветная полоска слева отвечает на «плохая новость или нет»
        // раньше, чем игрок прочтёт текст.
        TextColor mark = entry.color;
        mark.a = std::max(0.5f, alpha);
        ui.fill(Rect{card.x, card.y, std::max(2.0f, unit * 0.3f), card.h}, mark);

        TextColor color = entry.color;
        color.a *= alpha;

        if (entry.system != kNoSystem) {
            // Новость о том, что где-то идёт осада, бесполезна, если до
            // этого «где-то» надо ещё доскроллить вручную.
            const ButtonResult hit = ui.hotspot(uiId("message", uint32_t(index)), card);
            if (hit.hovered) {
                ui.fill(card, ui.theme().rowHover);
                ui.tooltip("щёлкните — камера перейдёт к системе " +
                           number(entry.system) + ", где это случилось");
            }
            if (hit.clicked) {
                action.kind = ActionKind::FocusSystem;
                action.value = entry.system;
            }
        }

        const float iconSize = rowHeight * 0.62f;
        if (entry.icon != nullptr) {
            ui.icon(Rect{card.x + unit * 0.8f, card.y + (card.h - iconSize) * 0.5f,
                         iconSize, iconSize},
                    entry.icon, color);
        }
        ui.text(card.x + unit * 1.3f + iconSize, card.y + (card.h - line) * 0.5f,
                entry.count > 1 ? entry.text + "  ×" + number(entry.count) : entry.text,
                color);
        y += rowHeight;
        ++index;
    }
    return action;
}

// ---------------------------------------------------------------------------
// Мини-карта галактики
// ---------------------------------------------------------------------------

ScreenAction Screen::minimap(Ui& ui, const game::Client& client,
                             const ScreenState& state, float bottom, float size) const {
    // ЗАЧЕМ ОНА. На двухстах системах вопрос «где я сейчас нахожусь»
    // без мини-карты не имеет ответа: чтобы его получить, надо отдалиться,
    // потерять текущий вид и вернуться обратно. Игрок этого не делает —
    // он просто перестаёт задавать вопрос и играет вслепую в масштабе,
    // который видит.
    //
    // Показывает ровно две вещи: где чьи владения и куда смотрит камера.
    // Ни имён, ни линий гиперпутей: мини-карта размером с ладонь,
    // и всё, что не читается с одного взгляда, только мешает.
    ScreenAction action;
    if (!client.ready()) return action;

    const sim::Galaxy& galaxy = client.galaxy();
    const uint32_t count = galaxy.systemCount();
    if (count == 0) return action;

    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const Rect panel{float(ui.screenWidth()) - size - unit, bottom - size, size, size};
    // Сначала ГЛУХАЯ заливка, потом рамка поверх. Плитка панели чуть
    // прозрачна, и сквозь мини-карту просвечивали настоящие звёзды —
    // рядом с её собственными точками они читались как ещё одни системы.
    // Карта в карте не должна показывать карту.
    ui.fill(panel, TextColor{0.012f, 0.018f, 0.032f, 1.0f});
    ui.panel(panel, "hud_panel_deep");

    const Rect field = panel.inset(unit * 0.7f);
    const float extent = float(galaxy.extent().toDouble());
    if (extent <= 0.0f) return action;

    // Галактика круглая, поле квадратное: масштаб один на обе оси,
    // иначе карта растянется и перестанет совпадать по форме с той,
    // на которую игрок смотрит.
    const float scale = std::min(field.w, field.h) / (extent * 2.0f);
    const auto toScreen = [&](float wx, float wy, float& sx, float& sy) {
        sx = field.x + field.w * 0.5f + wx * scale;
        // Ось Y на карте смотрит вверх, на экране вниз.
        sy = field.y + field.h * 0.5f - wy * scale;
    };

    // Рамка «вы здесь» рисуется ПОД точками: точки важнее, и перекрывать
    // их рамкой значит прятать то, ради чего сюда смотрят.
    if (state.viewWidth > 0.0f && state.viewHeight > 0.0f) {
        float cx = 0.0f, cy = 0.0f;
        toScreen(state.viewCenterX, state.viewCenterY, cx, cy);
        const float halfW = state.viewWidth * 0.5f * scale;
        const float halfH = state.viewHeight * 0.5f * scale;
        const Rect box{std::max(field.x, cx - halfW), std::max(field.y, cy - halfH),
                       std::min(field.right(), cx + halfW) - std::max(field.x, cx - halfW),
                       std::min(field.bottom(), cy + halfH) -
                           std::max(field.y, cy - halfH)};
        if (box.w > 1.0f && box.h > 1.0f) {
            ui.fill(box, TextColor{0.25f, 0.45f, 0.65f, 0.22f});
            ui.fill(Rect{box.x, box.y, box.w, 1.0f}, ui.theme().edge);
            ui.fill(Rect{box.x, box.bottom() - 1.0f, box.w, 1.0f}, ui.theme().edge);
            ui.fill(Rect{box.x, box.y, 1.0f, box.h}, ui.theme().edge);
            ui.fill(Rect{box.right() - 1.0f, box.y, 1.0f, box.h}, ui.theme().edge);
        }
    }

    const uint8_t mine = uint8_t(client.empire() & 0xFFu);
    const auto& systems = client.view().systems;
    for (uint32_t i = 0; i < count; ++i) {
        float sx = 0.0f, sy = 0.0f;
        toScreen(float(galaxy.positionX(i).toDouble()),
                 float(galaxy.positionY(i).toDouble()), sx, sy);

        const uint8_t owner = i < systems.size() ? systems[i].owner : 0xFFu;
        // Свои крупнее и ярче чужих, ничьи — тусклая точка. Игрок ищет
        // на мини-карте прежде всего СВОЁ, и оно обязано находиться
        // без разглядывания.
        const bool own = owner == mine;
        // Точка не меньше двух пикселей: точка в один пиксель на мини-карте
        // не видна вовсе — она попадает между строками растра и гаснет.
        // Ничья система обязана быть ВИДНА: именно ничьи и есть то, ради
        // чего на мини-карту смотрят в первой половине сезона.
        const float dot = own ? 4.0f : (owner == 0xFFu ? 2.0f : 3.0f);
        TextColor tint{0.52f, 0.60f, 0.72f, 0.9f};
        if (owner != 0xFFu) {
            const EmpireColor& c = empireColor(owner);
            tint = TextColor{c.r, c.g, c.b, own ? 1.0f : 0.85f};
        }
        ui.fill(Rect{std::round(sx - dot * 0.5f), std::round(sy - dot * 0.5f), dot, dot},
                tint);
    }

    // Выбранная система отмечена отдельно: без метки мини-карта
    // показывает галактику, но не показывает, о какой системе сейчас
    // рассказывают панели слева.
    if (state.system < count) {
        float sx = 0.0f, sy = 0.0f;
        toScreen(float(galaxy.positionX(state.system).toDouble()),
                 float(galaxy.positionY(state.system).toDouble()), sx, sy);
        ui.fill(Rect{sx - 4.0f, sy - 1.0f, 9.0f, 1.0f}, ui.theme().textWarn);
        ui.fill(Rect{sx - 1.0f, sy - 4.0f, 1.0f, 9.0f}, ui.theme().textWarn);
    }

    // Щелчок ведёт к БЛИЖАЙШЕЙ системе, а не к точке карты.
    //
    // Точность в один пиксель мини-карты — это десятки световых лет:
    // попасть по конкретной системе нельзя, и притворяться, что можно,
    // значит обманывать. Зато «перенеси меня примерно туда» — ровно тот
    // вопрос, который задают мини-карте, и на него ответ точный.
    const ButtonResult hit = ui.hotspot(uiId("minimap"), panel);
    if (hit.hovered) {
        ui.tooltip("вся галактика · щёлкните — камера перейдёт к ближайшей "
                   "оттуда системе · рамка показывает, что видно сейчас");
    }
    if (hit.clicked) {
        const float wx = (ui.frameMouseX() - (field.x + field.w * 0.5f)) / scale;
        const float wy = ((field.y + field.h * 0.5f) - ui.frameMouseY()) / scale;
        uint32_t best = kNoSystem;
        float bestDistance = 0.0f;
        for (uint32_t i = 0; i < count; ++i) {
            const float dx = float(galaxy.positionX(i).toDouble()) - wx;
            const float dy = float(galaxy.positionY(i).toDouble()) - wy;
            const float distance = dx * dx + dy * dy;
            if (best == kNoSystem || distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
        if (best != kNoSystem) {
            action.kind = ActionKind::FocusSystem;
            action.value = best;
        }
    }

    // Подпись под мини-картой: сколько систем и сколько ваших. Число,
    // которое иначе пришлось бы считать по точкам.
    uint32_t owned = 0;
    for (const auto& system : systems) {
        if (system.owner == mine) ++owned;
    }
    // Подпись НАД картой и на своей подложке: без неё две цифры висят
    // прямо на звёздах и читаются как часть галактики.
    const Rect caption{panel.x, panel.y - line * 1.35f, panel.w, line * 1.35f};
    ui.fill(caption, ui.theme().headerFill);
    ui.text(caption.x + unit * 0.7f, caption.y + (caption.h - line) * 0.5f, "ГАЛАКТИКА",
            ui.theme().textAccent);
    ui.textRight(Rect{caption.x, caption.y + (caption.h - line) * 0.5f,
                      caption.w - unit * 0.7f, line},
                 number(owned) + " из " + number(count), ui.theme().textDim);
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
    const float height = line + unit * 2.6f;
    const float width = float(ui.screenWidth());
    const Rect bar{0.0f, float(ui.screenHeight()) - height, width, height};
    // Полоса во всю ширину, а не кнопка в углу и подпись отдельно посреди
    // пустоты. Всё, что относится к экрану целиком, живёт здесь — и глазу
    // не приходится искать это по углам. Заливкой, по той же причине,
    // что и верхняя.
    ui.fill(bar, ui.theme().barFill);
    ui.fill(Rect{0.0f, bar.y, width, 1.0f}, ui.theme().edge);

    const Rect reset{unit * 1.5f, bar.y + unit * 0.7f, line * 9.0f, height - unit * 1.4f};
    const ButtonResult fit = ui.iconButton(uiId("reset-view"), reset, "icon_galaxy",
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

    // Связь, а не престиж: престиж переехал в верхнюю полосу, и держать
    // его в двух местах — значит заставлять игрока сверять два числа
    // и гадать, почему их два.
    //
    // В MMO игрок обязан отличать «сервер тормозит» от «я плохо играю»,
    // поэтому связь показывается ВСЕГДА, а не только когда стало плохо.
    // Индикатор, появляющийся вместе с проблемой, сам выглядит проблемой.
    const std::string link = number(client.roundTrip()) + " мс · потери " +
                             number(client.lossPercent()) + "%";
    const float linkWidth = ui.textWidth(link) + unit * 2.0f;
    const Rect linkBox{reset.right() + unit * 1.5f, bar.y, linkWidth, height};
    const TextColor quality = client.lossPercent() > 15  ? ui.theme().textBad
                              : client.lossPercent() > 5 ? ui.theme().textWarn
                                                         : ui.theme().textDim;
    ui.textCentered(linkBox, link, quality);
    if (ui.hotspot(uiId("link"), linkBox).hovered) {
        ui.tooltip("задержка до сервера и доля потерянных пакетов · выше "
                   "полусотни миллисекунд приказы начинают ощущаться вязкими");
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
    ui.textCentered(Rect{linkBox.right(), bar.y, quit.x - linkBox.right(), height}, hint,
                    hintColor);
    return action;
}

// ---------------------------------------------------------------------------
// Сборка
// ---------------------------------------------------------------------------
// Список своего: системы и флоты
// ---------------------------------------------------------------------------

ScreenAction Screen::outliner(Ui& ui, const game::Client& client,
                              const ScreenState& state, float top, float width) {
    // ЗАЧЕМ ОН НУЖЕН. Панели слева отвечают на вопрос «что я сейчас
    // выбрал». Этот список отвечает на другой: «что у меня вообще есть».
    // Вопросы разные, и задают их с разной частотой — первый раз в минуту,
    // второй каждый раз, когда надо что-то найти. Без списка поиск своей
    // системы среди двухсот чужих идёт глазами по карте, и на сотне систем
    // это перестаёт работать совсем.
    //
    // Разделы СВОРАЧИВАЮТСЯ. У игрока с тридцатью системами и пятью
    // флотами флоты уезжают за нижний край, а именно они меняются чаще
    // всего — свернул системы, и флот снова на виду.
    ScreenAction action;
    const float unit = ui.unit();
    const float line = ui.lineHeight();
    const uint8_t mine = uint8_t(client.empire() & 0xFFu);

    // Свои системы и свои флоты собираются заранее: высота панели считается
    // из ТОГО ЖЕ списка, что и рисуется. Считать высоту отдельно — это
    // ровно тот способ, которым панель однажды разъезжается с содержимым.
    std::vector<uint32_t> systems;
    for (uint32_t i = 0; i < uint32_t(client.view().systems.size()); ++i) {
        if (client.view().systems[i].owner == mine) systems.push_back(i);
    }
    std::vector<std::pair<uint32_t, const game::FleetView*>> fleets;
    for (const auto& [id, fleet] : client.view().fleets) {
        if (fleet.empire != mine) continue;
        fleets.emplace_back(id, &fleet);
    }
    // Самый крупный флот сверху: искать глазами обычно именно его.
    std::sort(fleets.begin(), fleets.end(), [](const auto& a, const auto& b) {
        return sim::fleetTonnage(a.second->composition) >
               sim::fleetTonnage(b.second->composition);
    });

    const float header = line * 1.7f;
    const float row = line * 1.9f;
    const float bottomLimit = float(ui.screenHeight()) - line * 3.6f - unit * 2.0f;

    // Сколько строк влезает. Место делится между разделами по потребности,
    // а не поровну: пустой раздел не должен занимать половину списка.
    const auto collapsed = [&](uint32_t section) {
        return (collapsed_ & (1u << section)) != 0;
    };
    const float available = bottomLimit - top - unit * 2.0f - header * 2.0f;
    int systemRows = collapsed(0) ? 0 : int(systems.size());
    int fleetRows = collapsed(1) ? 0 : int(fleets.size());
    if (available > 0.0f) {
        int budget = int(available / row);
        // Флотам гарантируется место: они короче и меняются чаще.
        const int fleetShare = std::min(fleetRows, std::max(0, budget / 3));
        budget -= fleetShare;
        systemRows = std::min(systemRows, std::max(0, budget));
        fleetRows = std::min(fleetRows, fleetShare + std::max(0, budget - systemRows));
    } else {
        systemRows = fleetRows = 0;
    }

    const bool systemsCut = systemRows < int(systems.size()) && !collapsed(0);
    const bool fleetsCut = fleetRows < int(fleets.size()) && !collapsed(1);

    Column column(unit);
    column.row(header, unit * 0.3f);
    for (int i = 0; i < systemRows; ++i) column.row(row);
    if (systemsCut) column.row(line * 1.4f);
    column.row(header, unit * 0.3f);
    for (int i = 0; i < fleetRows; ++i) column.row(row);
    if (fleetsCut) column.row(line * 1.4f);

    const Rect panel{float(ui.screenWidth()) - width - unit, top, width, column.height()};
    ui.panel(panel, "hud_panel");
    column.place(panel);

    // --- заголовок раздела ---
    const auto section = [&](uint32_t index, const char* title, size_t count) {
        const Rect at = column.next();
        ui.sectionHeader(at);
        const float mark = line * 0.9f;
        ui.icon(Rect{at.x + unit * 0.5f, at.y + (at.h - mark) * 0.5f, mark, mark},
                collapsed(index) ? "icon_chevron_right" : "icon_chevron_down");
        ui.text(at.x + unit * 0.6f + mark, at.y + (at.h - line) * 0.5f, title,
                ui.theme().textAccent);
        ui.textRight(Rect{at.x, at.y + (at.h - line) * 0.5f, at.w - unit * 0.6f, line},
                     number(int64_t(count)), ui.theme().textDim);
        const ButtonResult hit = ui.hotspot(uiId("section", index), at);
        if (hit.hovered) {
            ui.tooltip(collapsed(index) ? "развернуть раздел" : "свернуть раздел");
        }
        if (hit.clicked) collapsed_ ^= (1u << index);
    };

    section(0, "СИСТЕМЫ", systems.size());

    for (int i = 0; i < systemRows; ++i) {
        const uint32_t id = systems[size_t(i)];
        const auto& view = client.view().systems[id];
        const Rect at = column.next();
        const Rect card{at.x, at.y, at.w, at.h - 2.0f};
        const bool selected = id == state.system;

        const ButtonResult hit = ui.hotspot(uiId("out-system", id), card);
        ui.listRow(card, hit.hovered, selected);
        if (hit.clicked) {
            action.kind = ActionKind::FocusSystem;
            action.value = id;
        }

        const float mark = card.h * 0.62f;
        ui.icon(Rect{card.x + unit * 0.5f, card.y + (card.h - mark) * 0.5f, mark, mark},
                "icon_star");
        ui.text(card.x + unit * 0.9f + mark, card.y + (card.h - line) * 0.5f,
                "Система " + number(id), selected ? ui.theme().text : ui.theme().textDim);

        // Сколько планет уже ваши. Число, ради которого сюда и смотрят:
        // «3 / 5» означает, что в этой системе ещё есть что занимать.
        const std::string counter =
            number(view.ownedPlanets) + "/" + number(view.totalPlanets);
        ui.textRight(Rect{card.x, card.y + (card.h - line) * 0.5f, card.w - unit * 0.6f,
                          line},
                     counter,
                     view.ownedPlanets < view.totalPlanets ? ui.theme().textWarn
                                                           : ui.theme().textGood);
        if (hit.hovered) {
            ui.tooltip("щёлкните — камера наведётся на эту систему · занято планет " +
                       counter);
        }
    }
    if (systemsCut) {
        const Rect at = column.next();
        // О скрытом говорится ПРЯМО. Молча обрезанный список — это
        // сообщение «у вас больше ничего нет», и оно ложное.
        ui.text(at.x + unit * 0.5f, at.y, "ещё " +
                    number(int64_t(systems.size()) - systemRows) + " · сверните раздел",
                ui.theme().textDim);
    }

    section(1, "ФЛОТЫ", fleets.size());

    for (int i = 0; i < fleetRows; ++i) {
        const uint32_t id = fleets[size_t(i)].first;
        const game::FleetView& fleet = *fleets[size_t(i)].second;
        const Rect at = column.next();
        const Rect card{at.x, at.y, at.w, at.h - 2.0f};
        const bool selected = id == state.fleet;

        const ButtonResult hit = ui.hotspot(uiId("out-fleet", id), card);
        ui.listRow(card, hit.hovered, selected);
        if (hit.clicked) {
            action.kind = ActionKind::SelectFleet;
            action.value = id;
        }

        const float mark = card.h * 0.62f;
        ui.icon(Rect{card.x + unit * 0.5f, card.y + (card.h - mark) * 0.5f, mark, mark},
                "icon_fleet");
        ui.text(card.x + unit * 0.9f + mark, card.y + (card.h - line) * 0.5f,
                number(sim::fleetTonnage(fleet.composition)) + " т",
                selected ? ui.theme().text : ui.theme().textDim);

        // В пути или стоит — это первое, что нужно знать про флот.
        const bool moving = fleet.nextSystem != fleet.system;
        const std::string where =
            moving ? "→ " + number(fleet.nextSystem) : number(fleet.system);
        ui.textRight(Rect{card.x, card.y + (card.h - line) * 0.5f, card.w - unit * 0.6f,
                          line},
                     where, moving ? ui.theme().textWarn : ui.theme().textDim);
        if (hit.hovered) {
            std::string full;
            for (uint8_t hull = 1; hull < uint8_t(sim::Hull::Count); ++hull) {
                const uint32_t count = fleet.composition[sim::Hull(hull)];
                if (count == 0) continue;
                if (!full.empty()) full += ", ";
                full += std::string(hullName(hull)) + " " + number(count);
            }
            if (full.empty()) full = "пустой отряд";
            ui.tooltip(full + (moving ? " · идёт в систему " + number(fleet.nextSystem)
                                      : " · стоит в системе " + number(fleet.system)) +
                       " · щёлкните, чтобы выбрать");
        }
    }
    if (fleetsCut) {
        const Rect at = column.next();
        ui.text(at.x + unit * 0.5f, at.y,
                "ещё " + number(int64_t(fleets.size()) - fleetRows), ui.theme().textDim);
    }

    return action;
}

// ---------------------------------------------------------------------------

const char* actionName(ActionKind kind) {
    switch (kind) {
        case ActionKind::None: return "ничего";
        case ActionKind::EnterSystem: return "войти";
        case ActionKind::LeaveSystem: return "выйти";
        case ActionKind::SelectPlanet: return "планета";
        case ActionKind::SelectSlot: return "слот";
        case ActionKind::Build: return "строить";
        case ActionKind::Demolish: return "снести";
        case ActionKind::CancelBuild: return "отменить-стройку";
        case ActionKind::OrderShip: return "заказать";
        case ActionKind::SelectFleet: return "выбрать-флот";
        case ActionKind::BeginMove: return "отправить-флот";
        case ActionKind::CancelMove: return "отменить-приказ";
        case ActionKind::FocusSystem: return "навести";
        case ActionKind::ResetView: return "обзор";
        case ActionKind::Quit: return "выход";
        case ActionKind::Count: break;
    }
    return "?";
}

ScreenAction Screen::build(Ui& ui, const game::Client& client, const ScreenState& state,
                           int64_t now) {
    ScreenAction action;

    if (!client.ready()) {
        const float line = ui.lineHeight();
        const Rect box{(float(ui.screenWidth()) - line * 16.0f) * 0.5f,
                       (float(ui.screenHeight()) - line * 3.0f) * 0.5f, line * 16.0f,
                       line * 3.0f};
        ui.panel(box, "hud_panel");
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
    // Ниже верхней полосы ровно на её высоту: полоса нарисована из тех же
    // чисел, и разъехаться им нечем.
    const float top = line * 2.55f + unit * 0.5f + unit;

    // ДВА СТОЛБЦА, между ними карта.
    //
    // Слева — «что я выбрал»: система, потом планета, потом флот в ней.
    // Это порядок, в котором человек думает, и потому панели идут именно
    // так, а не разбросаны по углам.
    //
    // Справа — «что у меня есть»: список систем и флотов. Разделение
    // взято у Stellaris и держится на простом наблюдении: выбранное
    // разглядывают, а список просматривают, и смешивать эти два занятия
    // в одной колонке значит мешать обоим.
    const float leftWidth = std::min(float(ui.screenWidth()) * 0.26f, line * 21.0f);
    const float rightWidth = std::min(float(ui.screenWidth()) * 0.20f, line * 17.0f);

    take(topBar(ui, client));

    float leftBottom = top;
    take(systemPanel(ui, client, state, top, leftWidth, leftBottom));
    float planetBottom = leftBottom;
    take(planetPanel(ui, client, state, leftBottom + unit * 1.2f, leftWidth, planetBottom));
    take(fleetPanel(ui, client, state, planetBottom + unit * 1.2f, leftWidth));

    take(outliner(ui, client, state, top, rightWidth));
    take(messagePanel(ui, now, top, unit * 1.5f + leftWidth,
                      float(ui.screenWidth()) - rightWidth - unit * 1.5f));
    // Мини-карта — только на карте галактики. В виде системы она врала бы:
    // рамка «вы здесь» показывала бы прошлый вид карты, к которому камера
    // сейчас отношения не имеет.
    if (!state.inSystem) {
        const float barTop = float(ui.screenHeight()) - (line + unit * 2.6f) - unit;
        take(minimap(ui, client, state,
                     barTop, std::min(float(ui.screenHeight()) * 0.22f, line * 11.0f)));
    }
    take(bottomBar(ui, client, state, now));
    return action;
}

}  // namespace pw::render
