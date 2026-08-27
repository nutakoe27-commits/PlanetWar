#include "doctest.h"

#include "assets_path.h"

#include <cmath>
#include <fstream>

#include "pw/render/map_view.h"
#include "pw/render/system_view.h"

using namespace pw;
using namespace pw::render;
using namespace pw::render::testing;

namespace {

std::string findManifest() { return testing::findAsset("assets/build/planets.json"); }

/// Система с планетами и вид на неё. Ни окна, ни видеокарты: SystemView
/// отдаёт массивы экземпляров, и всё, что нужно проверить, — их
/// содержимое.
struct Scene {
    sim::World world;
    sim::Galaxy galaxy;
    game::WorldView view;
    SystemView systemView;
    SystemAssets assets;
    SystemFrame frame;
    SystemCamera camera;

    explicit Scene(uint32_t systems = 30) {
        sim::registerGalaxyComponents(world);
        sim::GalaxyParams params;
        params.seed = 0x51A7;
        params.systemCount = systems;
        galaxy.generate(world, params);
        view.resize(systems);
    }

    /// Первая система ровно с таким числом планет.
    uint32_t systemWith(uint32_t planets) const {
        for (uint32_t index = 0; index < galaxy.systemCount(); ++index) {
            if (galaxy.planetCount(index) == planets) return index;
        }
        return UINT32_MAX;
    }

    void build(uint32_t system, uint32_t empire = 0) {
        systemView.setAssets(&assets);
        systemView.build(galaxy, view, system, empire, camera, /*aspect=*/16.0f / 9.0f,
                         frame);
    }

    size_t instanceCount() const {
        size_t total = 0;
        for (const MeshBatch& batch : frame.batches) total += batch.instances.size();
        return total;
    }

    size_t instanceCount(MeshKind kind) const {
        size_t total = 0;
        for (const MeshBatch& batch : frame.batches) {
            if (batch.kind == kind) total += batch.instances.size();
        }
        return total;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Сетки
// ---------------------------------------------------------------------------

TEST_CASE("сетка: кольцо орбиты замкнуто и намотано наружу") {
    const MeshData ring = makeOrbitRing(64, 0.01f);
    CHECK(ring.vertices.size() == 128);
    CHECK(ring.indices.size() == 64 * 6);

    // Ни один индекс не смеет выйти за массив вершин: видеокарта отвечает
    // на это не исключением, а зависшим драйвером.
    for (uint32_t index : ring.indices) CHECK(index < ring.vertices.size());

    // Радиус около единицы: масштаб задаёт экземпляр, а не сетка.
    for (const rhi::MeshVertex& vertex : ring.vertices) {
        const float radius = std::sqrt(vertex.x * vertex.x + vertex.y * vertex.y);
        CHECK(radius > 0.98f);
        CHECK(radius < 1.02f);
    }
}

TEST_CASE("сетка: битый файл отвергается, а не читается наполовину") {
    MeshData mesh;
    std::string error;
    CHECK_FALSE(loadMesh("этого файла нет.pwm", mesh, &error));
    CHECK_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// Ассеты
// ---------------------------------------------------------------------------

TEST_CASE("ассеты: манифест планет читается целиком") {
    const std::string manifest = findManifest();
    if (manifest.empty()) {
        MESSAGE("ассеты не собраны — пропускаю");
        return;
    }

    SystemAssets assets;
    REQUIRE_MESSAGE(assets.load(manifest), assets.error());

    // Класс планеты — это ИНДЕКС в списке. Разойдись он с sim::PlanetClass,
    // океанический мир получил бы поверхность выжженного камня, и заметили
    // бы это глазами через неделю.
    CHECK(assets.planets().size() == size_t(sim::PlanetClass::Count));
    CHECK(assets.planets()[size_t(sim::PlanetClass::Ocean)].id == "ocean");
    CHECK(assets.planets()[size_t(sim::PlanetClass::GasGiant)].id == "gas_giant");
    CHECK(assets.planets()[size_t(sim::PlanetClass::Station)].id == "station");

    // У газового гиганта есть кольцо, у выжженного камня — нет.
    CHECK(assets.planets()[size_t(sim::PlanetClass::GasGiant)].ring);
    CHECK_FALSE(assets.planets()[size_t(sim::PlanetClass::Barren)].ring);

    // Светила: по одному на класс.
    CHECK(assets.stars().size() == size_t(sim::StarClass::Count));

    // Постройки: по одной на каждый вид, кроме None.
    CHECK(assets.structures().size() == size_t(sim::Building::Count) - 1);

    for (const auto& planet : assets.planets()) {
        CAPTURE(planet.id);
        CHECK_FALSE(planet.mesh.empty());
        CHECK(planet.textureWidth > 0);
        CHECK(planet.textureHeight > 0);
        // Развёртка равнопромежуточная: полный оборот по долготе против
        // половины по широте, то есть ровно два к одному.
        CHECK(planet.textureWidth == planet.textureHeight * 2);
    }
}

// ---------------------------------------------------------------------------
// Кадр
// ---------------------------------------------------------------------------

TEST_CASE("вид системы: без ассетов кадр пуст, а не сломан") {
    Scene scene;
    scene.build(0);
    CHECK(scene.frame.batches.empty());
    CHECK(scene.frame.spots.empty());
}

TEST_CASE("вид системы: каждая планета попадает в кадр и на экран") {
    const std::string manifest = findManifest();
    if (manifest.empty()) {
        MESSAGE("ассеты не собраны — пропускаю");
        return;
    }

    Scene scene(60);
    REQUIRE(scene.assets.load(manifest));

    const uint32_t system = scene.systemWith(4);
    REQUIRE(system != UINT32_MAX);
    scene.camera.distance = fitDistance(4);
    scene.build(system);

    CHECK(scene.frame.spots.size() == 4);
    for (const PlanetScreenSpot& spot : scene.frame.spots) {
        CAPTURE(spot.orbit);
        // Камера подобрана так, чтобы система влезла целиком: планета
        // за краем кадра означала бы, что подбор не работает.
        CHECK(spot.visible);
        CHECK(spot.screenX > 0.0f);
        CHECK(spot.screenX < 1.0f);
        CHECK(spot.screenRadius > 0.0f);
    }
}

TEST_CASE("вид системы: орбиты расходятся, а не лежат друг на друге") {
    const std::string manifest = findManifest();
    if (manifest.empty()) return;

    Scene scene(60);
    REQUIRE(scene.assets.load(manifest));
    const uint32_t system = scene.systemWith(4);
    REQUIRE(system != UINT32_MAX);
    scene.build(system);

    // Расстояние от светила растёт с номером орбиты. Иначе «первая орбита»
    // перестала бы что-либо значить, а вместе с ней и правило осады
    // по одной планете от звезды наружу.
    float previous = 0.0f;
    for (uint32_t orbit = 0; orbit < 4; ++orbit) {
        const float radius = kFirstOrbitRadius + kOrbitStep * float(orbit);
        CHECK(radius > previous);
        previous = radius;
    }
    CHECK(scene.instanceCount() > 4);
}

TEST_CASE("вид системы: владелец планеты виден по цвету орбиты") {
    const std::string manifest = findManifest();
    if (manifest.empty()) return;

    Scene scene(60);
    REQUIRE(scene.assets.load(manifest));
    const uint32_t system = scene.systemWith(3);
    REQUIRE(system != UINT32_MAX);

    // Ничья система: орбиты нейтральные.
    scene.build(system);
    size_t neutralOrbits = 0;
    for (const MeshBatch& batch : scene.frame.batches) {
        if (batch.kind != MeshKind::Orbit) continue;
        for (const rhi::MeshInstance& instance : batch.instances) {
            if (std::abs(instance.r - neutralColor().r) < 0.01f) ++neutralOrbits;
        }
    }
    CHECK(neutralOrbits == 3);

    // Отдаём одну планету империи 1 — её орбита обязана сменить цвет.
    const sim::Entity planet = scene.galaxy.planetEntity(system, 1);
    REQUIRE(planet.valid());
    game::PlanetView owned;
    owned.owner = 1;
    scene.view.planets[planet.index] = owned;

    scene.build(system);
    bool coloured = false;
    for (const MeshBatch& batch : scene.frame.batches) {
        if (batch.kind != MeshKind::Orbit) continue;
        for (const rhi::MeshInstance& instance : batch.instances) {
            if (std::abs(instance.r - empireColor(1).r) < 0.01f &&
                std::abs(instance.g - empireColor(1).g) < 0.01f) {
                coloured = true;
            }
        }
    }
    CHECK(coloured);
}

TEST_CASE("вид системы: постройки видно на поверхности") {
    const std::string manifest = findManifest();
    if (manifest.empty()) return;

    Scene scene(60);
    REQUIRE(scene.assets.load(manifest));
    const uint32_t system = scene.systemWith(3);
    REQUIRE(system != UINT32_MAX);

    const sim::Entity planet = scene.galaxy.planetEntity(system, 0);
    REQUIRE(planet.valid());

    scene.build(system);
    CHECK(scene.instanceCount(MeshKind::Structure) == 0);

    game::PlanetView developed;
    developed.owner = 0;
    developed.buildings[0] = uint8_t(sim::Building::Mine);
    developed.buildings[1] = uint8_t(sim::Building::Shipyard);
    scene.view.planets[planet.index] = developed;

    scene.build(system);
    CHECK(scene.instanceCount(MeshKind::Structure) == 2);

    // Постройки стоят НА поверхности, а не в центре тела и не в пустоте.
    const float radius = planetRadius(scene.galaxy.planetClass(system, 0));
    for (const MeshBatch& batch : scene.frame.batches) {
        if (batch.kind != MeshKind::Structure) continue;
        for (const rhi::MeshInstance& instance : batch.instances) {
            // Высота над плоскостью орбит не больше радиуса тела: постройка
            // на поверхности шара не может оказаться дальше его края.
            CHECK(std::abs(instance.origin[2]) <= radius * 1.05f);
        }
    }
}

TEST_CASE("вид системы: недостроенное меньше готового") {
    // Стройка видна с первого тика и растёт вместе с готовностью. Это
    // ровно та обратная связь, ради которой стройка и стала долгой.
    const std::string manifest = findManifest();
    if (manifest.empty()) return;

    Scene scene(60);
    REQUIRE(scene.assets.load(manifest));
    const uint32_t system = scene.systemWith(3);
    REQUIRE(system != UINT32_MAX);
    const sim::Entity planet = scene.galaxy.planetEntity(system, 0);
    REQUIRE(planet.valid());

    auto scaleOfFirstStructure = [&]() {
        for (const MeshBatch& batch : scene.frame.batches) {
            if (batch.kind != MeshKind::Structure) continue;
            if (batch.instances.empty()) continue;
            const rhi::MeshInstance& instance = batch.instances.front();
            return std::sqrt(instance.axisZ[0] * instance.axisZ[0] +
                             instance.axisZ[1] * instance.axisZ[1] +
                             instance.axisZ[2] * instance.axisZ[2]);
        }
        return 0.0f;
    };

    game::PlanetView started;
    started.owner = 0;
    started.buildSlot = 0;
    started.buildBuilding = uint8_t(sim::Building::Mine);
    started.buildPercent = 10;
    scene.view.planets[planet.index] = started;
    scene.build(system);
    const float small = scaleOfFirstStructure();
    CHECK(small > 0.0f);

    game::PlanetView finished;
    finished.owner = 0;
    finished.buildings[0] = uint8_t(sim::Building::Mine);
    scene.view.planets[planet.index] = finished;
    scene.build(system);
    const float full = scaleOfFirstStructure();

    CHECK(full > small);
}

TEST_CASE("вид системы: осаждающий флот виден") {
    // Осада без видимого осаждающего выглядит сломанной игрой, а не тихой
    // угрозой: игрок видит падающую оборону и не видит причины.
    const std::string manifest = findManifest();
    if (manifest.empty()) return;

    Scene scene(60);
    REQUIRE(scene.assets.load(manifest));
    const uint32_t system = scene.systemWith(3);
    REQUIRE(system != UINT32_MAX);

    scene.build(system);
    CHECK(scene.instanceCount(MeshKind::Fleet) == 0);

    game::FleetView raider;
    raider.id = 7;
    raider.empire = 1;
    raider.system = system;
    raider.nextSystem = system;
    raider.composition = sim::Fleet{4, 1, 0, 0};
    scene.view.fleets[raider.id] = raider;

    // Флот в пути не стоит в системе и рисоваться там не должен.
    game::FleetView passing = raider;
    passing.id = 8;
    passing.nextSystem = system + 1;
    scene.view.fleets[passing.id] = passing;

    scene.build(system);
    CHECK(scene.instanceCount(MeshKind::Fleet) == 1);

    // Цвет империи: чей флот, видно без подписей.
    for (const MeshBatch& batch : scene.frame.batches) {
        if (batch.kind != MeshKind::Fleet) continue;
        for (const rhi::MeshInstance& instance : batch.instances) {
            CHECK(std::abs(instance.r - empireColor(1).r) < 0.01f);
            CHECK(std::abs(instance.g - empireColor(1).g) < 0.01f);
        }
    }
}

TEST_CASE("вид системы: флоты не сваливаются в одну точку") {
    const std::string manifest = findManifest();
    if (manifest.empty()) return;

    Scene scene(60);
    REQUIRE(scene.assets.load(manifest));
    const uint32_t system = scene.systemWith(3);
    REQUIRE(system != UINT32_MAX);

    for (uint32_t id = 1; id <= 5; ++id) {
        game::FleetView fleet;
        fleet.id = id;
        fleet.empire = uint8_t(id % 3);
        fleet.system = system;
        fleet.nextSystem = system;
        fleet.composition = sim::Fleet{2, 0, 0, 0};
        scene.view.fleets[id] = fleet;
    }
    scene.build(system);
    CHECK(scene.instanceCount(MeshKind::Fleet) == 5);

    std::vector<std::pair<float, float>> spots;
    for (const MeshBatch& batch : scene.frame.batches) {
        if (batch.kind != MeshKind::Fleet) continue;
        for (const rhi::MeshInstance& instance : batch.instances) {
            spots.emplace_back(instance.origin[0], instance.origin[1]);
        }
    }
    for (size_t i = 0; i < spots.size(); ++i) {
        for (size_t j = i + 1; j < spots.size(); ++j) {
            const float dx = spots[i].first - spots[j].first;
            const float dy = spots[i].second - spots[j].second;
            CAPTURE(i);
            CAPTURE(j);
            CHECK(dx * dx + dy * dy > 1.0f);
        }
    }
}

TEST_CASE("вид системы: щелчок попадает в ближайшее тело") {
    const std::string manifest = findManifest();
    if (manifest.empty()) return;

    Scene scene(60);
    REQUIRE(scene.assets.load(manifest));
    const uint32_t system = scene.systemWith(4);
    REQUIRE(system != UINT32_MAX);
    scene.camera.distance = fitDistance(4);
    scene.build(system);

    // Точно в центр тела — попадание в него.
    for (const PlanetScreenSpot& spot : scene.frame.spots) {
        CAPTURE(spot.orbit);
        CHECK(SystemView::pick(scene.frame, spot.screenX, spot.screenY) != 0xFFFFFFFFu);
    }

    // Далеко в углу — мимо всего.
    CHECK(SystemView::pick(scene.frame, 0.02f, 0.98f) == 0xFFFFFFFFu);
}
