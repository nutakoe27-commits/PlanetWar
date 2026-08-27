#include "pw/sim/colony.h"

#include "pw/sim/control.h"

namespace pw::sim {

const char* colonyRefusalText(ColonyRefusal refusal) {
    switch (refusal) {
        case ColonyRefusal::Ok:           return "можно колонизировать";
        case ColonyRefusal::NotYours:     return "это не ваш флот";
        case ColonyRefusal::PlanetTaken:  return "планета занята — такие берут осадой";
        case ColonyRefusal::WrongSystem:  return "флот в другой системе";
        case ColonyRefusal::InTransit:    return "флот в пути — высадка требует остановки";
        case ColonyRefusal::NoColonizer:  return "во флоте нет колонизатора";
        case ColonyRefusal::Count:        break;
    }
    return "нельзя";
}

ColonyRefusal colonizeCheck(uint32_t fleetEmpire, const Fleet& composition,
                            const FleetLocation& location, uint32_t planetEmpire,
                            uint32_t planetSystem) {
    if (fleetEmpire == kNoEmpire) return ColonyRefusal::NotYours;
    if (planetEmpire != kNoEmpire) return ColonyRefusal::PlanetTaken;
    if (location.system != planetSystem) return ColonyRefusal::WrongSystem;
    // Стоящий флот — тот, у кого текущий и следующий узлы совпадают.
    // То же определение, что и везде: два определения «стоит» однажды
    // разъехались бы, и колонизация начала бы работать на ходу.
    if (location.nextSystem != location.system) return ColonyRefusal::InTransit;
    if (composition[Hull::Colonizer] == 0) return ColonyRefusal::NoColonizer;
    return ColonyRefusal::Ok;
}

void applyColonize(Fleet& composition, uint32_t& planetEmpire, fx& planetReadiness,
                   uint32_t empire) {
    if (composition[Hull::Colonizer] == 0) return;
    // Колонизатор ТРАТИТСЯ. Он не улетает обратно и не остаётся во флоте:
    // корабль разбирают на месте, из него и вырастает первое поселение.
    // Иначе один колонизатор занял бы всю галактику, и цена расширения
    // упала бы до нуля после первой покупки.
    --composition[Hull::Colonizer];
    planetEmpire = empire;
    planetReadiness = kColonyStartReadiness;
}

}  // namespace pw::sim
