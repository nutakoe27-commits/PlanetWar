// pw_rhi — трёхмерная часть: сетки, глубина, свет.
//
// Карта галактики остаётся плоской, и это правильно: карта — схема, объём
// ей ни к чему. А вид системы — место, куда игрок приходит СМОТРЕТЬ, и там
// плоские кружки на орбитах читаются как таблица, а не как мир.
//
// Отсюда всё устройство этого файла: один инстансированный вызов на сетку,
// свет от единственного источника (в звёздной системе светит звезда, второму
// источнику взяться неоткуда) и матрица модели, ужатая до трёх осей
// и переноса.

#include "vulkan_impl.h"

#include <cmath>

namespace pw::rhi {

namespace {

/// Что уезжает в push-константах сеточного конвейера.
///
/// Девяносто шесть байт при гарантированных Vulkan ста двадцати восьми.
/// Uniform-буфер ради этого значил бы дескрипторы, выравнивание и
/// синхронизацию между кадрами — за данные, которые меняются раз в кадр.
struct MeshPush {
    float viewProjection[16];
    float lightPosition[3];
    float ambient;
    float eyePosition[3];
    float lightPadding;
    float lightColor[3];
    float reserved;
};
static_assert(sizeof(MeshPush) == 112, "push-константы обязаны укладываться в 128 байт");

struct Vec3 {
    float x, y, z;
};

Vec3 subtract(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 normalise(const Vec3& v) {
    const float length = std::sqrt(dot(v, v));
    if (length <= 1e-6f) return {0.0f, 0.0f, 1.0f};
    return {v.x / length, v.y / length, v.z / length};
}

}  // namespace

/// Матрица «вид умножить на проекцию», по столбцам, как ждёт GLSL.
///
/// Считается здесь, на процессоре, и в float — это РИСОВАНИЕ, а не
/// симуляция. Правило «никакой плавающей точки» действует в pw_sim,
/// pw_net и pw_game, где от неё зависит воспроизводимость мира; на
/// картинку оно не распространяется и распространяться не должно.
void viewProjectionMatrix(const Camera3D& camera, float aspect, float out[16]) {
    const Vec3 eye{camera.eyeX, camera.eyeY, camera.eyeZ};
    const Vec3 target{camera.targetX, camera.targetY, camera.targetZ};
    const Vec3 up{camera.upX, camera.upY, camera.upZ};

    // Правая тройка, взгляд вдоль -forward: обычная схема Vulkan.
    const Vec3 forward = normalise(subtract(target, eye));
    const Vec3 right = normalise(cross(forward, up));
    const Vec3 trueUp = cross(right, forward);

    // Угол задан в ОБОРОТАХ, как и все углы в проекте. Половина угла
    // в радианах: turns * 2pi / 2 = turns * pi.
    const float halfAngle = camera.fovTurns * 3.14159265358979323846f;
    const float focal = 1.0f / std::tan(halfAngle);

    const float nearPlane = camera.nearPlane;
    const float farPlane = camera.farPlane;

    // Ось Y в отсечённом пространстве Vulkan смотрит ВНИЗ, поэтому строка
    // «вверх» берётся со знаком минус. Без этого сцена оказывается
    // перевёрнутой — та же ошибка, что однажды перевернула спрайты.
    float view[16] = {
        right.x,          trueUp.x * -1.0f,  forward.x * -1.0f, 0.0f,
        right.y,          trueUp.y * -1.0f,  forward.y * -1.0f, 0.0f,
        right.z,          trueUp.z * -1.0f,  forward.z * -1.0f, 0.0f,
        -dot(right, eye), dot(trueUp, eye),  dot(forward, eye), 1.0f,
    };

    // Глубина в Vulkan лежит в [0, 1], а не в [-1, 1].
    float projection[16] = {};
    projection[0] = focal / aspect;
    projection[5] = focal;
    projection[10] = farPlane / (nearPlane - farPlane);
    projection[11] = -1.0f;
    projection[14] = (nearPlane * farPlane) / (nearPlane - farPlane);

    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += projection[k * 4 + row] * view[column * 4 + k];
            }
            out[column * 4 + row] = sum;
        }
    }
}

bool projectPoint(const Camera3D& camera, float aspect, float x, float y, float z,
                  float& outX, float& outY, float& outDepth) {
    float matrix[16];
    viewProjectionMatrix(camera, aspect, matrix);

    const float clipX = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
    const float clipY = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
    const float clipW = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];

    outDepth = clipW;
    if (clipW <= 1e-4f) return false;   // точка за камерой

    // Отсечённое пространство Vulkan: X и Y в [-1, 1], причём Y смотрит вниз,
    // то есть уже так, как считает экран.
    outX = (clipX / clipW) * 0.5f + 0.5f;
    outY = (clipY / clipW) * 0.5f + 0.5f;
    return true;
}

// ---------------------------------------------------------------------------
// Сетки
// ---------------------------------------------------------------------------

void Device::Impl::destroyMeshes() {
    // Буфер экземпляров жил дольше устройства: сетки чистились, а он —
    // нет. Проверочные слои Vulkan назвали это первым же выходом из игры.
    destroyBuffer(meshBuffer);

    for (MeshVk& mesh : meshes) {
        if (mesh.vertices) vkDestroyBuffer(device, mesh.vertices, nullptr);
        if (mesh.vertexMemory) vkFreeMemory(device, mesh.vertexMemory, nullptr);
        if (mesh.indices) vkDestroyBuffer(device, mesh.indices, nullptr);
        if (mesh.indexMemory) vkFreeMemory(device, mesh.indexMemory, nullptr);
    }
    meshes.clear();

    if (meshPipeline) vkDestroyPipeline(device, meshPipeline, nullptr);
    if (meshLayout) vkDestroyPipelineLayout(device, meshLayout, nullptr);
    if (glowPipeline) vkDestroyPipeline(device, glowPipeline, nullptr);
    if (glowLayout) vkDestroyPipelineLayout(device, glowLayout, nullptr);
    meshPipeline = VK_NULL_HANDLE;
    meshLayout = VK_NULL_HANDLE;
    glowPipeline = VK_NULL_HANDLE;
    glowLayout = VK_NULL_HANDLE;
}

/// Залить данные в свежий буфер в памяти, видимой процессору.
///
/// Модели грузятся один раз за запуск и после этого только читаются,
/// поэтому промежуточный буфер и копирование в память устройства здесь
/// не окупаются: сетка планеты — это десятки килобайт.
bool Device::Impl::uploadStaticBuffer(const void* data, VkDeviceSize bytes,
                                      VkBufferUsageFlags usage, VkBuffer& outBuffer,
                                      VkDeviceMemory& outMemory) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &outBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device, outBuffer, &reqs);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = findMemoryType(physical, reqs.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(device, &alloc, nullptr, &outMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, outBuffer, outMemory, 0);

    void* mapped = nullptr;
    if (vkMapMemory(device, outMemory, 0, bytes, 0, &mapped) != VK_SUCCESS) return false;
    std::memcpy(mapped, data, size_t(bytes));
    vkUnmapMemory(device, outMemory);
    return true;
}

MeshHandle Device::createMesh(const MeshVertex* vertices, size_t vertexCount,
                              const uint32_t* indices, size_t indexCount) {
    Impl& d = *impl_;
    if (vertices == nullptr || indices == nullptr || vertexCount == 0 || indexCount == 0) {
        d.fail("пустая сетка");
        return kInvalidMesh;
    }

    MeshVk mesh;
    mesh.indexCount = uint32_t(indexCount);

    if (!d.uploadStaticBuffer(vertices, VkDeviceSize(vertexCount * sizeof(MeshVertex)),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh.vertices,
                              mesh.vertexMemory)) {
        d.fail("не удалось загрузить вершины сетки");
        return kInvalidMesh;
    }
    if (!d.uploadStaticBuffer(indices, VkDeviceSize(indexCount * sizeof(uint32_t)),
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh.indices,
                              mesh.indexMemory)) {
        d.fail("не удалось загрузить индексы сетки");
        return kInvalidMesh;
    }

    d.meshes.push_back(mesh);
    // Ноль зарезервирован под «нет сетки», поэтому номера начинаются с единицы.
    return MeshHandle(d.meshes.size());
}

// ---------------------------------------------------------------------------
// Конвейер
// ---------------------------------------------------------------------------

bool Device::createMeshPipeline(const std::vector<uint8_t>& vertexSpirv,
                                const std::vector<uint8_t>& fragmentSpirv) {
    Impl& d = *impl_;

    // Две привязки: вершины сетки и данные экземпляра. Именно ради второй
    // всё и затевалось — десять планет и полсотни построек рисуются двумя
    // вызовами отрисовки, а не шестьюдесятью.
    const VkVertexInputBindingDescription bindings[] = {
        {0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(MeshInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };

    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, x)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, nx)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, u)},
        {3, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshInstance, axisX)},
        {4, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshInstance, axisY)},
        {5, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshInstance, axisZ)},
        {6, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshInstance, origin)},
        {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, r)},
        {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, emissive)},
    };

    return d.buildPipeline(vertexSpirv, fragmentSpirv, /*textured=*/true,
                           VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, bindings, 2, attributes,
                           uint32_t(sizeof(attributes) / sizeof(attributes[0])), d.meshLayout,
                           d.meshPipeline, uint32_t(sizeof(MeshPush)), /*depth=*/true,
                           /*cull=*/true, /*blend=*/true);
}

bool Device::createGlowPipeline(const std::vector<uint8_t>& vertexSpirv,
                                const std::vector<uint8_t>& fragmentSpirv) {
    Impl& d = *impl_;

    const VkVertexInputBindingDescription bindings[] = {
        {0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(MeshInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };

    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, x)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, nx)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, u)},
        {3, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshInstance, axisX)},
        {4, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshInstance, axisY)},
        {5, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshInstance, axisZ)},
        {6, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshInstance, origin)},
        {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, r)},
        {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, emissive)},
    };

    // Отсечения задних граней НЕТ: оболочку свечения смотрят и снаружи,
    // и изнутри — камера входит внутрь короны, стоит приблизиться.
    return d.buildPipeline(vertexSpirv, fragmentSpirv, /*textured=*/true,
                           VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, bindings, 2, attributes,
                           uint32_t(sizeof(attributes) / sizeof(attributes[0])), d.glowLayout,
                           d.glowPipeline, uint32_t(sizeof(MeshPush)), /*depth=*/true,
                           /*cull=*/false, /*blend=*/true, /*depthWrite=*/false,
                           /*additive=*/true);
}

void Device::setCamera3D(const Camera3D& camera) { impl_->camera3d = camera; }

// Параметры названы НЕ как поля Impl (`pipeline`, `layout`): метод берёт
// конвейер аргументом, а у Impl есть свои одноимённые поля, и перекрытие
// здесь читается как «а какой из двух имеется в виду». Компилятор на маке
// сообщает об этом (-Wshadow), gcc на Linux молчит.
void Device::Impl::drawMeshesWith(VkPipeline usePipeline, VkPipelineLayout useLayout,
                                  MeshHandle handle, const MeshInstance* instances,
                                  size_t count, TextureHandle texture) {
    Impl& d = *this;
    if (!d.frameOpen || usePipeline == VK_NULL_HANDLE) return;
    if (instances == nullptr || count == 0) return;
    if (handle == kInvalidMesh || handle > d.meshes.size()) return;
    if (texture == kInvalidTexture || texture > d.textures.size()) return;

    const MeshVk& mesh = d.meshes[handle - 1];
    VkDeviceSize bytes = VkDeviceSize(count * sizeof(MeshInstance));
    if (!d.ensureBuffer(d.meshBuffer, d.meshUsed + bytes,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        // Рисуем сколько влезло: буфер растёт только между кадрами.
        if (d.meshBuffer.capacity <= d.meshUsed) return;
        count = size_t((d.meshBuffer.capacity - d.meshUsed) / sizeof(MeshInstance));
        if (count == 0) return;
        bytes = VkDeviceSize(count * sizeof(MeshInstance));
    }

    std::memcpy(static_cast<uint8_t*>(d.meshBuffer.mapped) + d.meshUsed, instances,
                size_t(bytes));

    MeshPush push{};
    const float aspect = float(d.width) / float(d.height > 0 ? d.height : 1);
    viewProjectionMatrix(d.camera3d, aspect, push.viewProjection);
    push.lightPosition[0] = d.camera3d.lightX;
    push.lightPosition[1] = d.camera3d.lightY;
    push.lightPosition[2] = d.camera3d.lightZ;
    push.ambient = d.camera3d.ambient;
    push.eyePosition[0] = d.camera3d.eyeX;
    push.eyePosition[1] = d.camera3d.eyeY;
    push.eyePosition[2] = d.camera3d.eyeZ;
    push.lightColor[0] = d.camera3d.lightR;
    push.lightColor[1] = d.camera3d.lightG;
    push.lightColor[2] = d.camera3d.lightB;

    vkCmdBindPipeline(d.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, usePipeline);
    vkCmdPushConstants(d.cmd, useLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdBindDescriptorSets(d.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, useLayout, 0, 1,
                            &d.textures[texture - 1].set, 0, nullptr);

    const VkBuffer buffers[] = {mesh.vertices, d.meshBuffer.buffer};
    const VkDeviceSize offsets[] = {0, d.meshUsed};
    vkCmdBindVertexBuffers(d.cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(d.cmd, mesh.indices, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(d.cmd, mesh.indexCount, uint32_t(count), 0, 0, 0);

    d.meshUsed += bytes;
}

void Device::drawMeshes(MeshHandle handle, const MeshInstance* instances, size_t count,
                        TextureHandle texture) {
    impl_->drawMeshesWith(impl_->meshPipeline, impl_->meshLayout, handle, instances, count,
                          texture);
}

void Device::drawGlow(MeshHandle handle, const MeshInstance* instances, size_t count,
                      TextureHandle texture) {
    impl_->drawMeshesWith(impl_->glowPipeline, impl_->glowLayout, handle, instances, count,
                          texture);
}

}  // namespace pw::rhi
