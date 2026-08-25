// pw_determinism_check — контрольный прогон детерминизма.
//
// Это НЕ юнит-тест, а инструмент кросс-платформенной сверки. CI запускает
// этот бинарь на Linux/x86-64, Windows/x86-64, macOS/arm64 и Android/arm64
// и сверяет напечатанные хеши. Расхождение хотя бы в одном разряде означает,
// что симуляция разъехалась между платформами — а значит, сломаны клиентское
// предсказание, реплеи, валидация матчей и восстановление ноды из журнала.
//
// Прогон намеренно бьёт по самым рискованным местам: умножение и деление
// Q32.32 через 128-битный промежуточный результат, корень по биту, таблицы
// синуса и atan2, поток PCG32.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pw/core/hash.h"
#include "pw/core/rng.h"
#include "pw/core/trig.h"

using namespace pw;

namespace {

constexpr int kFleets = 512;
constexpr int kTicks = 5000;
constexpr int kHashEvery = 500;

// Эталон, посчитанный на Linux/x86-64 и зафиксированный в репозитории.
// Обновлять его допустимо ТОЛЬКО вместе с осознанным изменением правил
// симуляции — и тогда меняется каждый расчёт в игре.
constexpr uint64_t kGolden = 0xEAAE7188626914F5ull;

struct Fleet {
    fx x, y;         // положение в галактике
    fx tx, ty;       // текущая цель
    fx speed;        // скорость за тик
    uint32_t owner;  // империя-владелец
};

/// Мини-симуляция: флоты идут к целям, по достижении берут новую.
/// Единственный источник случайности — явный поток PCG32.
uint64_t runSimulation() {
    Rng spawn(0xC0FFEE, /*stream=*/1);
    Rng retarget(0xC0FFEE, /*stream=*/2);

    std::vector<Fleet> fleets(kFleets);
    for (int i = 0; i < kFleets; ++i) {
        Fleet& f = fleets[size_t(i)];
        f.x = fx::fromInt(spawn.range(-2000, 2000));
        f.y = fx::fromInt(spawn.range(-2000, 2000));
        f.tx = fx::fromInt(spawn.range(-2000, 2000));
        f.ty = fx::fromInt(spawn.range(-2000, 2000));
        // Скорость — точная дробь, а не результат деления с плавающей точкой.
        f.speed = fx::fromFraction(spawn.range(5, 40), 10);
        f.owner = spawn.below(16);
    }

    Hasher rolling;

    for (int tick = 0; tick < kTicks; ++tick) {
        for (int i = 0; i < kFleets; ++i) {
            Fleet& f = fleets[size_t(i)];

            const fx dx = f.tx - f.x;
            const fx dy = f.ty - f.y;
            const fx dist = length(dx, dy);

            if (dist <= f.speed) {
                // Цель достигнута — встаём на неё и выбираем следующую.
                f.x = f.tx;
                f.y = f.ty;
                f.tx = fx::fromInt(retarget.range(-2000, 2000));
                f.ty = fx::fromInt(retarget.range(-2000, 2000));
                continue;
            }

            const fx heading = atan2Turns(dy, dx);
            f.x += cosTurns(heading) * f.speed;
            f.y += sinTurns(heading) * f.speed;
        }

        if ((tick + 1) % kHashEvery == 0) {
            Hasher snapshot;
            for (const Fleet& f : fleets) {
                snapshot.i64(f.x.raw()).i64(f.y.raw()).i64(f.tx.raw()).i64(f.ty.raw());
                snapshot.i64(f.speed.raw()).u32(f.owner);
            }
            std::printf("тик %5d  хеш состояния  %016llX\n", tick + 1,
                        static_cast<unsigned long long>(snapshot.value()));
            rolling.u64(snapshot.value());
        }
    }

    return rolling.value();
}

}  // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::strcmp(argv[1], "--print-only") == 0;

    std::printf("pw_determinism_check — %d флотов, %d тиков\n", kFleets, kTicks);
    const uint64_t result = runSimulation();
    std::printf("\nитоговый хеш   %016llX\n", static_cast<unsigned long long>(result));
    std::printf("эталон         %016llX\n", static_cast<unsigned long long>(kGolden));

    if (update) {
        std::printf("\nрежим печати: сверка пропущена\n");
        return 0;
    }
    if (result != kGolden) {
        std::printf("\nРАСХОЖДЕНИЕ. Симуляция не воспроизводится побитово.\n");
        std::printf("Либо правила изменились осознанно (обновите kGolden),\n");
        std::printf("либо платформа считает иначе — и это баг детерминизма.\n");
        return 1;
    }
    std::printf("\nсовпадение: симуляция детерминирована\n");
    return 0;
}
