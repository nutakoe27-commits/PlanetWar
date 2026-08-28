#include "doctest.h"

#include "game_time.h"

#include <cmath>

#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

using namespace pw;
using namespace pw::sim;
using namespace pw::test;

namespace {

struct Economy {
    World world;
    Galaxy galaxy;
    Ledger ledger;
    Entity empireEntity;

    explicit Economy(uint32_t systems = 40) {
        registerGalaxyComponents(world);
        registerFleetComponents(world);
        registerControlComponents(world);
        registerEconomyComponents(world);

        GalaxyParams params;
        params.systemCount = systems;
        galaxy.generate(world, params);
        initialiseControl(world, galaxy);

        world.setResource(&galaxy);
        world.setResource(&ledger);

        empireEntity = world.create();
        world.add<Empire>(empireEntity, Empire{fx::zero(), fx::zero(), fx::zero(),
                                               fx::zero(), fx::zero(), /*id=*/0,
                                               /*capital=*/0});
    }

    Empire& empire() { return *world.get<Empire>(empireEntity); }

    /// Отдать империи систему целиком — со всеми её планетами.
    ///
    /// Выработка достаётся владельцу ПЛАНЕТЫ, а не системы, поэтому
    /// проставить одно поле у системы больше недостаточно: она пустая
    /// запись, производят планеты.
    void own(uint32_t system, uint32_t who = 0) {
        world.get<Owner>(galaxy.systemEntity(system))->empire = who;
        world.each<Planet, Owner>([&](Entity, Planet& planet, Owner& owner) {
            if (planet.system == system) owner.empire = who;
        });
    }

    /// Планета с заданным классом и застройкой.
    ///
    /// Владелец ставится тут же: планета создаётся уже после
    /// initialiseControl, и навесить на неё владение задним числом
    /// значило бы менять таблицы посреди обхода.
    Entity colonise(uint32_t system, PlanetClass klass, Specialization spec,
                    std::initializer_list<Building> buildings, uint32_t who = 0) {
        const Entity planet = world.create();
        world.add<Planet>(planet, Planet{system, uint8_t(klass), kMaxSlots,
                                         uint8_t(spec), /*orbit=*/0});
        world.add<Owner>(planet, Owner{who, 0});
        world.add<PlanetDefense>(planet, PlanetDefense{fx::zero(), kReadinessMax});

        PlanetDevelopment development{};
        uint8_t slot = 0;
        for (Building building : buildings) {
            development.buildings[slot++] = uint8_t(building);
        }
        world.add<PlanetDevelopment>(planet, development);
        return planet;
    }

    Entity garrison(uint32_t system, const Fleet& composition, uint32_t who = 0) {
        const Entity e = world.create();
        world.add<Fleet>(e, composition);
        world.add<FleetLocation>(e, standingAt(system));
        world.add<Owner>(e, Owner{who, 0});
        return e;
    }

    /// Прогнать столько ИГРОВЫХ СЕКУНД — см. game_time.h.
    void run(int64_t seconds) {
        const int64_t ticks = testTicks(seconds);
        for (int64_t i = 0; i < ticks; ++i) systemEconomy(world, testTick(i));
    }
};

/// Сравнение с допуском: накопление за тысячи тиков копит и ошибку округления.
bool near(fx value, double expected, double tolerance = 0.5) {
    return std::abs(value.toDouble() - expected) <= tolerance;
}

}  // namespace

// ---------------------------------------------------------------------------
// Производство
// ---------------------------------------------------------------------------

TEST_CASE("экономика: шахта даёт минералы") {
    Economy world;
    world.own(1);
    world.colonise(1, PlanetClass::Desert, Specialization::None, {Building::Mine});

    world.run(1 * kHour);
    // Ожидание считается ИЗ КОНСТАНТЫ, а не записано числом. Записанное
    // число ломается при каждой перенастройке баланса и чинится подгонкой,
    // то есть перестаёт что-либо проверять; связь «шахта выдаёт свою
    // объявленную выработку» обязана держаться при любом балансе.
    CHECK(near(world.empire().minerals, kOutputMine.toDouble() * kHour, 1.0));
}

TEST_CASE("темп: час игры — заметная величина, а не минута") {
    // ПРОВЕРКА МАСШТАБА ВРЕМЕНИ, а не механики. Она сторожит обещание
    // «единица действия — час» (pw/sim/schedule.h, «ЧАСЫ ИГРЫ») со стороны
    // экономики: шахта окупает свою цену за часы, а не за минуты.
    //
    // Без такой проверки числа тихо уезжают обратно к масштабу, удобному
    // для короткого прогона, — ровно это однажды и случилось.
    const double perHour = kOutputMine.toDouble() * kHour;
    const double payback = double(buildingCost(Building::Mine)) / perHour;
    CAPTURE(perHour);
    CAPTURE(payback);
    CHECK(payback > 0.5);   // окупается дольше получаса
    CHECK(payback < 6.0);   // но за один игровой вечер

    // Стройка тоже мерится часами, а не минутами: шахта укладывается
    // в один сеанс игры, верфь его переживает.
    CHECK(buildSeconds(Building::Mine) > 10 * kMinute);
    CHECK(buildSeconds(Building::Mine) < 1 * kHour);
    CHECK(buildSeconds(Building::Shipyard) > 1 * kHour);
}

TEST_CASE("цепочка: завод просит ровно вдвое против выдачи") {
    // Одна опечатка в дроби превращает цепочку в бесплатный сплав, и никакой
    // прогон этого не заметит: сплавы просто копятся чуть быстрее. Сейчас
    // расход выражен через выработку и разойтись не может — проверка стоит
    // на случай, если кто-то вернёт отдельную дробь.
    CHECK(kFoundryMineralCost == kOutputFoundry * fx::fromInt(2));
    CHECK(kFoundryMineralCost > kOutputFoundry);
}

TEST_CASE("экономика: ничья планета не даёт ничего") {
    Economy world;
    // Владельца не назначаем.
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Mine, Building::Mine, Building::Mine}, kNoEmpire);

    world.run(1 * kHour);
    CHECK(world.empire().minerals == fx::zero());
}

TEST_CASE("экономика: потеря системы отнимает её выработку") {
    Economy world;
    world.own(1);
    world.colonise(1, PlanetClass::Desert, Specialization::None, {Building::Mine});

    world.run(30 * kMinute);
    const fx afterFifty = world.empire().minerals;
    CHECK(afterFifty > fx::zero());

    // Систему захватили. Планеты уходят вместе с ней — это и делает захват
    // значимым событием, а не косметикой на карте.
    world.own(1, /*who=*/7);
    world.run(30 * kMinute);
    CHECK(world.empire().minerals == afterFifty);
}

TEST_CASE("экономика: специализация и класс планеты умножают выработку") {
    auto mineralsAfter = [](PlanetClass klass, Specialization spec) {
        Economy world;
        world.own(1);
        world.colonise(1, klass, spec, {Building::Mine});
        world.run(1 * kHour);
        return world.empire().minerals.toDouble();
    };

    const double plain = mineralsAfter(PlanetClass::Ocean, Specialization::None);
    const double specialised = mineralsAfter(PlanetClass::Ocean, Specialization::Mining);
    const double onClass = mineralsAfter(PlanetClass::AsteroidBelt, Specialization::None);
    const double both = mineralsAfter(PlanetClass::AsteroidBelt, Specialization::Mining);

    CHECK(specialised > plain * 1.4);
    CHECK(onClass > plain * 1.2);
    // Профильное здание на профильной планете даёт почти вдвое — этого хватает,
    // чтобы специализироваться было выгодно, но непрофильное строительство
    // не становилось бессмысленным.
    CHECK(both > plain * 1.8);
    CHECK(both < plain * 2.0);
}

// ---------------------------------------------------------------------------
// Цепочка минералы -> сплавы
// ---------------------------------------------------------------------------

TEST_CASE("цепочка: завод плавит сплавы из минералов") {
    Economy world;
    world.own(1);
    // Шахтёрский мир и индустриальный: два завода и четыре шахты.
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Mine, Building::Mine, Building::Mine, Building::Mine});
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Foundry, Building::Foundry});

    world.run(1 * kHour);

    // Два завода по своей объявленной выработке за час.
    CHECK(near(world.empire().alloys, 2.0 * kOutputFoundry.toDouble() * kHour, 2.0));
    // Четыре шахты добыли ровно столько, сколько два завода съели, —
    // в остатке около нуля. Это и есть цепочка: сырьё ходит по кругу.
    CHECK(world.empire().minerals < fx::fromInt(5));
}

TEST_CASE("цепочка: без минералов завод встаёт") {
    Economy world;
    world.own(1);
    // Заводы без единой шахты. Индустриальный мир без шахтёрского бесполезен —
    // ровно то, ради чего цепочка и вводилась.
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Foundry, Building::Foundry});

    world.run(1 * kHour);
    CHECK(world.empire().alloys == fx::zero());
    CHECK(world.ledger.at(0).foundryIdle > fx::zero());
}

TEST_CASE("цепочка: нехватки хватает на частичную работу") {
    Economy world;
    world.own(1);
    // Одна шахта на четыре завода: сырья хватает на половину мощности.
    world.colonise(1, PlanetClass::Desert, Specialization::None, {Building::Mine});
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Foundry, Building::Foundry});

    world.run(2 * kHour);

    // Одна шахта против двух заводов: сырья хватает на четверть мощности,
    // потому что завод просит вдвое против того, что выдаёт.
    const double full = 2.0 * kOutputFoundry.toDouble() * 2 * kHour;
    const double alloys = world.empire().alloys.toDouble();
    CAPTURE(alloys);
    CHECK(alloys > full * 0.15);
    CHECK(alloys < full * 0.40);
    CHECK(world.ledger.at(0).foundryIdle > fx::zero());
}

// ---------------------------------------------------------------------------
// Содержание
// ---------------------------------------------------------------------------

TEST_CASE("содержание: здания и флот едят энергию") {
    Economy world;
    world.own(1);
    world.empire().energy = fx::fromInt(1000);
    world.colonise(1, PlanetClass::Desert, Specialization::None,
                   {Building::Mine, Building::Mine, Building::Mine, Building::Mine});

    world.run(1 * kHour);
    const fx afterBuildings = world.empire().energy;
    // Четыре здания по своему содержанию за час.
    const double spent = 4.0 * kUpkeepBuilding.toDouble() * kHour;
    CHECK(near(afterBuildings, 1000.0 - spent, 1.0));

    // Флот в двадцать корветов добавляет расход.
    world.garrison(1, makeFleet({{Hull::Corvette, 20}}));
    world.run(1 * kHour);
    CHECK(world.empire().energy < afterBuildings - fx::fromInt(int64_t(spent)));
}

TEST_CASE("содержание: электростанция перекрывает расход") {
    Economy world;
    world.own(1);
    world.colonise(1, PlanetClass::GasGiant, Specialization::None,
                   {Building::PowerPlant, Building::Mine});

    world.run(1 * kHour);
    // Станция на газовом гиганте даёт свою выработку с прибавкой за класс,
    // два здания просят своё содержание — империя заметно в плюсе.
    const double net = (kOutputPower.toDouble() * kClassAffinityBonus.toDouble()
                        - 2.0 * kUpkeepBuilding.toDouble()) * kHour;
    CAPTURE(net);
    CHECK(world.empire().energy > fx::fromInt(int64_t(net * 0.8)));
}

TEST_CASE("содержание: ресурсы не уходят в минус") {
    Economy world;
    world.own(1);
    // Ни одного производящего здания, зато большой флот.
    world.colonise(1, PlanetClass::Desert, Specialization::None, {Building::Fortress});
    world.garrison(1, makeFleet({{Hull::Destroyer, 50}}));

    world.run(24 * kHour);
    // Дефицит — это сигнал, а не долг: отключения появятся отдельной
    // механикой, но отрицательного склада быть не должно.
    CHECK(world.empire().energy == fx::zero());
}

// ---------------------------------------------------------------------------
// Крепости
// ---------------------------------------------------------------------------

TEST_CASE("крепость: поднимает потолок обороны СВОЕЙ планеты") {
    // Раньше крепости считались по системе, и крепость на пустом камне
    // укрепляла соседний мир-столицу: оборона строилась там, где дешевле,
    // а не там, где важно. Теперь укрепление — выбор конкретной планеты.
    Economy world;
    world.own(1);
    const Entity fortified = world.colonise(1, PlanetClass::Barren, Specialization::Fortress,
                                            {Building::Fortress, Building::Fortress});
    const Entity plain = world.colonise(1, PlanetClass::Barren, Specialization::None,
                                        {Building::Mine});

    TickContext context;
    planetDefenceCap(world.world, context);

    CHECK(world.world.get<PlanetDefense>(fortified)->maxReadiness ==
          kReadinessMax + kFortressReadiness * fx::fromInt(2));
    // Соседняя планета той же системы остаётся при базовом потолке.
    CHECK(world.world.get<PlanetDefense>(plain)->maxReadiness == kReadinessMax);
}

TEST_CASE("крепость: снос обрезает уже накопленную готовность") {
    Economy world;
    world.own(1);
    const Entity planet = world.colonise(1, PlanetClass::Barren, Specialization::None,
                                         {Building::Fortress});
    TickContext context;
    planetDefenceCap(world.world, context);

    PlanetDefense* defense = world.world.get<PlanetDefense>(planet);
    defense->readiness = defense->maxReadiness;
    CHECK(defense->readiness > kReadinessMax);

    // Крепость снесли — оборона обязана просесть сразу, а не остаться висеть.
    world.world.get<PlanetDevelopment>(planet)->buildings[0] = uint8_t(Building::None);
    planetDefenceCap(world.world, context);
    CHECK(defense->readiness == kReadinessMax);
}

// ---------------------------------------------------------------------------
// Детерминизм
// ---------------------------------------------------------------------------

TEST_CASE("экономика: воспроизводится тик в тик") {
    Economy first, second;
    for (Economy* world : {&first, &second}) {
        for (uint32_t system = 1; system <= 5; ++system) {
            world->own(system);
            world->colonise(system, PlanetClass(system % 6),
                            Specialization(system % 6),
                            {Building::Mine, Building::Foundry, Building::PowerPlant,
                             Building::Laboratory});
        }
        world->garrison(1, makeFleet({{Hull::Corvette, 12}, {Hull::Tender, 4}, {Hull::Colonizer, 2}, {Hull::Destroyer, 1}}));
    }
    REQUIRE(first.world.hash() == second.world.hash());

    first.run(1 * kHour);
    second.run(1 * kHour);

    CHECK(first.world.hash() == second.world.hash());
    // И экономика действительно работала, а не стояла.
    CHECK(first.empire().alloys > fx::zero());
    CHECK(first.empire().research > fx::zero());
}

// ---------------------------------------------------------------------------
// Инфраструктура и тормоз снежного кома
// ---------------------------------------------------------------------------

namespace {

/// Сколько энергии империя потеряла за секунду при заданном числе планет.
///
/// Считаем именно РАСХОД: содержание — это единственное, что зависит
/// от размера империи, и мерить его надо в отрыве от доходов.
double fleetUpkeepPerSecond(uint32_t planetCount, uint32_t depots) {
    Economy world;
    world.own(1);

    // Планеты БЕЗ застройки: нам нужен только их счёт, а не их доход.
    for (uint32_t index = 0; index < planetCount; ++index) {
        world.colonise(1, PlanetClass::Barren, Specialization::None, {});
    }
    for (uint32_t index = 0; index < depots; ++index) {
        world.colonise(1, PlanetClass::Barren, Specialization::None,
                       {Building::SupplyDepot});
    }
    world.garrison(1, makeFleet({{Hull::Battleship, 10}}));

    // Читаем ПОТОК из учётной книги, а не убыль казны: казна обрезается
    // по нулю, а стартовая энергия у империи как раз ноль — расход был бы
    // не виден вовсе. Ровно на этом первая версия теста и показала «0 > 0».
    world.run(1 * kSecond);
    return -world.ledger.at(0).energy.toDouble();
}

}  // namespace

TEST_CASE("содержание: большая империя платит за флот дороже") {
    // ГЛАВНОЕ ЧИСЛО ДЛЯ СЕРВЕРА НА ТЫСЯЧУ ИГРОКОВ. Без него первый, кто
    // вырвался вперёд, растёт быстрее всех просто потому, что уже впереди:
    // больше планет — больше дохода — больше флота — больше планет.
    // К третьей неделе сезона играть остаётся некому.
    const double small = fleetUpkeepPerSecond(/*планет=*/4, /*узлов=*/0);
    const double medium = fleetUpkeepPerSecond(/*планет=*/20, /*узлов=*/0);
    const double huge = fleetUpkeepPerSecond(/*планет=*/40, /*узлов=*/0);

    CHECK(medium > small);
    CHECK(huge > medium);

    // Первые восемь планет бесплатны: империя нормального размера
    // не должна чувствовать тормоз вовсе.
    CHECK(fleetUpkeepPerSecond(4, 0) == doctest::Approx(fleetUpkeepPerSecond(8, 0)));

    // Но и не разорительно: гигант платит вдвое с лишним, а не вдесятеро.
    // Иначе тормоз превращается в потолок, то есть в тот самый запрет,
    // от которого мы уходили.
    CHECK(huge < small * 3.0);
}

TEST_CASE("узел снабжения: платит за право расти дальше") {
    // Ограничитель обязан иметь ответ. Иначе он не задача, а стена.
    const double without = fleetUpkeepPerSecond(/*планет=*/40, /*узлов=*/0);
    const double withOne = fleetUpkeepPerSecond(/*планет=*/39, /*узлов=*/1);
    const double withFour = fleetUpkeepPerSecond(/*планет=*/36, /*узлов=*/4);

    CHECK(withOne < without);
    CHECK(withFour < withOne);

    // Но бесплатным флот не станет никогда.
    const double withMany = fleetUpkeepPerSecond(/*планет=*/30, /*узлов=*/10);
    CHECK(withMany > 0.0);
}

TEST_CASE("хабитат: добавляет слоты, и застройка в них работает") {
    Economy world;
    world.own(1);

    // Планета на четыре слота: хабитат, а дальше — три шахты и ещё две
    // сверх её собственного предела.
    const Entity planet = world.colonise(
        1, PlanetClass::Barren, Specialization::None,
        {Building::Habitat, Building::Mine, Building::Mine, Building::Mine,
         Building::Mine, Building::Mine});
    world.world.get<Planet>(planet)->slots = 4;

    const PlanetDevelopment& development = *world.world.get<PlanetDevelopment>(planet);
    const Planet& data = *world.world.get<Planet>(planet);

    CHECK(usableSlots(data, development) == 4 + kHabitatSlots);

    // Пять шахт работают, а не три: слоты за пределом собственного лимита
    // планеты открыл хабитат.
    world.run(1 * kHour);
    const double perMine = kOutputMine.toDouble() * kHour;
    CHECK(world.empire().minerals.toDouble() > perMine * 4.5);
}

TEST_CASE("хабитат: без него лишние слоты не работают") {
    Economy world;
    world.own(1);
    const Entity planet = world.colonise(
        1, PlanetClass::Barren, Specialization::None,
        {Building::Mine, Building::Mine, Building::Mine, Building::Mine,
         Building::Mine, Building::Mine});
    world.world.get<Planet>(planet)->slots = 4;

    world.run(1 * kHour);
    // Ровно четыре шахты, а не шесть.
    const double perMine = kOutputMine.toDouble() * kHour;
    CHECK(world.empire().minerals.toDouble() < perMine * 4.5);
}
