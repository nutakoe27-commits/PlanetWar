// Колонизация: как империя вырастает с одной планеты.
#include <string>
//
// Проверяется не «функция вернула то, что вернула», а ПРАВИЛА, на которых
// держится расширение. Каждое из них — решение дизайна, и если оно
// изменится, тест обязан покраснеть и заставить это обсудить.

#include "doctest.h"

#include "pw/sim/colony.h"
#include "pw/sim/control.h"

using namespace pw;
using namespace pw::sim;

namespace {

FleetLocation standing(uint32_t system) {
    return standingAt(system);
}

FleetLocation moving(uint32_t from, uint32_t to) {
    return inTransit(from, to, fx::fromFraction(1, 2));
}

}  // namespace

TEST_CASE("колония: флот с колонизатором занимает ничью планету") {
    const Fleet fleet = makeFleet({{Hull::Colonizer, 1}});
    CHECK(colonizeCheck(0, fleet, standing(7), kNoEmpire, 7) == ColonyRefusal::Ok);
}

TEST_CASE("колония: без колонизатора нельзя, сколько бы ни было боевых") {
    // ЭТО ГЛАВНОЕ ПРАВИЛО РАСШИРЕНИЯ. Боевой флот, каким бы он ни был,
    // не занимает пустую планету — он её только охраняет. Иначе титан
    // случайно колонизировал бы всё, мимо чего пролетает, и вся цена
    // расширения свелась бы к цене прогулки.
    const Fleet armada =
        makeFleet({{Hull::Battleship, 20}, {Hull::Titan, 3}, {Hull::Corvette, 50}});
    CHECK(colonizeCheck(0, armada, standing(7), kNoEmpire, 7) ==
          ColonyRefusal::NoColonizer);
}

TEST_CASE("колония: занятую планету высадкой не берут") {
    // Чужое отнимают осадой. Если бы колонизатор работал и по занятым,
    // осада стала бы не нужна вовсе: дешевле привезти колониста.
    const Fleet fleet = makeFleet({{Hull::Colonizer, 1}});
    CHECK(colonizeCheck(0, fleet, standing(7), 1, 7) == ColonyRefusal::PlanetTaken);
    // Даже свою собственную — колонизировать нечего.
    CHECK(colonizeCheck(0, fleet, standing(7), 0, 7) == ColonyRefusal::PlanetTaken);
}

TEST_CASE("колония: на ходу не высаживаются") {
    const Fleet fleet = makeFleet({{Hull::Colonizer, 1}});
    CHECK(colonizeCheck(0, fleet, moving(7, 8), kNoEmpire, 7) ==
          ColonyRefusal::InTransit);
}

TEST_CASE("колония: планета в другой системе недосягаема") {
    const Fleet fleet = makeFleet({{Hull::Colonizer, 1}});
    CHECK(colonizeCheck(0, fleet, standing(7), kNoEmpire, 9) ==
          ColonyRefusal::WrongSystem);
}

TEST_CASE("колония: высадка тратит колонизатор и не трогает остальных") {
    Fleet fleet = makeFleet({{Hull::Colonizer, 2}, {Hull::Corvette, 4}});
    uint32_t owner = kNoEmpire;
    fx readiness = fx::zero();

    applyColonize(fleet, owner, readiness, 3);

    CHECK(owner == 3u);
    CHECK(fleet[Hull::Colonizer] == 1u);   // один потрачен
    CHECK(fleet[Hull::Corvette] == 4u);    // эскорт цел
    CHECK(readiness == kColonyStartReadiness);
}

TEST_CASE("колония: рождается с обороной, но не с полной") {
    // Ноль означал бы, что свежую колонию берёт мимоходом любой корвет,
    // и колонизировать в спорных областях было бы бессмысленно. Полная
    // означала бы, что колония крепче столицы в первую же секунду.
    CHECK(kColonyStartReadiness > fx::zero());
    CHECK(kColonyStartReadiness < kReadinessMax);
}

TEST_CASE("колония: у каждого отказа своя причина словами") {
    // Игроку показывают ПРИЧИНУ, а не «нельзя»: «нет колонизатора»
    // и «планета занята» — это два разных следующих действия.
    for (uint8_t i = 0; i < uint8_t(ColonyRefusal::Count); ++i) {
        const char* text = colonyRefusalText(ColonyRefusal(i));
        REQUIRE(text != nullptr);
        CHECK(text[0] != '\0');
    }
    // И они все разные — иначе перечисление не помогает игроку.
    for (uint8_t a = 0; a < uint8_t(ColonyRefusal::Count); ++a) {
        for (uint8_t b = uint8_t(a + 1); b < uint8_t(ColonyRefusal::Count); ++b) {
            CHECK(std::string(colonyRefusalText(ColonyRefusal(a))) !=
                  std::string(colonyRefusalText(ColonyRefusal(b))));
        }
    }
}

TEST_CASE("колония: колонизатор не воюет и гибнет позже эскорта") {
    // Два правила разом, и оба — дизайн, а не реализация.
    //
    // Первое: колонизатор не добавляет флоту ни одного оружейного слота.
    // Второе: он стоит в перечислении ПОСЛЕ дешёвого эскорта, значит
    // потери списываются сначала с корветов — то есть прикрытие работает.
    CHECK(hullCost(Hull::Colonizer) > hullCost(Hull::Corvette));
    CHECK(hullCost(Hull::Colonizer) > hullCost(Hull::Tender));
    CHECK(hullCost(Hull::Colonizer) < hullCost(Hull::Destroyer));
    CHECK(uint8_t(Hull::Colonizer) > uint8_t(Hull::Corvette));

    // Колонизатор медленный: расширение обязано стоить времени.
    CHECK(hullSpeed(Hull::Colonizer) < hullSpeed(Hull::Corvette));
}

// ---------------------------------------------------------------------------
// Стоянка флота у планеты
// ---------------------------------------------------------------------------

TEST_CASE("стоянка: у флота в пути орбиты нет") {
    // Флот между звёздами не стоит ни у чего. Показывать его у планеты
    // значило бы врать о том, где он находится и что защищает.
    const FleetLocation flying = inTransit(3, 4, fx::fromFraction(1, 3));
    CHECK(flying.orbit == kNoOrbit);
}

TEST_CASE("стоянка: фабрика ставит стоящий флот без орбиты") {
    const FleetLocation parked = standingAt(5);
    CHECK(parked.system == 5u);
    CHECK(parked.nextSystem == 5u);
    CHECK(parked.orbit == kNoOrbit);
    CHECK(parked.progress == fx::zero());
}

TEST_CASE("стоянка: орбиту можно задать сразу") {
    const FleetLocation guard = standingAt(5, 2);
    CHECK(guard.orbit == 2u);
}

// ---------------------------------------------------------------------------
// Выделение кораблей из флота
// ---------------------------------------------------------------------------

TEST_CASE("выделение: колонизатор отделяется от боевого флота") {
    // Ради этого механика и существует: флоты в системе сливаются, и без
    // выделения колонизатор уходил бы воевать вместе с линкорами.
    Fleet fleet = makeFleet({{Hull::Colonizer, 2}, {Hull::Battleship, 3}});
    CHECK(splitCheck(fleet, standingAt(1), Hull::Colonizer, 1) == SplitRefusal::Ok);

    const Fleet taken = applySplit(fleet, Hull::Colonizer, 1);
    CHECK(taken[Hull::Colonizer] == 1u);
    CHECK(taken[Hull::Battleship] == 0u);
    CHECK(fleet[Hull::Colonizer] == 1u);
    CHECK(fleet[Hull::Battleship] == 3u);
}

TEST_CASE("выделение: на ходу перестроиться нельзя") {
    const Fleet fleet = makeFleet({{Hull::Colonizer, 1}, {Hull::Corvette, 4}});
    CHECK(splitCheck(fleet, inTransit(1, 2, fx::fromFraction(1, 4)), Hull::Colonizer, 1) ==
          SplitRefusal::InTransit);
}

TEST_CASE("выделение: больше, чем есть, не выделишь") {
    const Fleet fleet = makeFleet({{Hull::Colonizer, 1}, {Hull::Corvette, 4}});
    CHECK(splitCheck(fleet, standingAt(1), Hull::Colonizer, 2) == SplitRefusal::NotEnough);
    CHECK(splitCheck(fleet, standingAt(1), Hull::Titan, 1) == SplitRefusal::NotEnough);
    CHECK(splitCheck(fleet, standingAt(1), Hull::Corvette, 0) == SplitRefusal::NotEnough);
}

TEST_CASE("выделение: весь флот выделить нельзя") {
    // Иначе исходный флот опустеет и будет распущен, а новый займёт его
    // место: игрок получит тот же флот с новым номером и решит, что игра
    // его обманула.
    const Fleet lone = makeFleet({{Hull::Colonizer, 3}});
    CHECK(splitCheck(lone, standingAt(1), Hull::Colonizer, 3) == SplitRefusal::WholeFleet);
    // А два из трёх — можно.
    CHECK(splitCheck(lone, standingAt(1), Hull::Colonizer, 2) == SplitRefusal::Ok);
}

TEST_CASE("выделение: тоннаж сходится — корабли не появляются и не пропадают") {
    Fleet fleet = makeFleet({{Hull::Colonizer, 2}, {Hull::Corvette, 7}, {Hull::Titan, 1}});
    const uint32_t before = fleetTonnage(fleet);

    const Fleet taken = applySplit(fleet, Hull::Corvette, 3);

    CHECK(fleetTonnage(fleet) + fleetTonnage(taken) == before);
}

TEST_CASE("выделение: у каждого отказа своя причина словами") {
    for (uint8_t a = 0; a < uint8_t(SplitRefusal::Count); ++a) {
        REQUIRE(splitRefusalText(SplitRefusal(a)) != nullptr);
        for (uint8_t b = uint8_t(a + 1); b < uint8_t(SplitRefusal::Count); ++b) {
            CHECK(std::string(splitRefusalText(SplitRefusal(a))) !=
                  std::string(splitRefusalText(SplitRefusal(b))));
        }
    }
}
