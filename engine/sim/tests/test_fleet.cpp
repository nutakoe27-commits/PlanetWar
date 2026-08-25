#include "doctest.h"

#include <algorithm>
#include <queue>
#include <vector>

#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

using namespace pw;
using namespace pw::sim;

namespace {

/// Мир с галактикой и флотами. Галактика кладётся в ресурсы мира — системы
/// тика берут её оттуда.
struct Space {
    World world;
    Galaxy galaxy;
    Simulation* sim = nullptr;

    explicit Space(uint64_t seed = 0xFEED, uint32_t systems = 200) {
        registerGalaxyComponents(world);
        registerFleetComponents(world);
        GalaxyParams params;
        params.seed = seed;
        params.systemCount = systems;
        galaxy.generate(world, params);
        world.setResource(&galaxy);
    }

    Entity spawnFleet(uint32_t system, const Fleet& composition) {
        const Entity e = world.create();
        world.add<Fleet>(e, composition);
        world.add<FleetLocation>(e, FleetLocation{system, system, fx::zero()});
        world.add<MoveOrder>(e, MoveOrder{kNoSystem, 0});
        return e;
    }

    /// Прогнать N тиков системой движения.
    void run(uint64_t ticks) {
        for (uint64_t i = 0; i < ticks; ++i) {
            TickContext context;
            context.tick = i;
            systemFleetMovement(world, context);
        }
    }
};

/// Независимый Дейкстра для проверки оптимальности A*.
///
/// Проверять алгоритм самим собой бессмысленно, поэтому эталон считается
/// другим способом.
double dijkstraDistance(const Galaxy& galaxy, uint32_t from, uint32_t to) {
    const uint32_t count = galaxy.systemCount();
    std::vector<double> best(count, 1e30);
    using Item = std::pair<double, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;

    best[from] = 0.0;
    queue.emplace(0.0, from);
    while (!queue.empty()) {
        const auto [distance, node] = queue.top();
        queue.pop();
        if (distance > best[node] + 1e-9) continue;
        if (node == to) return distance;
        for (uint32_t k = 0; k < galaxy.neighborCount(node); ++k) {
            const uint32_t neighbour = galaxy.neighbors(node)[k];
            const double step = galaxy.straightDistance(node, neighbour).toDouble();
            if (distance + step < best[neighbour] - 1e-9) {
                best[neighbour] = distance + step;
                queue.emplace(best[neighbour], neighbour);
            }
        }
    }
    return best[to];
}

double pathDistance(const Galaxy& galaxy, const std::vector<uint32_t>& path) {
    double total = 0.0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        total += galaxy.straightDistance(path[i], path[i + 1]).toDouble();
    }
    return total;
}

}  // namespace

// ---------------------------------------------------------------------------
// Состав флота
// ---------------------------------------------------------------------------

TEST_CASE("флот: скорость задаёт самый медленный корабль") {
    CHECK(fleetSpeed(Fleet{10, 0, 0, 0}) == kSpeedCorvette);
    CHECK(fleetSpeed(Fleet{10, 5, 0, 0}) == kSpeedDestroyer);
    CHECK(fleetSpeed(Fleet{10, 5, 2, 0}) == kSpeedCruiser);
    // Один линкор замедляет весь флот — это и делает рейдовые отряды
    // отдельной ролью, а не побочным занятием линейных сил.
    CHECK(fleetSpeed(Fleet{100, 0, 0, 1}) == kSpeedBattleship);
    CHECK(fleetSpeed(Fleet{0, 0, 0, 0}) == fx::zero());
}

TEST_CASE("флот: тоннаж и пустота") {
    CHECK(fleetEmpty(Fleet{0, 0, 0, 0}));
    CHECK_FALSE(fleetEmpty(Fleet{1, 0, 0, 0}));
    CHECK(fleetTonnage(Fleet{1, 1, 1, 1}) == 1u + 3u + 8u + 20u);
    // Линкор весит больше четырёх корветов — потери должны быть ощутимы.
    CHECK(fleetTonnage(Fleet{4, 0, 0, 0}) < fleetTonnage(Fleet{0, 0, 0, 1}));
}

// ---------------------------------------------------------------------------
// Поиск пути
// ---------------------------------------------------------------------------

TEST_CASE("путь: маршрут связен и начинается там, где просили") {
    Space space;
    std::vector<uint32_t> path;

    const int32_t hops = space.galaxy.findPath(0, 137, path);
    REQUIRE(hops > 0);
    REQUIRE(path.size() == size_t(hops) + 1);
    CHECK(path.front() == 0u);
    CHECK(path.back() == 137u);

    // Каждый шаг обязан быть настоящей гиперлинией.
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        CHECK(space.galaxy.laneLength(path[i], path[i + 1]) > fx::zero());
    }
}

TEST_CASE("путь: A* находит кратчайший по расстоянию") {
    Space space(0xA11, 200);
    std::vector<uint32_t> path;

    for (uint32_t target : {17u, 63u, 128u, 199u}) {
        if (target >= space.galaxy.systemCount()) continue;
        REQUIRE(space.galaxy.findPath(0, target, path) >= 0);

        const double ours = pathDistance(space.galaxy, path);
        const double reference = dijkstraDistance(space.galaxy, 0, target);
        CAPTURE(target);
        // Совпадение с независимым Дейкстрой — оценка A* не завышает,
        // значит найденный маршрут действительно кратчайший.
        CHECK(ours <= reference * 1.0001 + 0.01);
    }
}

TEST_CASE("путь: короткий по расстоянию не обязан быть коротким по прыжкам") {
    Space space(0xB22, 300);
    std::vector<uint32_t> path;

    int differing = 0;
    for (uint32_t target = 1; target < 60; ++target) {
        const int32_t byDistance = space.galaxy.findPath(0, target, path);
        const int32_t byHops = space.galaxy.hopDistance(0, target);
        REQUIRE(byDistance >= 0);
        REQUIRE(byHops >= 0);
        // Путь по расстоянию не может быть КОРОЧЕ минимального по прыжкам.
        CHECK(byDistance >= byHops);
        if (byDistance != byHops) ++differing;
    }
    // И на реальной карте такие случаи обязаны встречаться: три коротких
    // прыжка часто быстрее одного длинного. Если бы их не было, различать
    // два вида расстояния не имело бы смысла.
    CHECK(differing > 0);
}

TEST_CASE("путь: до себя и в никуда") {
    Space space;
    std::vector<uint32_t> path;

    CHECK(space.galaxy.findPath(5, 5, path) == 0);
    CHECK(path.size() == 1);
    CHECK(space.galaxy.findPath(0, 99999, path) == -1);
    CHECK(space.galaxy.nextHop(7, 7) == 7);
}

TEST_CASE("путь: первый шаг совпадает со вторым узлом маршрута") {
    Space space(0xC33, 150);
    std::vector<uint32_t> path;
    for (uint32_t target : {12u, 44u, 99u}) {
        REQUIRE(space.galaxy.findPath(3, target, path) > 0);
        CHECK(space.galaxy.nextHop(3, target) == int32_t(path[1]));
    }
}

// ---------------------------------------------------------------------------
// Движение
// ---------------------------------------------------------------------------

TEST_CASE("движение: флот доходит до цели") {
    Space space;
    const uint32_t target = 90;
    const Entity fleet = space.spawnFleet(0, Fleet{4, 0, 0, 0});
    space.world.get<MoveOrder>(fleet)->target = target;

    // С запасом: 200 систем, диаметр десятки прыжков, корветы быстрые.
    space.run(600000);

    const FleetLocation* location = space.world.get<FleetLocation>(fleet);
    REQUIRE(location != nullptr);
    CHECK(location->system == target);
    CHECK(location->nextSystem == target);
    // Приказ снят — значит флот действительно понял, что дошёл.
    CHECK(space.world.get<MoveOrder>(fleet)->target == kNoSystem);
}

TEST_CASE("движение: медленный флот идёт дольше быстрого") {
    Space space;
    const uint32_t target = 90;

    auto ticksToArrive = [&](const Fleet& composition) {
        const Entity fleet = space.spawnFleet(0, composition);
        space.world.get<MoveOrder>(fleet)->target = target;
        uint64_t ticks = 0;
        while (ticks < 900000) {
            TickContext context;
            context.tick = ticks;
            systemFleetMovement(space.world, context);
            ++ticks;
            if (space.world.get<FleetLocation>(fleet)->system == target) break;
        }
        return ticks;
    };

    const uint64_t fast = ticksToArrive(Fleet{5, 0, 0, 0});
    const uint64_t slow = ticksToArrive(Fleet{5, 0, 0, 1});
    CHECK(slow > fast);
    // Отношение времён обязано примерно совпасть с отношением скоростей.
    const double ratio = double(slow) / double(fast);
    const double expected = kSpeedCorvette.toDouble() / kSpeedBattleship.toDouble();
    CHECK(ratio > expected * 0.85);
    CHECK(ratio < expected * 1.15);
}

TEST_CASE("движение: флот проходит через промежуточные системы") {
    Space space;
    const uint32_t target = 90;
    std::vector<uint32_t> path;
    REQUIRE(space.galaxy.findPath(0, target, path) > 1);

    const Entity fleet = space.spawnFleet(0, Fleet{3, 0, 0, 0});
    space.world.get<MoveOrder>(fleet)->target = target;

    std::vector<uint32_t> visited;
    for (uint64_t tick = 0; tick < 600000; ++tick) {
        TickContext context;
        context.tick = tick;
        systemFleetMovement(space.world, context);

        const FleetLocation* location = space.world.get<FleetLocation>(fleet);
        if (visited.empty() || visited.back() != location->system) {
            visited.push_back(location->system);
        }
        if (location->system == target) break;
    }

    // Флот обязан пройти именно по маршруту, а не телепортироваться.
    CHECK(visited.size() > 2);
    CHECK(visited.front() == 0u);
    CHECK(visited.back() == target);
    for (size_t i = 0; i + 1 < visited.size(); ++i) {
        CHECK(space.galaxy.laneLength(visited[i], visited[i + 1]) > fx::zero());
    }
}

TEST_CASE("движение: недостижимая цель снимает приказ") {
    Space space;
    const Entity fleet = space.spawnFleet(0, Fleet{2, 0, 0, 0});
    space.world.get<MoveOrder>(fleet)->target = 99999;  // такой системы нет

    space.run(5);
    // Приказ снят, а не висит молча: иначе игрок не поймёт, почему флот стоит.
    CHECK(space.world.get<MoveOrder>(fleet)->target == kNoSystem);
    CHECK(space.world.get<FleetLocation>(fleet)->system == 0u);
}

TEST_CASE("движение: пустой флот никуда не идёт") {
    Space space;
    const Entity fleet = space.spawnFleet(0, Fleet{0, 0, 0, 0});
    space.world.get<MoveOrder>(fleet)->target = 50;

    space.run(1000);
    CHECK(space.world.get<FleetLocation>(fleet)->system == 0u);
}

TEST_CASE("движение: без графа в ресурсах ничего не ломается") {
    // Тесты симуляции без карты — обычное дело, падать здесь нельзя.
    World world;
    registerGalaxyComponents(world);
    registerFleetComponents(world);

    const Entity fleet = world.create();
    world.add<Fleet>(fleet, Fleet{1, 0, 0, 0});
    world.add<FleetLocation>(fleet, FleetLocation{0, 0, fx::zero()});
    world.add<MoveOrder>(fleet, MoveOrder{5, 0});

    TickContext context;
    systemFleetMovement(world, context);  // не должно упасть
    CHECK(world.get<FleetLocation>(fleet)->system == 0u);
}

TEST_CASE("движение: воспроизводится тик в тик") {
    Space first(0xD44, 150), second(0xD44, 150);
    REQUIRE(first.world.hash() == second.world.hash());

    for (Space* space : {&first, &second}) {
        for (uint32_t i = 0; i < 40; ++i) {
            const Entity fleet = space->spawnFleet(i * 3,
                Fleet{i % 5, (i + 1) % 3, i % 2, (i % 7 == 0) ? 1u : 0u});
            space->world.get<MoveOrder>(fleet)->target = (i * 7 + 11) % 140;
        }
    }
    REQUIRE(first.world.hash() == second.world.hash());

    first.run(20000);
    second.run(20000);

    // Сорок флотов, три тысячи тиков, поиск пути на каждом прибытии —
    // и ни одного расхождения.
    CHECK(first.world.hash() == second.world.hash());
}
