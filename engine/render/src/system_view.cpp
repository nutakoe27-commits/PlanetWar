#include "pw/render/system_view.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "pw/core/png.h"
#include "pw/render/map_view.h"
#include "pw/sim/fleet.h"
#include "json.h"

namespace pw::render {

namespace {

constexpr float kTau = 6.28318530718f;

/// Радиусы тел по классам. Порядок — как в sim::PlanetClass.
constexpr float kPlanetRadius[] = {
    1.85f,  // Barren
    2.15f,  // Desert
    2.30f,  // Ocean
    2.00f,  // Volcanic
    3.90f,  // GasGiant — обязан быть заметно крупнее камня
    1.15f,  // AsteroidBelt
    1.30f,  // Station
};

/// Радиус светила по классу. Порядок — как в sim::StarClass.
constexpr float kStarRadius[] = {
    3.4f,  // Red
    4.2f,  // Yellow
    5.4f,  // Blue
    2.2f,  // Neutron
    3.0f,  // BlackHole
};

/// Цвет светила: он же цвет света, падающего на планеты.
struct StarLight {
    float r, g, b;
};

constexpr StarLight kStarLight[] = {
    {1.00f, 0.62f, 0.44f},  // Red
    {1.00f, 0.94f, 0.84f},  // Yellow
    {0.78f, 0.86f, 1.00f},  // Blue
    {0.90f, 0.96f, 1.00f},  // Neutron
    // Чёрная дыра почти не светит: система вокруг неё освещена аккреционным
    // диском, и это тусклый, злой свет. Именно поэтому она и выглядит так,
    // как выглядит, — самая ценная и самая неуютная недвижимость карты.
    {0.62f, 0.40f, 0.70f},  // BlackHole
};

/// Цвет атмосферы по классу планеты.
constexpr StarLight kAtmosphere[] = {
    {0.55f, 0.52f, 0.48f},  // Barren — почти нет, тонкая пыль
    {0.86f, 0.72f, 0.48f},  // Desert
    {0.48f, 0.70f, 1.00f},  // Ocean
    {0.90f, 0.42f, 0.22f},  // Volcanic
    {0.82f, 0.74f, 0.58f},  // GasGiant
    {0.50f, 0.50f, 0.50f},  // AsteroidBelt — нет
    {0.40f, 0.80f, 0.95f},  // Station — нет, но есть ходовые огни
};

/// Значение из таблицы по классу, с обрезкой.
///
/// Класс приходит из мира и может оказаться любым: галактика строится
/// клиентом по сиду, а сид приходит по сети. Выход за таблицу — это
/// чтение чужой памяти по данным из пакета.
template <typename T, size_t N>
const T& byClass(const T (&table)[N], size_t index) {
    return table[index < N ? index : 0];
}

/// Положение планеты на орбите в момент `tick`.
///
/// Угол выводится из тика мира, а не из локальных часов клиента: тогда все
/// игроки видят систему в одинаковом положении, и «планета за звездой»
/// означает одно и то же для всех. Ничего не стоит и не требует сети.
void orbitPosition(uint32_t orbit, uint32_t systemSeed, uint64_t tick, float& outX,
                   float& outY) {
    const float radius = kFirstOrbitRadius + kOrbitStep * float(orbit);

    // Дальние орбиты медленнее. Не по третьему закону Кеплера — по нему
    // внешние планеты застывали бы намертво, а движение здесь нужно как
    // признак живого мира, а не как астрономия.
    const int64_t period = kOrbitPeriodTicks * int64_t(orbit + 1);
    const float phase = float((systemSeed >> (orbit * 3 % 24)) & 0xFFu) / 255.0f;
    const float turns = phase + float(double(int64_t(tick) % period) / double(period));

    outX = radius * std::cos(turns * kTau);
    outY = radius * std::sin(turns * kTau);
}

/// Точка на сфере для слота застройки.
///
/// Спираль Фибоначчи: точки ложатся равномерно при ЛЮБОМ их числе.
/// Раскладка по широте и долготе сбивалась бы в комки у полюсов, а
/// планета с тремя слотами получала бы все три в одной точке.
void surfacePoint(uint32_t slot, uint32_t total, float& nx, float& ny, float& nz) {
    if (total == 0) total = 1;
    const float z = 1.0f - 2.0f * (float(slot) + 0.5f) / float(total);
    const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
    // Золотой угол: 2pi * (1 - 1/phi).
    const float angle = 2.39996323f * float(slot);
    nx = radius * std::cos(angle);
    ny = radius * std::sin(angle);
    nz = z;
}

/// Две оси, перпендикулярные местной вертикали.
void surfaceBasis(float nx, float ny, float nz, float outX[3], float outY[3]) {
    // Опорный вектор берётся подальше от нормали: у полюса любой другой
    // выбор даёт вырожденное произведение и постройку, растянутую в ноль.
    const bool polar = std::abs(nz) > 0.9f;
    const float refX = polar ? 1.0f : 0.0f;
    const float refY = 0.0f;
    const float refZ = polar ? 0.0f : 1.0f;

    outX[0] = refY * nz - refZ * ny;
    outX[1] = refZ * nx - refX * nz;
    outX[2] = refX * ny - refY * nx;
    const float length =
        std::sqrt(outX[0] * outX[0] + outX[1] * outX[1] + outX[2] * outX[2]);
    const float scale = length > 1e-5f ? 1.0f / length : 1.0f;
    outX[0] *= scale;
    outX[1] *= scale;
    outX[2] *= scale;

    outY[0] = ny * outX[2] - nz * outX[1];
    outY[1] = nz * outX[0] - nx * outX[2];
    outY[2] = nx * outX[1] - ny * outX[0];
}

rhi::MeshInstance makeInstance(float x, float y, float z, float scale) {
    rhi::MeshInstance instance;
    instance.axisX[0] = scale;
    instance.axisY[1] = scale;
    instance.axisZ[2] = scale;
    instance.origin[0] = x;
    instance.origin[1] = y;
    instance.origin[2] = z;
    return instance;
}

MeshBatch& batchFor(SystemFrame& frame, rhi::MeshHandle mesh, rhi::TextureHandle texture,
                    MeshKind kind, bool glow = false) {
    // Ищем с конца: пакеты идут в порядке отрисовки, и склеивать можно
    // только с последним — иначе непрозрачное уедет поверх прозрачного.
    if (!frame.batches.empty()) {
        MeshBatch& last = frame.batches.back();
        if (last.mesh == mesh && last.texture == texture && last.kind == kind &&
            last.glow == glow) {
            return last;
        }
    }
    frame.batches.push_back(MeshBatch{mesh, texture, kind, glow, {}});
    return frame.batches.back();
}

}  // namespace

float planetRadius(uint8_t planetClass) { return byClass(kPlanetRadius, planetClass); }

float fitDistance(uint32_t planetCount) {
    const uint32_t outer = planetCount > 0 ? planetCount - 1 : 0;
    const float radius = kFirstOrbitRadius + kOrbitStep * float(outer);
    // Запас берётся не на глаз: за внешней орбитой стоят флоты, и кадр,
    // подогнанный ровно по орбитам, срезал бы их по нижнему краю —
    // ровно там, где перспектива уводит ближнюю дугу за пределы экрана.
    return radius * 2.7f + 16.0f;
}

bool planetHasAtmosphere(uint8_t planetClass) {
    // Пояс астероидов и станция — не планеты, и голубой ободок вокруг них
    // был бы прямой ложью о мире.
    return planetClass != uint8_t(sim::PlanetClass::AsteroidBelt) &&
           planetClass != uint8_t(sim::PlanetClass::Station);
}

// ---------------------------------------------------------------------------
// Ассеты
// ---------------------------------------------------------------------------

namespace {

bool loadTexture(const std::string& path, std::vector<Rgba8>& pixels, int& width,
                 int& height) {
    return readPng(path, pixels, width, height);
}

}  // namespace

bool SystemAssets::load(const std::string& manifestPath) {
    planets_.clear();
    stars_.clear();
    error_.clear();

    std::ifstream file(manifestPath);
    if (!file) {
        error_ = "не удалось открыть " + manifestPath;
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const Json json(buffer.str());
    const std::string directory = directoryOf(manifestPath);

    size_t cursor = json.find("planets");
    if (cursor == std::string::npos) {
        error_ = "в манифесте нет списка планет: " + manifestPath;
        return false;
    }

    // Класс планеты — это ИНДЕКС в списке, и он обязан совпадать с
    // sim::PlanetClass. Проверяем явно: разойдись порядок, океанический
    // мир получил бы поверхность выжженного камня, и заметили бы это
    // глазами через неделю.
    //
    // Читаем РОВНО столько записей, сколько классов в симуляции, и ни одной
    // больше. Свободный обход «пока находится поле class» уезжал дальше,
    // в список светил — у них поле называется так же, — и падал на первой
    // же звезде с сообщением про нарушенный порядок планет.
    for (uint32_t index = 0; index < uint32_t(sim::PlanetClass::Count); ++index) {
        const size_t entry = json.find("class", cursor);
        if (entry == std::string::npos) {
            error_ = "в манифесте не все классы планет";
            return false;
        }

        long declared = -1;
        if (!json.number("class", declared, cursor) || declared != long(index)) {
            error_ = "порядок классов планет в манифесте нарушен";
            return false;
        }

        PlanetEntry planet;
        std::string meshName, textureName;
        if (!json.string("id", planet.id, entry) ||
            !json.string("name", planet.name, entry) ||
            !json.string("mesh", meshName, entry) ||
            !json.string("texture", textureName, entry)) {
            error_ = "неполная запись планеты в манифесте";
            return false;
        }

        std::string meshError;
        if (!loadMesh(directory + meshName, planet.mesh, &meshError)) {
            error_ = meshError;
            return false;
        }
        if (!loadTexture(directory + textureName, planet.texture, planet.textureWidth,
                         planet.textureHeight)) {
            error_ = "не удалось прочитать " + directory + textureName;
            return false;
        }

        const size_t ringAt = json.find("ring", entry);
        planet.ring = ringAt != std::string::npos &&
                      json.text().compare(ringAt + 1, 4, "true") == 0;

        planets_.push_back(std::move(planet));
        cursor = entry + 1;
    }

    cursor = json.find("stars");
    if (cursor != std::string::npos) {
        for (;;) {
            const size_t entry = json.find("class", cursor);
            if (entry == std::string::npos) break;

            StarEntry star;
            std::string textureName;
            if (!json.string("id", star.id, entry) ||
                !json.string("texture", textureName, entry)) {
                break;
            }
            if (!loadTexture(directory + textureName, star.texture, star.textureWidth,
                             star.textureHeight)) {
                error_ = "не удалось прочитать " + directory + textureName;
                return false;
            }
            stars_.push_back(std::move(star));
            cursor = entry + 1;
        }
    }

    // Постройки. Индекс в списке — это sim::Building минус единица.
    {
        size_t at = json.find("structures");
        while (at != std::string::npos) {
            const size_t entry = json.find("building", at);
            if (entry == std::string::npos) break;

            StructureEntry structure;
            std::string meshName;
            if (!json.string("id", structure.id, entry) ||
                !json.string("mesh", meshName, entry)) {
                break;
            }
            std::string meshError;
            if (!loadMesh(directory + meshName, structure.mesh, &meshError)) {
                error_ = meshError;
                return false;
            }
            structures_.push_back(std::move(structure));
            at = entry + 1;
        }

        std::string textureName;
        const size_t textureAt = json.find("structure_texture");
        if (textureAt != std::string::npos &&
            json.string("structure_texture", textureName)) {
            loadTexture(directory + textureName, structureTextureData_,
                        structureTextureWidth_, structureTextureHeight_);
        }
    }

    // Корпуса кораблей. Индекс в списке — это sim::Hull минус единица.
    {
        size_t at = json.find("hulls");
        while (at != std::string::npos) {
            const size_t entry = json.find("hull", at);
            if (entry == std::string::npos) break;

            HullEntry hull;
            std::string meshName;
            if (!json.string("id", hull.id, entry) ||
                !json.string("mesh", meshName, entry)) {
                break;
            }
            std::string meshError;
            if (!loadMesh(directory + meshName, hull.mesh, &meshError)) {
                error_ = meshError;
                return false;
            }
            hulls_.push_back(std::move(hull));
            at = entry + 1;
        }

        std::string textureName;
        if (json.find("hull_texture") != std::string::npos &&
            json.string("hull_texture", textureName)) {
            loadTexture(directory + textureName, hullTextureData_, hullTextureWidth_,
                        hullTextureHeight_);
        }
    }

    {
        const size_t spaceBlock = json.find("space");
        std::string textureName;
        if (spaceBlock != std::string::npos &&
            json.string("texture", textureName, spaceBlock)) {
            loadTexture(directory + textureName, spaceTextureData_, spaceTextureWidth_,
                        spaceTextureHeight_);
        }
    }

    const size_t ringBlock = json.find("ring", cursor);
    if (ringBlock != std::string::npos) {
        std::string meshName, textureName;
        if (json.string("mesh", meshName, ringBlock) &&
            json.string("texture", textureName, ringBlock)) {
            std::string meshError;
            loadMesh(directory + meshName, ringMeshData_, &meshError);
            loadTexture(directory + textureName, ringTextureData_, ringTextureWidth_,
                        ringTextureHeight_);
        }
    }
    return true;
}

bool SystemAssets::upload(rhi::Device& device) {
    if (planets_.empty()) {
        error_ = "нечего заливать: манифест не прочитан";
        return false;
    }

    for (PlanetEntry& planet : planets_) {
        planet.meshHandle = device.createMesh(planet.mesh.vertices.data(),
                                              planet.mesh.vertices.size(),
                                              planet.mesh.indices.data(),
                                              planet.mesh.indices.size());
        planet.textureHandle = device.createTexture(planet.textureWidth,
                                                    planet.textureHeight,
                                                    planet.texture.data());
        if (planet.meshHandle == rhi::kInvalidMesh ||
            planet.textureHandle == rhi::kInvalidTexture) {
            error_ = "не удалось залить планету " + planet.id;
            return false;
        }
    }

    // Сфера общая: форма у классов одна, различаются они текстурой.
    // Берём первую — она гарантированно есть.
    sphereMesh_ = planets_.front().meshHandle;

    for (StarEntry& star : stars_) {
        star.textureHandle =
            device.createTexture(star.textureWidth, star.textureHeight, star.texture.data());
    }

    if (!ringMeshData_.empty()) {
        ringMesh_ = device.createMesh(ringMeshData_.vertices.data(),
                                      ringMeshData_.vertices.size(),
                                      ringMeshData_.indices.data(),
                                      ringMeshData_.indices.size());
    }
    if (!ringTextureData_.empty()) {
        ringTexture_ = device.createTexture(ringTextureWidth_, ringTextureHeight_,
                                            ringTextureData_.data());
    }

    const MeshData orbit = makeOrbitRing();
    orbitMesh_ = device.createMesh(orbit.vertices.data(), orbit.vertices.size(),
                                   orbit.indices.data(), orbit.indices.size());

    for (StructureEntry& structure : structures_) {
        structure.meshHandle = device.createMesh(structure.mesh.vertices.data(),
                                                 structure.mesh.vertices.size(),
                                                 structure.mesh.indices.data(),
                                                 structure.mesh.indices.size());
    }
    if (!structureTextureData_.empty()) {
        structureTexture_ = device.createTexture(structureTextureWidth_,
                                                 structureTextureHeight_,
                                                 structureTextureData_.data());
    }

    for (HullEntry& hull : hulls_) {
        hull.meshHandle =
            device.createMesh(hull.mesh.vertices.data(), hull.mesh.vertices.size(),
                              hull.mesh.indices.data(), hull.mesh.indices.size());
    }
    if (!hullTextureData_.empty()) {
        hullTexture_ =
            device.createTexture(hullTextureWidth_, hullTextureHeight_, hullTextureData_.data());
    }

    if (!spaceTextureData_.empty()) {
        spaceTexture_ = device.createTexture(spaceTextureWidth_, spaceTextureHeight_,
                                             spaceTextureData_.data());
    }

    const Rgba8 white{255, 255, 255, 255};
    blankTexture_ = device.createTexture(1, 1, &white);

    return orbitMesh_ != rhi::kInvalidMesh && blankTexture_ != rhi::kInvalidTexture;
}

// ---------------------------------------------------------------------------
// Сборка кадра
// ---------------------------------------------------------------------------

void SystemView::build(const sim::Galaxy& galaxy, const game::WorldView& world,
                       uint32_t system, uint32_t empire, SystemCamera& camera,
                       float aspect, SystemFrame& out) const {
    out.clear();
    if (assets_ == nullptr || !assets_->valid()) return;
    if (system >= galaxy.systemCount()) return;

    const uint8_t starClass = galaxy.starClass(system);
    const uint32_t planetCount = galaxy.planetCount(system);
    const StarLight& light = byClass(kStarLight, starClass);

    // --- камера ---
    //
    // Сферическая вокруг светила. Если игрок выбрал тело, смотрим на него:
    // «выбрать планету» обязано означать «увидеть планету», иначе выбор
    // на дальней орбите теряется в кадре.
    float focusX = 0.0f, focusY = 0.0f;
    if (camera.focusOrbit < planetCount) {
        orbitPosition(camera.focusOrbit, uint32_t(galaxy.seed()) ^ system, world.tick,
                      focusX, focusY);
    }

    // Взгляд ДОГОНЯЕТ цель, а не переставляется на неё. Экспонента —
    // та же, что в интерфейсе: не зависит от частоты кадров и не
    // перелетает цель. Первый кадр встаёт сразу, иначе каждый вход
    // в систему начинался бы перелётом из её центра.
    if (!camera.lookValid || camera.followSeconds <= 0.0f ||
        camera.deltaSeconds <= 0.0f) {
        camera.lookX = focusX;
        camera.lookY = focusY;
        camera.lookValid = true;
    } else {
        const float k =
            1.0f - std::exp(-camera.deltaSeconds / (camera.followSeconds / 3.0f));
        camera.lookX += (focusX - camera.lookX) * k;
        camera.lookY += (focusY - camera.lookY) * k;
    }
    focusX = camera.lookX;
    focusY = camera.lookY;

    const float yaw = camera.yawTurns * kTau;
    const float pitch = camera.pitchTurns * kTau;
    const float horizontal = std::cos(pitch) * camera.distance;

    out.camera.targetX = focusX;
    out.camera.targetY = focusY;
    out.camera.targetZ = 0.0f;
    out.camera.eyeX = focusX + horizontal * std::cos(yaw);
    out.camera.eyeY = focusY + horizontal * std::sin(yaw);
    out.camera.eyeZ = std::sin(pitch) * camera.distance;
    out.camera.lightX = 0.0f;
    out.camera.lightY = 0.0f;
    out.camera.lightZ = 0.0f;
    out.camera.lightR = light.r;
    out.camera.lightG = light.g;
    out.camera.lightB = light.b;
    // Ночная сторона не чёрная: подсветка от рассеянного света системы.
    // Совсем чёрная сторона читается как дыра в геометрии, а не как ночь.
    // Ночная сторона не чёрная: рассеянный свет системы. Совсем чёрная
    // сторона читается как дыра в геометрии, а не как ночь, и планета
    // перестаёт быть шаром.
    out.camera.ambient = 0.14f;
    out.camera.farPlane = std::max(400.0f, camera.distance * 8.0f);

    // --- небо ---
    //
    // Огромный шар вокруг камеры, натянутый ИЗНУТРИ. Отрицательный масштаб
    // по одной оси выворачивает намотку граней наизнанку — иначе отсечение
    // задних граней съело бы небо целиком.
    //
    // Чёрный фон читается как «сцена не догрузилась». Живое небо стоит
    // одной текстуры и меняет ощущение картинки целиком.
    if (assets_->spaceTexture() != rhi::kInvalidTexture) {
        const float radius = out.camera.farPlane * 0.42f;
        rhi::MeshInstance sky =
            makeInstance(out.camera.eyeX, out.camera.eyeY, out.camera.eyeZ, radius);
        sky.axisX[0] = -radius;
        sky.emissive = 1.0f;
        sky.gloss = 0.0f;
        // Небо приглушено: оно фон, а не предмет разглядывания. Яркое небо
        // спорит с планетами за внимание и делает кадр плоским.
        sky.r = sky.g = sky.b = 0.85f;
        batchFor(out, assets_->sphereMesh(), assets_->spaceTexture(), MeshKind::Sky)
            .instances.push_back(sky);
    }

    // --- светило ---
    const rhi::TextureHandle starTexture =
        starClass < assets_->stars().size() ? assets_->stars()[starClass].textureHandle
                                            : assets_->blankTexture();
    const float starRadius = byClass(kStarRadius, starClass);
    {
        rhi::MeshInstance star = makeInstance(0.0f, 0.0f, 0.0f, starRadius);
        star.r = light.r;
        star.g = light.g;
        star.b = light.b;
        star.emissive = 1.0f;
        star.gloss = 0.0f;
        batchFor(out, assets_->sphereMesh(), starTexture, MeshKind::Star)
            .instances.push_back(star);
    }

    // --- планеты ---
    //
    // Непрозрачное рисуется до прозрачного: буфер глубины у нас пишется
    // всеми, и атмосфера, нарисованная первой, закрыла бы собой планету.
    for (uint32_t orbit = 0; orbit < planetCount; ++orbit) {
        const sim::Entity entity = galaxy.planetEntity(system, orbit);
        if (!entity.valid()) continue;

        uint8_t owner = 0xFF;
        {
            const auto found = world.planets.find(entity.index);
            if (found != world.planets.end()) owner = found->second.owner;
        }

        // Класс тела берётся из СВОЕЙ галактики: она выводится из сида
        // и по сети не ездит. Снапшот привозит только то, что меняется.
        const uint8_t planetClass = galaxy.planetClass(system, orbit);

        float x = 0.0f, y = 0.0f;
        orbitPosition(orbit, uint32_t(galaxy.seed()) ^ system, world.tick, x, y);
        const float radius = planetRadius(planetClass);

        const bool selected = camera.focusOrbit == orbit;

        // Орбита. Тонируется цветом владельца: наполовину взятая система
        // должна читаться с одного взгляда, а по-другому этого не показать.
        {
            const EmpireColor& colour =
                owner == 0xFF ? neutralColor() : empireColor(owner);
            rhi::MeshInstance track = makeInstance(
                0.0f, 0.0f, 0.0f, kFirstOrbitRadius + kOrbitStep * float(orbit));
            track.r = colour.r;
            track.g = colour.g;
            track.b = colour.b;
            track.a = selected ? 0.85f : (owner == 0xFF ? 0.22f : 0.45f);
            track.emissive = 1.0f;
            batchFor(out, assets_->orbitMesh(), assets_->blankTexture(), MeshKind::Orbit)
                .instances.push_back(track);
        }

        const SystemAssets::PlanetEntry& entry =
            assets_->planets()[planetClass < assets_->planets().size() ? planetClass : 0];

        rhi::MeshInstance body = makeInstance(x, y, 0.0f, radius);
        body.gloss = entry.id == "ocean" ? 0.8f : 0.2f;
        // Своё ярче, чужое приглушено. Это не украшение: на карте из
        // десяти тел взгляд обязан находить свои без чтения подписей.
        if (owner != 0xFF && owner == uint8_t(empire & 0xFFu)) {
            body.r = body.g = body.b = 1.0f;
        } else if (owner != 0xFF) {
            body.r = body.g = body.b = 0.82f;
        } else {
            body.r = body.g = body.b = 0.9f;
        }
        if (selected) body.rim = 0.55f;
        batchFor(out, entry.meshHandle, entry.textureHandle, MeshKind::Planet)
            .instances.push_back(body);

        // Кольцо газового гиганта.
        if (entry.ring && assets_->ringMesh() != rhi::kInvalidMesh) {
            rhi::MeshInstance ring = makeInstance(x, y, 0.0f, radius * 0.92f);
            // Небольшой наклон: кольцо строго в плоскости орбиты выглядит
            // приклеенным к схеме, а не принадлежащим планете.
            const float tilt = 0.06f * kTau;
            ring.axisY[1] = radius * 0.92f * std::cos(tilt);
            ring.axisY[2] = radius * 0.92f * std::sin(tilt);
            ring.axisZ[1] = -radius * 0.92f * std::sin(tilt);
            ring.axisZ[2] = radius * 0.92f * std::cos(tilt);
            // Кольцо рисуется СКЛАДЫВАЮЩИМСЯ светом: это пыль, которая
            // рассеивает, а не заслоняет. Заодно тёмные полосы текстуры
            // становятся прозрачными сами собой — щели в кольце получаются
            // без отдельной карты прозрачности.
            ring.a = 0.85f;
            ring.emissive = 1.0f;
            ring.gloss = 0.0f;
            batchFor(out, assets_->ringMesh(),
                     assets_->ringTexture() != rhi::kInvalidTexture
                         ? assets_->ringTexture()
                         : assets_->blankTexture(),
                     MeshKind::Ring, /*glow=*/true)
                .instances.push_back(ring);
        }

        // --- постройки на поверхности ---
        //
        // Игрок обязан ВИДЕТЬ, что он захватывает: обжитой мир с верфью
        // и крепостью или голый камень. Список в панели этого не даёт —
        // цифры читаются, а не узнаются, и на десятке тел взгляд по ним
        // не пробежит.
        if (!assets_->structures().empty()) {
            const auto found = world.planets.find(entity.index);
            if (found != world.planets.end()) {
                const game::PlanetView& live = found->second;
                const uint8_t slots = galaxy.planetSlots(system, orbit);
                const uint8_t limit = std::min<uint8_t>(slots, sim::kMaxSlots);

                for (uint8_t slot = 0; slot < limit; ++slot) {
                    uint8_t building = live.buildings[slot];
                    float growth = 1.0f;
                    if (building == uint8_t(sim::Building::None)) {
                        if (live.buildSlot != slot) continue;
                        // Стройка видна с первого тика и растёт вместе
                        // с готовностью. Пустой слот, в котором «что-то
                        // происходит», — это ровно та обратная связь,
                        // ради которой стройка и стала долгой.
                        building = live.buildBuilding;
                        growth = 0.28f + 0.72f * float(live.buildPercent) / 100.0f;
                    }
                    if (building == uint8_t(sim::Building::None)) continue;

                    const uint32_t index = uint32_t(building) - 1u;
                    if (index >= assets_->structures().size()) continue;
                    const rhi::MeshHandle mesh = assets_->structures()[index].meshHandle;

                    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                    surfacePoint(slot, limit, nx, ny, nz);

                    // Базис на поверхности: местная вертикаль плюс две оси
                    // вдоль неё. Без него постройки лежали бы плашмя
                    // и торчали из шара под случайными углами.
                    float ax[3], ay[3];
                    surfaceBasis(nx, ny, nz, ax, ay);

                    // Постройка заметно крупнее натуральной: настоящий
                    // город на планете размером с экран занимал бы доли
                    // пикселя. Здесь важна не достоверность масштаба,
                    // а читаемость — по силуэту игрок узнаёт, что стоит
                    // на планете, не открывая списка.
                    const float scale = radius * 0.30f * growth;
                    rhi::MeshInstance instance;
                    for (int i = 0; i < 3; ++i) {
                        instance.axisX[i] = ax[i] * scale;
                        instance.axisY[i] = ay[i] * scale;
                    }
                    instance.axisZ[0] = nx * scale;
                    instance.axisZ[1] = ny * scale;
                    instance.axisZ[2] = nz * scale;
                    // Чуть утоплено в грунт: постройка, стоящая ровно
                    // на сфере, парит над ней из-за гранёности сетки.
                    const float lift = radius * 0.97f;
                    instance.origin[0] = x + nx * lift;
                    instance.origin[1] = y + ny * lift;
                    instance.origin[2] = nz * lift;
                    instance.gloss = 0.35f;
                    if (growth < 1.0f) {
                        // Недостроенное отличается цветом каркаса: игрок
                        // должен видеть разницу между «стоит» и «строится».
                        instance.r = 0.70f;
                        instance.g = 0.78f;
                        instance.b = 0.95f;
                        instance.emissive = 0.15f;
                    }
                    batchFor(out, mesh, assets_->structureTexture(), MeshKind::Structure)
                        .instances.push_back(instance);
                }
            }
        }

        // Экранное место тела: по нему работает и выбор мышью, и подписи.
        PlanetScreenSpot spot;
        spot.orbit = orbit;
        spot.planetId = entity.index;
        spot.visible = rhi::projectPoint(out.camera, aspect, x, y, 0.0f, spot.screenX,
                                         spot.screenY, spot.depth);
        if (spot.visible && spot.depth > 0.0f) {
            // Радиус на экране: угловой размер тела, переведённый в доли
            // высоты кадра. Считается из тех же чисел, что и проекция,
            // поэтому попадание мышью совпадает с тем, что видит глаз.
            const float halfHeight =
                std::tan(out.camera.fovTurns * 3.14159265358979323846f);
            spot.screenRadius = radius / (spot.depth * halfHeight) * 0.5f;
        }
        out.spots.push_back(spot);
    }

    // --- флоты ---
    //
    // Осада без видимого осаждающего выглядит сломанной игрой, а не тихой
    // угрозой: игрок видит падающую оборону и не видит причины. Флоты
    // становятся на высокую орбиту вокруг светила — там, где им и место,
    // и там, где они не спорят с планетами за один и тот же пиксель.
    if (!assets_->hulls().empty()) {
        // Порядок обхода задаёт карман: std::map упорядочен по номеру
        // сущности, значит расстановка одинакова у всех игроков и не
        // прыгает от кадра к кадру.
        uint32_t pocket = 0;
        for (const auto& [id, fleet] : world.fleets) {
            if (fleet.system != system || fleet.nextSystem != system) continue;

            const uint32_t tonnage = sim::fleetTonnage(fleet.composition);
            if (tonnage == 0) continue;

            // Силуэт задаёт САМЫЙ СТАРШИЙ присутствующий корпус, и ищется
            // он циклом с конца.
            //
            // Раньше здесь стояла цепочка из четырёх if — линкор, крейсер,
            // эсминец, иначе корвет. Классов давно восемь, и флот из одних
            // титанов рисовался корветом: пять минут постройки венца сезона
            // выглядели как пять минут постройки самого дешёвого корабля.
            uint32_t hull = uint32_t(sim::Hull::Corvette);
            for (uint8_t candidate = uint8_t(sim::Hull::Count) - 1; candidate >= 1;
                 --candidate) {
                if (fleet.composition[sim::Hull(candidate)] > 0) {
                    hull = candidate;
                    break;
                }
            }

            const uint32_t index = hull - 1u;
            if (index >= assets_->hulls().size()) continue;

            // ФЛОТ СТОИТ У СВОЕЙ ПЛАНЕТЫ, а не на общей дуге вокруг звезды.
            //
            // «Флот в системе» ничего не говорит о том, что он там делает.
            // Флот на орбите конкретного мира читается с одного взгляда:
            // это гарнизон вот этой планеты — или, если планета чужая,
            // то, что её осаждает. Раньше все отряды висели одной гроздью
            // за внешней орбитой, и понять, кто кого сторожит, было нельзя.
            //
            // Орбиту назначает СЕРВЕР (FleetLocation::orbit) — она часть
            // состояния мира, а не выдумка клиента. Иначе двое игроков
            // видели бы один и тот же флот у разных планет.
            float centreX = 0.0f;
            float centreY = 0.0f;
            float station = 0.0f;
            if (fleet.orbit < planetCount) {
                orbitPosition(fleet.orbit, uint32_t(galaxy.seed()) ^ system, world.tick, centreX, centreY);
                // Радиус стоянки чуть больше самой планеты: корабли
                // кружат НАД ней, не вминаясь в поверхность.
                station = 2.6f + std::sqrt(float(tonnage)) * 0.12f;
            } else {
                // Орбиты нет — общая стоянка за внешней планетой, как было.
                const float outer = kFirstOrbitRadius +
                                    kOrbitStep * float(planetCount > 0 ? planetCount - 1 : 0);
                station = outer + 3.5f;
            }

            // Своя дуга внутри стоянки: два отряда у одной планеты обязаны
            // стоять в разных точках, иначе они сливаются в один силуэт.
            // Угол выводится из НОМЕРА ФЛОТА и тика, а не из порядка обхода:
            // так корабль не прыгает по кругу, когда рядом появляется
            // или гибнет сосед.
            const float turn =
                float(double(int64_t(world.tick) % kOrbitPeriodTicks) /
                      double(kOrbitPeriodTicks));
            const float angle =
                kTau * (turn + float(id % 89u) / 89.0f) + float(pocket) * 0.7f;
            const float x = centreX + station * std::cos(angle);
            const float y = centreY + station * std::sin(angle);
            const float z = 2.4f + float(pocket % 3u) * 1.6f;
            ++pocket;

            // Размер от тоннажа, но с сильным затуханием: иначе флот
            // в триста тонн накрыл бы собой всю систему.
            //
            // Масштаб заведомо не натуральный: корабль рядом с планетой
            // в натуральную величину — это доли пикселя. Читается силуэт,
            // а не размер, и корвет обязан отличаться от линкора.
            const float size = 2.2f + std::sqrt(float(tonnage)) * 0.5f;

            // Нос смотрит вдоль движения по орбите, то есть по касательной.
            // Корабль, висящий боком, читается как обломок.
            const float nose = angle + kTau * 0.25f;
            rhi::MeshInstance ship;
            ship.axisX[0] = std::cos(nose) * size;
            ship.axisX[1] = std::sin(nose) * size;
            ship.axisY[0] = -std::sin(nose) * size;
            ship.axisY[1] = std::cos(nose) * size;
            ship.axisZ[2] = size;
            ship.origin[0] = x;
            ship.origin[1] = y;
            ship.origin[2] = z;

            const EmpireColor& colour =
                fleet.empire == 0xFF ? neutralColor() : empireColor(fleet.empire);
            ship.r = colour.r;
            ship.g = colour.g;
            ship.b = colour.b;
            ship.gloss = 0.5f;
            // Свой флот заметно ярче чужого: в системе, где идёт бой,
            // взгляд обязан находить своих без чтения подписей.
            ship.emissive = fleet.empire == uint8_t(empire & 0xFFu) ? 0.30f : 0.10f;

            batchFor(out, assets_->hulls()[index].meshHandle,
                     assets_->hullTexture() != rhi::kInvalidTexture
                         ? assets_->hullTexture()
                         : assets_->blankTexture(),
                     MeshKind::Fleet)
                .instances.push_back(ship);
        }
    }

    // --- атмосферы ---
    //
    // Отдельным проходом, ПОСЛЕ всех непрозрачных тел: они прозрачны,
    // и нарисованные вперемешку с планетами закрывали бы то, что за ними.
    for (uint32_t orbit = 0; orbit < planetCount; ++orbit) {
        const uint8_t planetClass = galaxy.planetClass(system, orbit);
        if (!planetHasAtmosphere(planetClass)) continue;

        float x = 0.0f, y = 0.0f;
        orbitPosition(orbit, uint32_t(galaxy.seed()) ^ system, world.tick, x, y);

        const StarLight& colour = byClass(kAtmosphere, planetClass);
        rhi::MeshInstance shell =
            makeInstance(x, y, 0.0f, planetRadius(planetClass) * 1.055f);
        shell.r = colour.r;
        shell.g = colour.g;
        shell.b = colour.b;
        shell.a = 0.30f;
        // Вся суть атмосферы — в контуре: у тела с воздухом край всегда
        // светлее середины. Один этот эффект отличает шар от круга.
        shell.rim = 1.6f;
        shell.gloss = 0.0f;
        batchFor(out, assets_->sphereMesh(), assets_->blankTexture(), MeshKind::Atmosphere,
                 /*glow=*/true)
            .instances.push_back(shell);
    }

    // --- корона светила ---
    //
    // Три вложенные оболочки с падающей плотностью. Одна оболочка даёт
    // резкий ободок — это читается как ошибка, а не как свечение; свет
    // вокруг звезды спадает плавно, и «плавно» здесь стоит трёх слоёв.
    //
    // Рисуется ПОСЛЕДНЕЙ: она прозрачна и должна лечь поверх всего,
    // включая планеты, оказавшиеся перед звездой.
    {
        const float layers[][3] = {{1.20f, 0.30f, 1.4f},
                                   {1.60f, 0.16f, 1.1f},
                                   {2.30f, 0.08f, 0.8f}};
        for (const auto& layer : layers) {
            rhi::MeshInstance halo = makeInstance(0.0f, 0.0f, 0.0f, starRadius * layer[0]);
            // Корона НАСЫЩЕННЕЕ самой звезды. Складывающийся свет быстро
            // уходит в белое, и корона, взятая цветом светила один в один,
            // получается серой — жёлтая звезда светит белым ореолом.
            halo.r = std::min(1.0f, light.r * 1.15f);
            halo.g = light.g * 0.86f;
            halo.b = light.b * 0.70f;
            halo.a = layer[1];
            halo.emissive = 1.0f;
            halo.rim = 0.0f;
            halo.gloss = 0.0f;
            // Спад к краю оболочки: без него получаются три чётких кольца
            // вместо свечения.
            halo.halo = layer[2];
            batchFor(out, assets_->sphereMesh(), assets_->blankTexture(), MeshKind::Corona,
                     /*glow=*/true)
                .instances.push_back(halo);
        }
    }
}

uint32_t SystemView::pick(const SystemFrame& frame, float screenX, float screenY) {
    uint32_t best = 0xFFFFFFFFu;
    float bestDepth = 0.0f;

    for (const PlanetScreenSpot& spot : frame.spots) {
        if (!spot.visible) continue;

        // Радиус попадания не меньше двух процентов высоты экрана: тело
        // на дальней орбите занимает считанные пиксели, и требовать
        // попасть в него точно значило бы сделать вид неуправляемым.
        const float radius = std::max(spot.screenRadius, 0.02f);
        const float dx = screenX - spot.screenX;
        const float dy = screenY - spot.screenY;
        if (dx * dx + dy * dy > radius * radius) continue;

        // Ближайшее к камере выигрывает: перекрывающиеся тела выбираются
        // так же, как их видит глаз.
        if (best != 0xFFFFFFFFu && spot.depth >= bestDepth) continue;
        best = spot.orbit;
        bestDepth = spot.depth;
    }
    return best;
}

}  // namespace pw::render
