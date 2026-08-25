#include "doctest.h"

#include "pw/core/fixed.h"
#include "pw/core/rng.h"
#include "pw/core/trig.h"
#include "pw/sim/schedule.h"

using namespace pw;
using namespace pw::sim;

namespace {

struct Position {
    fx x, y;
};
struct Velocity {
    fx dx, dy;
};
struct Target {
    fx x, y;
};
struct Speed {
    fx value;
};

void registerAll(World& world) {
    world.registerComponent<Position>("Position");
    world.registerComponent<Velocity>("Velocity");
    world.registerComponent<Target>("Target");
    world.registerComponent<Speed>("Speed");
}

// --- системы ---------------------------------------------------------------

void systemSteer(World& world, const TickContext&) {
    // Флот разворачивается на свою цель. Курс считается через atan2 в оборотах.
    world.each<Position, Target, Speed, Velocity>(
        [](Entity, Position& p, Target& t, Speed& s, Velocity& v) {
            const fx dx = t.x - p.x;
            const fx dy = t.y - p.y;
            if (dx == fx::zero() && dy == fx::zero()) {
                v.dx = fx::zero();
                v.dy = fx::zero();
                return;
            }
            const fx heading = atan2Turns(dy, dx);
            v.dx = cosTurns(heading) * s.value;
            v.dy = sinTurns(heading) * s.value;
        });
}

void systemMove(World& world, const TickContext& ctx) {
    world.each<Position, Velocity>([&ctx](Entity, Position& p, Velocity& v) {
        p.x += v.dx * ctx.delta;
        p.y += v.dy * ctx.delta;
    });
}

int gCallOrder = 0;
int gFirstRan = 0;
int gSecondRan = 0;

void systemFirst(World&, const TickContext&) { gFirstRan = ++gCallOrder; }
void systemSecond(World&, const TickContext&) { gSecondRan = ++gCallOrder; }

/// Собрать одинаковую партию: флоты со случайными позициями и целями.
void populate(Simulation& sim, uint64_t seed) {
    Rng rng(seed, /*stream=*/1);
    for (int i = 0; i < 200; ++i) {
        const Entity e = sim.world().create();
        sim.world().add<Position>(e, {fx::fromInt(rng.range(-500, 500)),
                                      fx::fromInt(rng.range(-500, 500))});
        sim.world().add<Target>(e, {fx::fromInt(rng.range(-500, 500)),
                                    fx::fromInt(rng.range(-500, 500))});
        sim.world().add<Speed>(e, {fx::fromFraction(rng.range(10, 60), 10)});
        sim.world().add<Velocity>(e, {fx::zero(), fx::zero()});
    }
}

}  // namespace

TEST_CASE("расписание: системы выполняются в порядке добавления") {
    gCallOrder = 0;
    Simulation sim;
    registerAll(sim.world());

    sim.schedule().add("first", systemFirst);
    sim.schedule().add("second", systemSecond);
    CHECK(sim.schedule().systemCount() == 2);

    sim.step();
    CHECK(gFirstRan == 1);
    CHECK(gSecondRan == 2);
}

TEST_CASE("расписание: тик растёт, время считается точно") {
    Simulation sim;
    CHECK(sim.tick() == 0);
    sim.advance(25);
    CHECK(sim.tick() == 25);

    TickContext ctx;
    ctx.tick = 25;
    // 25 тиков при 10 в секунду — ровно 2.5 секунды, без накопления ошибки.
    CHECK(ctx.elapsed() == fx::fromFraction(5, 2));
}

TEST_CASE("расписание: пустая система не ломает тик") {
    Simulation sim;
    sim.schedule().add("null", nullptr);
    CHECK(sim.schedule().systemCount() == 0);
    sim.step();
    CHECK(sim.tick() == 1);
}

TEST_CASE("симуляция: флоты доходят до своих целей") {
    Simulation sim;
    registerAll(sim.world());
    sim.schedule().add("steer", systemSteer);
    sim.schedule().add("move", systemMove);

    const Entity fleet = sim.world().create();
    sim.world().add<Position>(fleet, {fx::zero(), fx::zero()});
    sim.world().add<Target>(fleet, {fx::fromInt(100), fx::fromInt(0)});
    sim.world().add<Speed>(fleet, {fx::fromInt(10)});
    sim.world().add<Velocity>(fleet, {fx::zero(), fx::zero()});

    // Скорость 10 в секунду, расстояние 100 — примерно 10 секунд, 100 тиков.
    sim.advance(100);

    const Position* p = sim.world().get<Position>(fleet);
    REQUIRE(p != nullptr);
    CHECK(abs(p->x - fx::fromInt(100)).floorToInt() <= 1);
    CHECK(abs(p->y).floorToInt() <= 1);
}

// ---------------------------------------------------------------------------
// Главное: воспроизводимость целой симуляции
// ---------------------------------------------------------------------------

TEST_CASE("симуляция: два одинаковых прогона дают одинаковый хеш") {
    Simulation first, second;
    for (Simulation* sim : {&first, &second}) {
        registerAll(sim->world());
        sim->schedule().add("steer", systemSteer);
        sim->schedule().add("move", systemMove);
        populate(*sim, 0xBEEF);
    }
    REQUIRE(first.hash() == second.hash());

    first.advance(500);
    second.advance(500);

    // Пятьсот тиков по двумстам флотам: сто тысяч вычислений atan2, синуса,
    // косинуса и умножений с фиксированной точкой. Совпадение хеша означает,
    // что ни одно из них не разошлось.
    CHECK(first.hash() == second.hash());
}

TEST_CASE("симуляция: разбиение на шаги не влияет на результат") {
    Simulation whole, pieces;
    for (Simulation* sim : {&whole, &pieces}) {
        registerAll(sim->world());
        sim->schedule().add("steer", systemSteer);
        sim->schedule().add("move", systemMove);
        populate(*sim, 0x1234);
    }

    whole.advance(300);
    // Тот же интервал, но кусками — как это и будет на живом сервере,
    // где тики идут по мере поступления времени.
    for (int i = 0; i < 30; ++i) pieces.advance(10);

    CHECK(whole.hash() == pieces.hash());
    CHECK(whole.tick() == pieces.tick());
}

TEST_CASE("симуляция: расхождение в одном разряде заметно по хешу") {
    Simulation first, second;
    for (Simulation* sim : {&first, &second}) {
        registerAll(sim->world());
        sim->schedule().add("steer", systemSteer);
        sim->schedule().add("move", systemMove);
        populate(*sim, 0x77);
    }
    first.advance(50);
    second.advance(50);
    REQUIRE(first.hash() == second.hash());

    // Сдвигаем один флот на младший разряд Q32.32 — около 2.3e-10 игровой
    // единицы. Незамеченное расхождение такого масштаба и есть начало
    // рассинхронизации клиента с сервером.
    second.world().each<Position>([](Entity e, Position& p) {
        if (e.index == 42) p.x += fx::epsilon();
    });
    CHECK(first.hash() != second.hash());

    // И оно не «рассасывается» с ходом времени, а расходится дальше.
    first.advance(50);
    second.advance(50);
    CHECK(first.hash() != second.hash());
}
