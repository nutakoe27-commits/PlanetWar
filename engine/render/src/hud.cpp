#include "pw/render/hud.h"

#include <algorithm>
#include <cstdio>

namespace pw::render {

namespace {

const TextColor kNormal{0.92f, 0.94f, 0.97f, 1.0f};
const TextColor kDim{0.68f, 0.71f, 0.78f, 1.0f};
const TextColor kGood{0.62f, 0.84f, 0.52f, 1.0f};
const TextColor kWarn{0.93f, 0.72f, 0.35f, 1.0f};
const TextColor kBad{0.88f, 0.45f, 0.45f, 1.0f};

std::string number(int64_t value) {
    // Разряды через узкий пробел: 144530 читается мгновенно, 144530 — нет.
    // Именно через обычный пробел, а не запятую: в русской типографике
    // запятая — это дробная часть.
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    std::string digits = buffer;

    const bool negative = !digits.empty() && digits[0] == '-';
    if (negative) digits.erase(0, 1);

    std::string out;
    int counter = 0;
    for (size_t i = digits.size(); i > 0; --i) {
        out.insert(out.begin(), digits[i - 1]);
        if (++counter % 3 == 0 && i > 1) out.insert(out.begin(), ' ');
    }
    return negative ? "-" + out : out;
}

const char* buildingName(uint8_t building) {
    switch (sim::Building(building)) {
        case sim::Building::None:       return "—";
        case sim::Building::Mine:       return "шахта";
        case sim::Building::PowerPlant: return "энергия";
        case sim::Building::Foundry:    return "литейная";
        case sim::Building::Laboratory: return "лаборатория";
        case sim::Building::TradeHub:   return "торговля";
        case sim::Building::Fortress:   return "крепость";
        case sim::Building::Shipyard:   return "верфь";
        default:                        return "?";
    }
}

const char* starName(uint8_t starClass) {
    switch (sim::StarClass(starClass)) {
        case sim::StarClass::Red:       return "красный карлик";
        case sim::StarClass::Yellow:    return "жёлтая звезда";
        case sim::StarClass::Blue:      return "голубой гигант";
        case sim::StarClass::Neutron:   return "нейтронная звезда";
        case sim::StarClass::BlackHole: return "чёрная дыра";
        default:                        return "звезда";
    }
}

}  // namespace

void MessageLog::add(const std::string& text, const TextColor& color, int64_t now) {
    entries_.push_back(Entry{text, color, now});
    // Держим только последние: длинная лента съедает экран и внимание.
    while (entries_.size() > kMaxVisible) entries_.erase(entries_.begin());
}

void MessageLog::update(int64_t now) {
    while (!entries_.empty() && now - entries_.front().bornAt > kLifetime) {
        entries_.erase(entries_.begin());
    }
}

float Hud::lineHeight(int screenHeight) {
    // Доля высоты экрана, а не фиксированный размер: на 4K надпись
    // в 14 пикселей нечитаема, а на ноутбуке в 28 занимает пол-экрана.
    return std::clamp(float(screenHeight) * 0.022f, 13.0f, 30.0f);
}

void Hud::push(HudFrame& out, const std::string& text, float x, float y, float height,
               const TextColor& color) const {
    out.lines.push_back(HudLine{text, color});
    if (font_ != nullptr) font_->layout(text, x, y, height, color, out.sprites);
}

void Hud::build(const game::Client& client, const Selection& selection, int screenWidth,
                int screenHeight, HudFrame& out) const {
    build(client, selection, screenWidth, screenHeight, /*now=*/0, out);
}

void Hud::build(const game::Client& client, const Selection& selection, int screenWidth,
                int screenHeight, int64_t now, HudFrame& out) const {
    out.clear();
    if (!client.ready()) {
        push(out, "подключаюсь...", 16.0f, 16.0f, lineHeight(screenHeight), kDim);
        return;
    }

    const float height = lineHeight(screenHeight);
    const float step = height * 1.3f;
    const float margin = height * 0.8f;

    // --- ресурсы: верхняя строка ---
    //
    // Первое, что нужно игроку: можно ли сейчас что-то построить.
    const auto& empire = client.view().empire;
    float y = margin;
    push(out, "сплавы " + number(empire.alloys.floorToInt()) +
                  "   минералы " + number(empire.minerals.floorToInt()) +
                  "   энергия " + number(empire.energy.floorToInt()),
         margin, y, height, kNormal);

    // Владения и флот: сколько у меня всего.
    uint32_t systems = 0;
    for (const auto& system : client.view().systems) {
        if (system.owner == uint8_t(client.empire())) ++systems;
    }
    uint32_t fleets = 0, tonnage = 0;
    for (const auto& [id, fleet] : client.view().fleets) {
        if (fleet.empire != uint8_t(client.empire())) continue;
        ++fleets;
        tonnage += sim::fleetTonnage(fleet.composition);
    }

    y += step;
    push(out, "систем " + number(systems) + "   флот " + number(tonnage) + " т в " +
                  number(fleets) + " отрядах",
         margin, y, height, kDim);

    // Связь. Показывается всегда: в MMO игрок обязан отличать «сервер
    // тормозит» от «я плохо играю».
    y += step;
    const TextColor quality = client.lossPercent() > 15   ? kBad
                              : client.lossPercent() > 5  ? kWarn
                                                          : kDim;
    push(out, "задержка " + number(client.roundTrip()) + " мс, потери " +
                  number(client.lossPercent()) + "%",
         margin, y, height, quality);

    // --- выбранная система: левый нижний угол ---
    if (selection.system < client.galaxy().systemCount()) {
        const uint32_t index = selection.system;
        const auto& view = client.view().systems[index];

        std::vector<HudLine> panel;
        panel.push_back(HudLine{"система " + number(index) + " · " +
                                    starName(client.galaxy().starClass(index)),
                                kNormal});

        const char* ownership = view.owner == 0xFF ? "ничья"
                                : view.owner == uint8_t(client.empire()) ? "ваша"
                                                                        : "чужая";
        const TextColor ownerColor = view.owner == 0xFF ? kDim
                                     : view.owner == uint8_t(client.empire()) ? kGood
                                                                              : kBad;
        panel.push_back(HudLine{std::string(ownership) + ", оборона " +
                                    number(view.readiness) + "%",
                                ownerColor});

        if (view.siegeEmpire != 0xFF) {
            panel.push_back(HudLine{"ОСАДА: " + number(view.siegeProgress) + "%", kWarn});
        }

        const auto planets = client.planetsAt(index);

        // Есть ли верфь. Без неё система не строит флот, и знать это
        // игрок должен ДО того, как нажмёт заказ, а не после.
        uint32_t shipyards = 0;
        for (const auto& planet : planets) {
            for (uint8_t slot = 0; slot < planet.slots; ++slot) {
                if (planet.buildings[slot] == uint8_t(sim::Building::Shipyard)) ++shipyards;
            }
        }
        if (view.owner == uint8_t(client.empire())) {
            panel.push_back(shipyards > 0
                                ? HudLine{"верфей " + number(shipyards) + " — флот строится",
                                          kGood}
                                : HudLine{"верфи нет — флот здесь не построить", kWarn});
        }

        for (size_t order = 0; order < planets.size(); ++order) {
            const auto& planet = planets[order];
            // Стрелка отмечает планету, на которую пойдёт следующая
            // постройка. Без неё игрок строит вслепую и не понимает,
            // почему здание появилось не там, где он ждал.
            const bool active = order == selection.planetIndex;
            const std::string mark = active ? "> " : "  ";

            panel.push_back(HudLine{mark + "планета " + number(int64_t(order) + 1) +
                                        ": слотов " + number(planet.slots) + ", свободно " +
                                        number(planet.freeSlots()),
                                    active ? kNormal : kDim});

            std::string built;
            for (uint8_t slot = 0; slot < planet.slots; ++slot) {
                if (planet.buildings[slot] == uint8_t(sim::Building::None)) continue;
                if (!built.empty()) built += ", ";
                built += buildingName(planet.buildings[slot]);
            }
            if (!built.empty()) panel.push_back(HudLine{"     " + built, kDim});
        }

        const auto own = client.fleetsAt(index);
        for (uint32_t id : own) {
            const auto& fleet = client.view().fleets.at(id);
            const bool active = id == selection.fleet;
            panel.push_back(HudLine{
                std::string(active ? "> " : "  ") + "флот " + number(id) + ": " +
                    number(fleet.composition.corvettes) + " корв, " +
                    number(fleet.composition.destroyers) + " эсм, " +
                    number(fleet.composition.cruisers) + " крей, " +
                    number(fleet.composition.battleships) + " линк",
                active ? kGood : kDim});
        }

        // Панель растёт вверх от нижнего края: так её высота не влияет
        // на положение первой строки, и глаз не бегает.
        float panelY = float(screenHeight) - margin - float(panel.size()) * step;
        for (const HudLine& line : panel) {
            push(out, line.text, margin, panelY, height, line.color);
            panelY += step;
        }
    }

    // --- журнал событий: по центру сверху ---
    //
    // Именно там, куда игрок смотрит реже всего в спокойный момент и
    // куда переводит взгляд, когда что-то случилось. Сбоку он спорил бы
    // с ресурсами, снизу — с панелью системы.
    if (messages_ != nullptr && !messages_->entries().empty()) {
        float messageY = margin;
        for (const MessageLog::Entry& entry : messages_->entries()) {
            TextColor color = entry.color;
            // Гаснет к концу жизни: угасающая строка не отвлекает,
            // но ещё читается, если игрок обернулся.
            const int64_t age = now - entry.bornAt;
            const int64_t fadeFrom = MessageLog::kLifetime * 2 / 3;
            if (now > 0 && age > fadeFrom) {
                const float left = float(MessageLog::kLifetime - age) /
                                   float(MessageLog::kLifetime - fadeFrom);
                color.a *= std::clamp(left, 0.0f, 1.0f);
            }

            const float textWidth = font_ != nullptr
                                        ? font_->width(entry.text, height)
                                        : float(entry.text.size()) * height * 0.47f;
            push(out, entry.text, (float(screenWidth) - textWidth) * 0.5f, messageY, height,
                 color);
            messageY += step;
        }
    }

    // --- подсказки: правый нижний угол ---
    const char* hints[] = {
        "ЛКМ — выбрать систему, ЛКМ по другой — отправить флот",
        "ПКМ — двигать карту, колесо — зум, пробел — вся галактика",
        "1..8 — построить здание, Q W E R — заказать корабль",
    };
    float hintY = float(screenHeight) - margin - 3.0f * step;
    for (const char* hint : hints) {
        const float x = float(screenWidth) - margin -
                        (font_ != nullptr ? font_->width(hint, height)
                                          : float(std::string(hint).size()) * height * 0.6f);
        push(out, hint, x, hintY, height, kDim);
        hintY += step;
    }
}

}  // namespace pw::render
