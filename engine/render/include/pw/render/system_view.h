#pragma once

// Вид звёздной системы: планеты в объёме.
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ ВИД. Захватывают теперь планеты, а не системы. Значит
// у игрока появилось место, где он принимает решения о КОНКРЕТНОМ теле:
// что на нём строить, стоит ли его удерживать, чем он рискует, оставив
// дальнюю орбиту соседу. На карте галактики такого места нет — там система
// одна точка, и большего она показать не может.
//
// Отсюда объём. Не ради красоты как таковой: орбита, размер тела, кольцо,
// освещённая и ночная стороны — это всё информация, которую плоский кружок
// передать не может, а глаз считывает мгновенно.
//
// Модуль ничего не знает ни про Vulkan, ни про окно: он отдаёт списки
// экземпляров, а кто и как их рисует — дело вызывающего. Из этого следует
// главное: вид системы собирается и проверяется БЕЗ ВИДЕОКАРТЫ.

#include <cstdint>
#include <string>
#include <vector>

#include "pw/game/snapshot.h"
#include "pw/render/mesh.h"
#include "pw/rhi/rhi.h"
#include "pw/sim/galaxy.h"

namespace pw::render {

/// Радиус первой орбиты и шаг между ними, в единицах вида системы.
inline constexpr float kFirstOrbitRadius = 11.0f;
inline constexpr float kOrbitStep = 8.5f;

/// Сколько тиков занимает полный оборот по первой орбите.
///
/// Планета обязана заметно двигаться, но не мельтешить: за минуту наблюдения
/// сдвиг должен быть виден, за секунду — нет. Дальние орбиты медленнее,
/// как и положено.
inline constexpr int64_t kOrbitPeriodTicks = 20 * 60 * 12;

/// Радиус тела по классу планеты. Порядок — как в sim::PlanetClass.
///
/// Числа заданы глазом, а не выведены: это масштаб картинки, а не баланс.
/// Газовый гигант обязан быть заметно крупнее камня — иначе класс планеты
/// перестаёт читаться с одного взгляда, и игрок вынужден наводить курсор
/// на каждое тело.
float planetRadius(uint8_t planetClass);

/// Удаление камеры, при котором система целиком помещается в кадр.
///
/// Считается, а не задаётся: система с одной планетой и система с шестью
/// требуют разного отступа, и одно число на обе означало бы либо пустой
/// кадр, либо орбиты за краем экрана.
float fitDistance(uint32_t planetCount);

/// Какие тела вообще имеют атмосферу. У пояса астероидов и станции её нет,
/// и рисовать им голубой ободок значило бы врать о мире.
bool planetHasAtmosphere(uint8_t planetClass);

// ---------------------------------------------------------------------------
// Ассеты
// ---------------------------------------------------------------------------

/// Сетки и текстуры вида системы, прочитанные из assets/build/planets.json.
///
/// Загрузка разделена надвое: сначала чтение с диска (можно в тесте, без
/// видеокарты), потом заливка на устройство. Иначе проверить, что манифест
/// цел и все файлы на месте, было бы нельзя без окна и драйвера.
class SystemAssets {
public:
    struct PlanetEntry {
        std::string id;
        std::string name;
        MeshData mesh;
        std::vector<Rgba8> texture;
        int textureWidth = 0;
        int textureHeight = 0;
        bool ring = false;

        rhi::MeshHandle meshHandle = rhi::kInvalidMesh;
        rhi::TextureHandle textureHandle = rhi::kInvalidTexture;
    };

    /// Постройка на поверхности. Индекс в списке равен sim::Building
    /// минус единица: None постройкой не является.
    struct StructureEntry {
        std::string id;
        MeshData mesh;
        rhi::MeshHandle meshHandle = rhi::kInvalidMesh;
    };

    struct StarEntry {
        std::string id;
        std::vector<Rgba8> texture;
        int textureWidth = 0;
        int textureHeight = 0;
        rhi::TextureHandle textureHandle = rhi::kInvalidTexture;
    };

    /// Прочитать манифест и всё, на что он ссылается.
    bool load(const std::string& manifestPath);

    /// Залить прочитанное на устройство. Вызывается один раз за запуск.
    bool upload(rhi::Device& device);

    bool valid() const { return !planets_.empty(); }
    const std::string& error() const { return error_; }

    const std::vector<PlanetEntry>& planets() const { return planets_; }
    const std::vector<StarEntry>& stars() const { return stars_; }
    const std::vector<StructureEntry>& structures() const { return structures_; }
    rhi::TextureHandle structureTexture() const { return structureTexture_; }

    /// Сетка сферы. Общая на все классы планет — форма у них одна,
    /// различаются они текстурой.
    rhi::MeshHandle sphereMesh() const { return sphereMesh_; }
    rhi::MeshHandle ringMesh() const { return ringMesh_; }
    rhi::MeshHandle orbitMesh() const { return orbitMesh_; }
    rhi::TextureHandle ringTexture() const { return ringTexture_; }
    /// Однопиксельная белая текстура: сеточный конвейер всегда читает
    /// текстуру, и телам без своей нужна заглушка.
    rhi::TextureHandle blankTexture() const { return blankTexture_; }
    /// Звёздное небо. Натягивается ИЗНУТРИ на огромный шар вокруг камеры,
    /// поэтому честно поворачивается вместе с ней.
    rhi::TextureHandle spaceTexture() const { return spaceTexture_; }

private:
    std::vector<PlanetEntry> planets_;
    std::vector<StarEntry> stars_;
    std::vector<StructureEntry> structures_;
    std::vector<Rgba8> structureTextureData_;
    int structureTextureWidth_ = 0, structureTextureHeight_ = 0;
    MeshData ringMeshData_;
    std::vector<Rgba8> ringTextureData_;
    int ringTextureWidth_ = 0, ringTextureHeight_ = 0;
    std::vector<Rgba8> spaceTextureData_;
    int spaceTextureWidth_ = 0, spaceTextureHeight_ = 0;

    rhi::MeshHandle sphereMesh_ = rhi::kInvalidMesh;
    rhi::MeshHandle ringMesh_ = rhi::kInvalidMesh;
    rhi::MeshHandle orbitMesh_ = rhi::kInvalidMesh;
    rhi::TextureHandle ringTexture_ = rhi::kInvalidTexture;
    rhi::TextureHandle blankTexture_ = rhi::kInvalidTexture;
    rhi::TextureHandle spaceTexture_ = rhi::kInvalidTexture;
    rhi::TextureHandle structureTexture_ = rhi::kInvalidTexture;
    std::string error_;
};

// ---------------------------------------------------------------------------
// Кадр
// ---------------------------------------------------------------------------

/// Куда смотрит игрок внутри системы.
struct SystemCamera {
    /// Поворот вокруг светила и подъём над плоскостью орбит, в ОБОРОТАХ.
    float yawTurns = 0.12f;
    float pitchTurns = 0.09f;
    /// Удаление от центра системы.
    float distance = 46.0f;

    /// Какое тело в фокусе. 0xFFFFFFFF — вся система целиком.
    uint32_t focusOrbit = 0xFFFFFFFFu;
};

/// Что именно рисует пакет.
///
/// Нужно не отрисовке — ей хватает сетки и текстуры, — а ПРОВЕРКАМ.
/// Без вида пакета тест не может отличить постройку от планеты иначе
/// как по дескрипторам, а дескрипторы выдаёт устройство, которого
/// в тесте нет. Заодно порядок сборки кадра становится читаемым.
enum class MeshKind : uint8_t {
    Sky,
    Star,
    Orbit,
    Planet,
    Ring,
    Structure,
    Atmosphere,
    Corona,
};

/// Один вызов отрисовки: сетка, текстура и её экземпляры.
struct MeshBatch {
    rhi::MeshHandle mesh = rhi::kInvalidMesh;
    rhi::TextureHandle texture = rhi::kInvalidTexture;
    MeshKind kind = MeshKind::Planet;
    /// Рисовать конвейером СВЕЧЕНИЯ: цвет складывается, глубина не пишется.
    /// Так идут корона звезды и атмосферы — всё, что светится, а не заслоняет.
    bool glow = false;
    std::vector<rhi::MeshInstance> instances;
};

/// Где на экране оказалось тело. Нужно и выбору мышью, и подписям.
struct PlanetScreenSpot {
    uint32_t orbit = 0;
    uint32_t planetId = 0;
    /// Доли экрана: 0..1 по обеим осям, начало в левом верхнем углу.
    float screenX = 0.0f, screenY = 0.0f;
    /// Радиус тела на экране, в долях высоты экрана.
    float screenRadius = 0.0f;
    /// Расстояние до камеры. Ближайшее тело выигрывает попадание мышью.
    float depth = 0.0f;
    bool visible = false;
};

struct SystemFrame {
    rhi::Camera3D camera;
    /// Пакеты в порядке отрисовки: сначала непрозрачное, потом орбиты
    /// и атмосферы. Порядок задаётся здесь, а не вызывающим: он часть
    /// того, как сцена выглядит, а не деталь отрисовки.
    std::vector<MeshBatch> batches;
    std::vector<PlanetScreenSpot> spots;

    void clear() {
        batches.clear();
        spots.clear();
    }
};

class SystemView {
public:
    void setAssets(const SystemAssets* assets) { assets_ = assets; }

    /// Собрать кадр системы.
    ///
    /// `empire` — чья это картинка: свои планеты подсвечиваются, чужие
    /// приглушаются. `aspect` — отношение сторон цели.
    void build(const sim::Galaxy& galaxy, const game::WorldView& world, uint32_t system,
               uint32_t empire, const SystemCamera& camera, float aspect,
               SystemFrame& out) const;

    /// Какая орбита под точкой экрана. 0xFFFFFFFF — мимо.
    static uint32_t pick(const SystemFrame& frame, float screenX, float screenY);

private:
    const SystemAssets* assets_ = nullptr;
};

}  // namespace pw::render
