#include "pw/sim/galaxy.h"

#include <algorithm>

#include "pw/core/hash.h"
#include "pw/core/rng.h"
#include "pw/core/trig.h"

namespace pw::sim {
namespace {

// Отдельный поток случайности на каждую задачу генерации. Общий поток
// сделал бы результат зависящим от порядка вызовов: добавили одну выборку
// в расстановку — и планеты по всей галактике стали другими.
constexpr uint64_t kStreamPlacement = 101;
constexpr uint64_t kStreamPlanets = 102;

/// Квадрат расстояния. Корень не берём: сравнивать квадраты дешевле,
/// а точность выше.
inline fx distanceSquared(fx ax, fx ay, fx bx, fx by) {
    const fx dx = ax - bx;
    const fx dy = ay - by;
    return dx * dx + dy * dy;
}

/// Система непересекающихся множеств для проверки и починки связности.
class DisjointSet {
public:
    explicit DisjointSet(uint32_t count) : parent_(count) {
        for (uint32_t i = 0; i < count; ++i) parent_[i] = i;
    }

    uint32_t find(uint32_t node) {
        while (parent_[node] != node) {
            parent_[node] = parent_[parent_[node]];  // сжатие пути
            node = parent_[node];
        }
        return node;
    }

    bool unite(uint32_t a, uint32_t b) {
        const uint32_t ra = find(a), rb = find(b);
        if (ra == rb) return false;
        // Меньший корень становится родителем: результат не зависит от
        // порядка объединений, а значит воспроизводим.
        if (ra < rb) parent_[rb] = ra; else parent_[ra] = rb;
        return true;
    }

private:
    std::vector<uint32_t> parent_;
};

}  // namespace

// ---------------------------------------------------------------------------

void registerGalaxyComponents(World& world) {
    // ПОРЯДОК ЗДЕСЬ — ЧАСТЬ КОНТРАКТА. Идентификаторы компонентов входят
    // в хеш мира; перестановка строк меняет хеш всей вселенной.
    world.registerComponent<StarSystem>("StarSystem");
    world.registerComponent<Planet>("Planet");
    world.registerComponent<Empire>("Empire");
    world.registerComponent<Owner>("Owner");
}

// ---------------------------------------------------------------------------
// Расстановка звёзд
// ---------------------------------------------------------------------------

void Galaxy::placeSystems(const GalaxyParams& params) {
    Rng rng(params.seed, kStreamPlacement);
    points_.clear();
    points_.reserve(params.systemCount);

    const fx minSpacingSquared = params.minSpacing * params.minSpacing;
    // Потолок попыток: при слишком тесных параметрах генерация обязана
    // закончиться меньшим числом систем, а не крутиться вечно.
    const uint32_t maxAttempts = params.systemCount * 60;

    const fx half = fx::fromFraction(1, 2);
    const uint32_t arms = params.arms == 0 ? 1 : params.arms;

    for (uint32_t attempt = 0;
         attempt < maxAttempts && points_.size() < params.systemCount; ++attempt) {
        // Доля радиуса напрямую, БЕЗ корня.
        //
        // Корень дал бы равномерную плотность по площади, и центр остался бы
        // пустым: маленькая площадь при равномерной плотности получает мало
        // систем. Дизайн требует обратного — плотное богатое ядро и просторный
        // фронтир. Линейная выборка даёт плотность, падающую как 1/r, ровно это.
        fx radial = rng.unit();

        // Разброс по радиусу. Без него рукав остаётся ниткой в один ряд
        // систем: соседи выстраиваются в цепочку, граф вырождается
        // в одномерный, и путь через галактику занимает десятки прыжков.
        radial += (rng.unit() - half) * params.radialJitter;
        radial = clamp(radial, fx::fromFraction(1, 40), fx::one());

        // Часть систем кладём вне рукавов — межрукавное поле.
        //
        // Оно связывает соседние рукава напрямую. Без него единственным
        // мостом между рукавами остаётся центр, весь трафик галактики идёт
        // через одну точку, и карта превращается в звезду из цепочек.
        fx angle;
        if (rng.below(100) < params.armFractionPercent) {
            const uint32_t arm = rng.below(arms);
            const fx armOffset = fx::fromFraction(int64_t(arm), int64_t(arms));
            // Закрутка на пол-оборота даёт узнаваемую спираль и неравномерную
            // плотность — источник естественных горлышек.
            const fx twist = radial * params.twist;
            const fx spread = (rng.unit() - half) * params.armSpread;
            angle = armOffset + twist + spread;
        } else {
            angle = rng.unit();  // равномерно по кругу
        }

        const fx distance = radial * params.radius;
        const fx x = cosTurns(angle) * distance;
        const fx y = sinTurns(angle) * distance;

        bool tooClose = false;
        for (const Point& other : points_) {
            if (distanceSquared(x, y, other.x, other.y) < minSpacingSquared) {
                tooClose = true;
                break;
            }
        }
        if (tooClose) continue;

        // Кольцо: ядро, промежуточные, фронтир. От него зависят богатство
        // системы и логистические штрафы.
        const fx quarter = params.radius / int64_t(4);
        uint8_t ring = 3;
        if (distance < quarter) ring = 0;
        else if (distance < quarter * fx::fromInt(2)) ring = 1;
        else if (distance < quarter * fx::fromInt(3)) ring = 2;

        points_.push_back(Point{x, y, ring});
    }
}

// ---------------------------------------------------------------------------
// Гиперлинии
// ---------------------------------------------------------------------------

void Galaxy::linkSystems(const GalaxyParams& params) {
    const uint32_t count = uint32_t(points_.size());
    offsets_.assign(count + 1, 0);
    adjacency_.clear();
    laneCount_ = 0;
    if (count == 0) return;

    // Рёбра как множество пар (меньший, больший) — каждая линия одна.
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    const fx maxLengthSquared = params.maxLaneLength * params.maxLaneLength;

    struct Candidate {
        fx distance;
        uint32_t index;
    };
    std::vector<Candidate> candidates;

    for (uint32_t i = 0; i < count; ++i) {
        candidates.clear();
        for (uint32_t j = 0; j < count; ++j) {
            if (i == j) continue;
            const fx d = distanceSquared(points_[i].x, points_[i].y,
                                         points_[j].x, points_[j].y);
            if (d <= maxLengthSquared) candidates.push_back(Candidate{d, j});
        }
        // Индекс — вторичный ключ сортировки. Без него две системы на
        // одинаковом расстоянии могли бы встать в любом порядке, и галактика
        // перестала бы воспроизводиться.
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.distance != b.distance) return a.distance < b.distance;
                      return a.index < b.index;
                  });

        const uint32_t take = std::min<uint32_t>(params.lanesPerSystem,
                                                 uint32_t(candidates.size()));
        for (uint32_t k = 0; k < take; ++k) {
            const uint32_t j = candidates[k].index;
            edges.emplace_back(std::min(i, j), std::max(i, j));
        }
    }

    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    // Починка связности. Несвязная галактика — это игроки, до которых
    // нельзя ни долететь, ни довоевать; такая карта просто сломана.
    DisjointSet sets(count);
    for (const auto& edge : edges) sets.unite(edge.first, edge.second);

    for (;;) {
        // Ищем ближайшую пару систем из разных компонент.
        fx best = fx::max();
        uint32_t bestA = 0, bestB = 0;
        bool found = false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t root = sets.find(i);
            for (uint32_t j = i + 1; j < count; ++j) {
                if (sets.find(j) == root) continue;
                const fx d = distanceSquared(points_[i].x, points_[i].y,
                                             points_[j].x, points_[j].y);
                if (!found || d < best) {
                    best = d;
                    bestA = i;
                    bestB = j;
                    found = true;
                }
            }
        }
        if (!found) break;  // всё уже в одной компоненте
        sets.unite(bestA, bestB);
        edges.emplace_back(bestA, bestB);
    }
    // Сборка сжатых строк из списка рёбер. Вызывается несколько раз:
    // проход срезок пересчитывает расстояния по уже построенному графу.
    auto rebuild = [&] {
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        laneCount_ = uint32_t(edges.size());

        std::vector<uint32_t> degree(count, 0);
        for (const auto& edge : edges) {
            ++degree[edge.first];
            ++degree[edge.second];
        }
        uint32_t running = 0;
        for (uint32_t i = 0; i < count; ++i) {
            offsets_[i] = running;
            running += degree[i];
        }
        offsets_[count] = running;

        adjacency_.assign(running, 0);
        std::vector<uint32_t> cursor(offsets_.begin(), offsets_.end() - 1);
        for (const auto& edge : edges) {
            adjacency_[cursor[edge.first]++] = edge.second;
            adjacency_[cursor[edge.second]++] = edge.first;
        }
        // Соседи по возрастанию — обход становится стабильным.
        for (uint32_t i = 0; i < count; ++i) {
            std::sort(adjacency_.begin() + offsets_[i], adjacency_.begin() + offsets_[i + 1]);
        }
    };
    rebuild();

    // -----------------------------------------------------------------------
    // Проход срезок
    //
    // Всепарные расстояния считаются обходом в ширину из каждой вершины.
    // Для сезонной галактики в тысячи систем это доли секунды и делается
    // один раз при генерации. Для безграничной галактики Фазы 4 подход
    // будет другим — там связи строятся по регионам, — поэтому проход
    // отключается на больших картах, а не тормозит молча.
    // -----------------------------------------------------------------------
    constexpr uint32_t kShortcutLimit = 4000;
    if (count <= kShortcutLimit && params.shortcutRounds > 0) {
        const fx shortcutRange =
            params.maxLaneLength * fx::fromFraction(int64_t(params.shortcutRangePercent), 100);
        const fx shortcutRangeSquared = shortcutRange * shortcutRange;

        std::vector<uint16_t> hops(size_t(count) * count);
        std::vector<uint32_t> queue;
        queue.reserve(count);

        struct Shortcut {
            uint16_t hops;
            fx distance;
            uint32_t a, b;
        };
        std::vector<Shortcut> shortcuts;

        for (uint32_t round = 0; round < params.shortcutRounds; ++round) {
            // Расстояния в прыжках от каждой системы.
            constexpr uint16_t kUnreached = 0xFFFF;
            std::fill(hops.begin(), hops.end(), kUnreached);
            for (uint32_t start = 0; start < count; ++start) {
                uint16_t* row = hops.data() + size_t(start) * count;
                queue.clear();
                queue.push_back(start);
                row[start] = 0;
                for (size_t head = 0; head < queue.size(); ++head) {
                    const uint32_t node = queue[head];
                    const uint16_t next = uint16_t(row[node] + 1);
                    for (uint32_t k = offsets_[node]; k < offsets_[node + 1]; ++k) {
                        const uint32_t neighbour = adjacency_[k];
                        if (row[neighbour] != kUnreached) continue;
                        row[neighbour] = next;
                        queue.push_back(neighbour);
                    }
                }
            }

            shortcuts.clear();
            for (uint32_t i = 0; i < count; ++i) {
                const uint16_t* row = hops.data() + size_t(i) * count;
                for (uint32_t j = i + 1; j < count; ++j) {
                    if (row[j] <= params.shortcutHopThreshold) continue;
                    const fx d = distanceSquared(points_[i].x, points_[i].y,
                                                 points_[j].x, points_[j].y);
                    if (d > shortcutRangeSquared) continue;
                    shortcuts.push_back(Shortcut{row[j], d, i, j});
                }
            }
            if (shortcuts.empty()) break;

            // Сначала самые вопиющие: далеко по графу и близко по карте.
            // Индексы в ключе сортировки — чтобы порядок был полным
            // и результат воспроизводился.
            std::sort(shortcuts.begin(), shortcuts.end(),
                      [](const Shortcut& a, const Shortcut& b) {
                          if (a.hops != b.hops) return a.hops > b.hops;
                          if (a.distance != b.distance) return a.distance < b.distance;
                          if (a.a != b.a) return a.a < b.a;
                          return a.b < b.b;
                      });

            // За проход добавляем немного: иначе одним махом наставим лишних
            // связей там, где хватило бы двух, и горлышки исчезнут.
            const size_t budget = std::max<size_t>(4, count / 25);
            const size_t take = std::min(budget, shortcuts.size());
            for (size_t k = 0; k < take; ++k) {
                edges.emplace_back(shortcuts[k].a, shortcuts[k].b);
            }
            rebuild();
        }
    }
}

// ---------------------------------------------------------------------------
// Планеты
// ---------------------------------------------------------------------------

void Galaxy::spawnPlanets(World& world, const GalaxyParams& params) {
    for (uint32_t index = 0; index < systems_.size(); ++index) {
        // Содержимое системы выводится из её координат в списке и сида сезона.
        // Тот же приём, что и в безграничной галактике на Фазе 4: система
        // не хранится, она вычисляется.
        const uint64_t systemSeed = mixCoord(params.seed, int64_t(index), 0x51A7);
        Rng rng(systemSeed, kStreamPlanets);

        StarSystem* star = world.get<StarSystem>(systems_[index]);

        // Класс звезды. Чем ближе к ядру, тем выше шанс на редкое светило —
        // так центр карты становится дорогой недвижимостью.
        const uint32_t roll = rng.below(100);
        StarClass starClass = StarClass::Red;
        const bool core = star->ring <= 1;
        if (roll < (core ? 3u : 1u))        starClass = StarClass::BlackHole;
        else if (roll < (core ? 9u : 4u))   starClass = StarClass::Neutron;
        else if (roll < (core ? 34u : 22u)) starClass = StarClass::Blue;
        else if (roll < (core ? 70u : 62u)) starClass = StarClass::Yellow;
        star->seed = uint32_t(systemSeed);
        star->starClass = uint8_t(starClass);

        // Число планет. У чёрных дыр их нет — система ценна сама по себе.
        uint32_t planets = 0;
        if (starClass != StarClass::BlackHole) {
            planets = uint32_t(rng.range(1, star->ring == 0 ? 6 : 4));
        }
        star->planetCount = uint8_t(planets);

        for (uint32_t orbit = 0; orbit < planets; ++orbit) {
            const uint32_t classRoll = rng.below(uint32_t(PlanetClass::Count));
            const auto planetClass = PlanetClass(classRoll);

            // Слоты зависят от класса: газовые гиганты просторные,
            // выжженные камни тесные. Это и есть основа экономики (ADR-004).
            uint8_t slots = 4;
            switch (planetClass) {
                case PlanetClass::GasGiant:     slots = uint8_t(rng.range(8, 12)); break;
                case PlanetClass::Ocean:        slots = uint8_t(rng.range(6, 10)); break;
                case PlanetClass::Desert:       slots = uint8_t(rng.range(5, 9)); break;
                case PlanetClass::Volcanic:     slots = uint8_t(rng.range(4, 7)); break;
                case PlanetClass::AsteroidBelt: slots = uint8_t(rng.range(2, 5)); break;
                default:                        slots = uint8_t(rng.range(3, 6)); break;
            }

            const Entity planet = world.create();
            world.add<Planet>(planet, Planet{index, uint8_t(planetClass), slots,
                                             /*specialization=*/0, uint8_t(orbit)});
        }
    }
}

// ---------------------------------------------------------------------------

void Galaxy::generate(World& world, const GalaxyParams& params) {
    placeSystems(params);
    linkSystems(params);

    systems_.clear();
    systems_.reserve(points_.size());
    for (uint32_t index = 0; index < points_.size(); ++index) {
        const Entity entity = world.create();
        world.add<StarSystem>(entity, StarSystem{points_[index].x, points_[index].y,
                                                 /*seed=*/0, index,
                                                 uint8_t(StarClass::Red),
                                                 /*planetCount=*/0, points_[index].ring,
                                                 /*reserved=*/0, /*reserved2=*/0});
        systems_.push_back(entity);
    }

    spawnPlanets(world, params);
}

bool Galaxy::connected() const {
    const uint32_t count = systemCount();
    if (count <= 1) return true;

    std::vector<bool> seen(count, false);
    std::vector<uint32_t> stack{0};
    seen[0] = true;
    uint32_t reached = 1;

    while (!stack.empty()) {
        const uint32_t node = stack.back();
        stack.pop_back();
        for (uint32_t k = 0; k < neighborCount(node); ++k) {
            const uint32_t next = neighbors(node)[k];
            if (seen[next]) continue;
            seen[next] = true;
            ++reached;
            stack.push_back(next);
        }
    }
    return reached == count;
}

int32_t Galaxy::hopDistance(uint32_t from, uint32_t to) const {
    const uint32_t count = systemCount();
    if (from >= count || to >= count) return -1;
    if (from == to) return 0;

    std::vector<int32_t> depth(count, -1);
    std::vector<uint32_t> queue;
    queue.reserve(count);
    queue.push_back(from);
    depth[from] = 0;

    for (size_t head = 0; head < queue.size(); ++head) {
        const uint32_t node = queue[head];
        for (uint32_t k = 0; k < neighborCount(node); ++k) {
            const uint32_t next = neighbors(node)[k];
            if (depth[next] >= 0) continue;
            depth[next] = depth[node] + 1;
            if (next == to) return depth[next];
            queue.push_back(next);
        }
    }
    return -1;
}

uint64_t Galaxy::hash() const {
    Hasher hasher;
    hasher.u64(points_.size()).u64(laneCount_);
    for (const Point& point : points_) {
        hasher.i64(point.x.raw()).i64(point.y.raw()).u32(point.ring);
    }
    for (const uint32_t neighbour : adjacency_) hasher.u32(neighbour);
    return hasher.value();
}

// ---------------------------------------------------------------------------
// Поиск пути
// ---------------------------------------------------------------------------

fx Galaxy::straightDistance(uint32_t a, uint32_t b) const {
    if (a >= points_.size() || b >= points_.size()) return fx::zero();
    return sqrt(distanceSquared(points_[a].x, points_[a].y, points_[b].x, points_[b].y));
}

fx Galaxy::laneLength(uint32_t a, uint32_t b) const {
    if (a >= systemCount()) return fx::zero();
    for (uint32_t k = offsets_[a]; k < offsets_[a + 1]; ++k) {
        if (adjacency_[k] == b) return straightDistance(a, b);
    }
    return fx::zero();
}

int32_t Galaxy::findPath(uint32_t from, uint32_t to, std::vector<uint32_t>& out) const {
    out.clear();
    const uint32_t count = systemCount();
    if (from >= count || to >= count) return -1;
    if (from == to) {
        out.push_back(from);
        return 0;
    }

    // A* с оценкой по прямой. Оценка не завышает: путь по гиперлиниям не
    // бывает короче отрезка между системами, поэтому найденный маршрут
    // гарантированно кратчайший, а не просто похожий на кратчайший.
    std::vector<fx> best(count, fx::max());
    std::vector<uint32_t> cameFrom(count, UINT32_MAX);
    std::vector<bool> closed(count, false);

    struct Node {
        fx estimate;   // пройдено плюс оценка остатка
        fx travelled;
        uint32_t index;
    };
    // Куча по возрастанию оценки. Индекс в ключе сравнения обязателен:
    // без него две равные оценки могли бы разложиться в любом порядке,
    // и маршрут перестал бы воспроизводиться.
    const auto worse = [](const Node& a, const Node& b) {
        if (a.estimate != b.estimate) return a.estimate > b.estimate;
        if (a.travelled != b.travelled) return a.travelled > b.travelled;
        return a.index > b.index;
    };

    std::vector<Node> open;
    open.push_back(Node{straightDistance(from, to), fx::zero(), from});
    best[from] = fx::zero();

    while (!open.empty()) {
        std::pop_heap(open.begin(), open.end(), worse);
        const Node current = open.back();
        open.pop_back();

        if (current.index == to) break;
        if (closed[current.index]) continue;
        closed[current.index] = true;

        for (uint32_t k = offsets_[current.index]; k < offsets_[current.index + 1]; ++k) {
            const uint32_t neighbour = adjacency_[k];
            if (closed[neighbour]) continue;

            const fx travelled = current.travelled + straightDistance(current.index, neighbour);
            if (travelled >= best[neighbour]) continue;

            best[neighbour] = travelled;
            cameFrom[neighbour] = current.index;
            open.push_back(Node{travelled + straightDistance(neighbour, to), travelled, neighbour});
            std::push_heap(open.begin(), open.end(), worse);
        }
    }

    if (cameFrom[to] == UINT32_MAX) return -1;

    for (uint32_t node = to; node != UINT32_MAX; node = cameFrom[node]) {
        out.push_back(node);
        if (node == from) break;
    }
    std::reverse(out.begin(), out.end());
    return int32_t(out.size()) - 1;
}

int32_t Galaxy::nextHop(uint32_t from, uint32_t to) const {
    if (from == to) return int32_t(from);
    std::vector<uint32_t> path;
    if (findPath(from, to, path) < 1) return -1;
    return int32_t(path[1]);
}

}  // namespace pw::sim
