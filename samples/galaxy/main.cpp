// pw_galaxy — предпросмотр процедурной галактики.
//
// Рисует карту в PNG и печатает статистику. Нужен ровно потому, что
// процедурная генерация — область, где все тесты зелёные, а результат при
// этом никуда не годится. Автотест проверяет, что граф связен и звёзды не
// слипаются; он не проверяет, что галактика похожа на галактику.
//
// Первая версия генератора кораблей это уже показала: она строила флот,
// летящий кормой вперёд, и ни одна проверка не сработала.
//
// Инструменту не нужны ни видеокарта, ни SDL — только pw_sim и pw_core.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pw/core/png.h"
#include "pw/sim/galaxy.h"

using namespace pw;
using namespace pw::sim;

namespace {

constexpr Rgba8 kSpace{10, 14, 25, 255};
constexpr Rgba8 kLane{40, 52, 78, 255};

/// Цвета светил. Янтарь у чёрной дыры — акцент палитры проекта: она
/// и должна выделяться, это самая ценная недвижимость на карте.
Rgba8 colorOf(StarClass starClass) {
    switch (starClass) {
        case StarClass::Red:       return Rgba8{196, 96, 84, 255};
        case StarClass::Yellow:    return Rgba8{236, 214, 168, 255};
        case StarClass::Blue:      return Rgba8{150, 190, 246, 255};
        case StarClass::Neutron:   return Rgba8{190, 250, 246, 255};
        case StarClass::BlackHole: return Rgba8{232, 163, 61, 255};
        default:                   return Rgba8{160, 160, 160, 255};
    }
}

struct Canvas {
    int width, height;
    std::vector<Rgba8> pixels;

    Canvas(int w, int h) : width(w), height(h), pixels(size_t(w) * size_t(h), kSpace) {}

    void blend(int x, int y, Rgba8 color, float alpha) {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        if (alpha <= 0.0f) return;
        if (alpha > 1.0f) alpha = 1.0f;
        Rgba8& dst = pixels[size_t(y) * size_t(width) + size_t(x)];
        auto mix = [alpha](uint8_t a, uint8_t b) {
            return uint8_t(float(a) * (1.0f - alpha) + float(b) * alpha + 0.5f);
        };
        dst.r = mix(dst.r, color.r);
        dst.g = mix(dst.g, color.g);
        dst.b = mix(dst.b, color.b);
    }

    /// Отрезок по Брезенхэму. Линии слабые: важен рисунок связей, а не они сами.
    void line(int x0, int y0, int x1, int y1, Rgba8 color, float alpha) {
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            blend(x0, y0, color, alpha);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    /// Звезда с ореолом: ядро плюс мягкое свечение вокруг.
    void star(int cx, int cy, float radius, Rgba8 color) {
        const int reach = int(radius * 3.0f) + 1;
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const float distance = float(dx * dx + dy * dy);
                const float falloff = radius * radius * 2.4f;
                float alpha = 1.0f - distance / falloff;
                if (alpha <= 0.0f) continue;
                alpha *= alpha * 0.85f;
                blend(cx + dx, cy + dy, color, alpha);
            }
        }
    }
};

const char* starName(StarClass c) {
    switch (c) {
        case StarClass::Red:       return "красный карлик";
        case StarClass::Yellow:    return "жёлтая";
        case StarClass::Blue:      return "голубой гигант";
        case StarClass::Neutron:   return "нейтронная";
        case StarClass::BlackHole: return "чёрная дыра";
        default:                   return "?";
    }
}

}  // namespace

int main(int argc, char** argv) {
    GalaxyParams params;
    std::string outPath = "galaxy.png";
    int imageSize = 1400;
    bool quiet = false;  // только сводная строка: удобно для развёрток

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            params.seed = std::strtoull(argv[++i], nullptr, 0);
        } else if (arg == "--systems" && i + 1 < argc) {
            params.systemCount = uint32_t(std::atoi(argv[++i]));
        } else if (arg == "--arms" && i + 1 < argc) {
            params.arms = uint32_t(std::atoi(argv[++i]));
        } else if (arg == "--out" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            imageSize = std::atoi(argv[++i]);
        } else if (arg == "--lanes" && i + 1 < argc) {
            params.lanesPerSystem = uint32_t(std::atoi(argv[++i]));
        } else if (arg == "--arm-percent" && i + 1 < argc) {
            params.armFractionPercent = uint32_t(std::atoi(argv[++i]));
        } else if (arg == "--spread" && i + 1 < argc) {
            // Задаётся знаменателем: 9 означает 1/9 оборота.
            params.armSpread = fx::fromFraction(1, std::atoi(argv[++i]));
        } else if (arg == "--jitter" && i + 1 < argc) {
            params.radialJitter = fx::fromFraction(1, std::atoi(argv[++i]));
        } else if (arg == "--quiet") {
            quiet = true;
        } else {
            std::printf(
                "pw_galaxy — предпросмотр процедурной галактики\n\n"
                "  --seed <n>      сид сезона\n"
                "  --systems <n>   сколько систем\n"
                "  --arms <n>      спиральных рукавов\n"
                "  --lanes <n>     гиперлиний на систему\n"
                "  --arm-percent <n>  доля систем в рукавах\n"
                "  --spread <n>    разброс поперёк рукава, 1/n оборота\n"
                "  --jitter <n>    разброс по радиусу, 1/n\n"
                "  --quiet         только сводная строка\n"
                "  --size <n>      сторона картинки в пикселях\n"
                "  --out <файл>    куда сохранить\n");
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
    }

    World world;
    registerGalaxyComponents(world);

    Galaxy galaxy;
    galaxy.generate(world, params);

    // --- статистика ---
    const uint32_t count = galaxy.systemCount();
    std::map<uint8_t, int> byClass, byRing;
    int planets = 0, slots = 0;
    world.each<StarSystem>([&](Entity, StarSystem& s) {
        ++byClass[s.starClass];
        ++byRing[s.ring];
    });
    world.each<Planet>([&](Entity, Planet& p) {
        ++planets;
        slots += p.slots;
    });

    uint32_t degreeSum = 0, maxDegree = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t degree = galaxy.neighborCount(i);
        degreeSum += degree;
        if (degree > maxDegree) maxDegree = degree;
    }

    int32_t diameter = 0;
    if (count > 2 && galaxy.connected()) {
        for (uint32_t target = 0; target < count; ++target) {
            const int32_t hops = galaxy.hopDistance(0, target);
            if (hops > diameter) diameter = hops;
        }
    }

    if (quiet) {
        std::printf("рукава %3u%%  линий/сист %u  разброс 1/%-3lld  "
                    "-> систем %3u  линий %4u  степень %.2f  диаметр %2d  %s\n",
                    params.armFractionPercent, params.lanesPerSystem,
                    static_cast<long long>((fx::one() / params.armSpread).floorToInt()),
                    count, galaxy.laneCount(),
                    double(degreeSum) / double(count ? count : 1), diameter,
                    galaxy.connected() ? "цельная" : "РАЗОРВАНА");
        return 0;
    }

    std::printf("Галактика, сид 0x%llX\n", static_cast<unsigned long long>(params.seed));
    std::printf("  систем        %u из %u запрошенных\n", count, params.systemCount);
    std::printf("  гиперлиний    %u, средняя степень %.2f, максимум %u\n",
                galaxy.laneCount(), double(degreeSum) / double(count ? count : 1), maxDegree);
    std::printf("  связность     %s\n", galaxy.connected() ? "цельная" : "РАЗОРВАНА");
    std::printf("  планет        %d, слотов под здания %d\n", planets, slots);

    std::printf("  светила:      ");
    for (const auto& [cls, n] : byClass) {
        std::printf("%s %d (%.0f%%)  ", starName(StarClass(cls)), n,
                    100.0 * double(n) / double(count));
    }
    std::printf("\n  кольца:       ");
    for (const auto& [ring, n] : byRing) {
        std::printf("%s %d  ", ring == 0 ? "ядро" : (ring == 3 ? "фронтир" : "кольцо"), n);
    }
    std::printf("\n");

    // Диаметр карты в прыжках прямо задаёт темп войны: по нему настраивается
    // и размер галактики, и скорость флотов.
    std::printf("  диаметр       %d прыжков\n", diameter);

    // --- отрисовка ---
    Canvas canvas(imageSize, imageSize);
    const double extent = params.radius.toDouble() * 1.08;
    const double scale = double(imageSize) * 0.5 / extent;
    auto toPixel = [&](fx v) { return int(v.toDouble() * scale + double(imageSize) * 0.5); };

    // Сначала связи, чтобы звёзды легли поверх.
    for (uint32_t i = 0; i < count; ++i) {
        const StarSystem* a = world.get<StarSystem>(galaxy.systemEntity(i));
        for (uint32_t k = 0; k < galaxy.neighborCount(i); ++k) {
            const uint32_t j = galaxy.neighbors(i)[k];
            if (j < i) continue;  // каждое ребро один раз
            const StarSystem* b = world.get<StarSystem>(galaxy.systemEntity(j));
            canvas.line(toPixel(a->x), toPixel(a->y), toPixel(b->x), toPixel(b->y),
                        kLane, 0.55f);
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        const StarSystem* s = world.get<StarSystem>(galaxy.systemEntity(i));
        const auto starClass = StarClass(s->starClass);
        // Редкие светила крупнее: их видно на карте, и это правильно —
        // именно за них будут воевать.
        float radius = 1.6f;
        if (starClass == StarClass::Blue) radius = 2.2f;
        if (starClass == StarClass::Neutron) radius = 2.6f;
        if (starClass == StarClass::BlackHole) radius = 3.4f;
        canvas.star(toPixel(s->x), toPixel(s->y), radius, colorOf(starClass));
    }

    if (!writePng(outPath, canvas.pixels, imageSize, imageSize)) {
        std::printf("не удалось записать %s\n", outPath.c_str());
        return 1;
    }
    std::printf("\nкарта сохранена: %s (%dx%d)\n", outPath.c_str(), imageSize, imageSize);
    return 0;
}
