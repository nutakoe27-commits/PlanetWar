#pragma once

// Внутреннее устройство бэкенда на Vulkan.
//
// Заголовок ВНУТРЕННИЙ: лежит в src/, наружу не ставится и в публичный
// интерфейс не входит. Существует ровно затем, чтобы бэкенд не оказался
// одним файлом на полторы тысячи строк: базовая часть (устройство, цель,
// кадр) и рисование (спрайты, линии, текстуры) разъезжаются по разным
// файлам, но делят одно состояние.

#include "pw/rhi/rhi.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "pw/core/log.h"
#include "pw/platform/window.h"

namespace pw::rhi {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

/// Формат цели. RGBA8 выбран ради считывания: байты ложатся в память в том
/// же порядке, что ждёт PNG, и не нужно менять местами каналы.
constexpr VkFormat kOffscreenFormat = VK_FORMAT_R8G8B8A8_UNORM;


inline VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        PW_LOG_ERROR("rhi", "валидация: %s", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        PW_LOG_WARN("rhi", "валидация: %s", data->pMessage);
    }
    return VK_FALSE;
}

inline bool hasInstanceExtension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

inline bool hasInstanceLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> props(count);
    vkEnumerateInstanceLayerProperties(&count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.layerName, name) == 0) return true;
    }
    return false;
}

inline bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

inline uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t typeBits,
                        VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return UINT32_MAX;
}



/// Кадровый буфер под спрайты и линии.
///
/// Живёт в памяти, видимой процессору, и остаётся отображённым всё время:
/// данные меняются каждый кадр, и гонять их через промежуточный буфер
/// значило бы платить копированием за то, что и так копируется один раз.
struct FrameBufferVk {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
};

/// Загруженная сетка: вершины и индексы в памяти устройства.
struct MeshVk {
    VkBuffer vertices = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indices = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
};

/// Загруженная текстура.
struct TextureVk {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
};

struct Device::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;

    bool headless = true;
    int width = 0;
    int height = 0;
    VkFormat format = kOffscreenFormat;

    // Оконный путь.
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;

    // Безголовый путь.
    VkImage offImage = VK_NULL_HANDLE;
    VkDeviceMemory offMemory = VK_NULL_HANDLE;
    VkImageView offView = VK_NULL_HANDLE;
    VkBuffer readBuffer = VK_NULL_HANDLE;
    VkDeviceMemory readMemory = VK_NULL_HANDLE;

    // Буфер глубины. Один на цель: проходов у нас один, и разделять
    // глубину между кадрами swapchain не нужно — кадры не пересекаются
    // во времени, потому что кадр ждёт забора.
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    // --- спрайты и линии ---
    VkPipelineLayout spriteLayout = VK_NULL_HANDLE;
    VkPipeline spritePipeline = VK_NULL_HANDLE;
    VkPipelineLayout lineLayout = VK_NULL_HANDLE;
    VkPipeline linePipeline = VK_NULL_HANDLE;

    // --- сетки ---
    VkPipelineLayout meshLayout = VK_NULL_HANDLE;
    VkPipeline meshPipeline = VK_NULL_HANDLE;
    FrameBufferVk meshBuffer;
    VkDeviceSize meshUsed = 0;
    std::vector<MeshVk> meshes;
    Camera3D camera3d;

    VkDescriptorSetLayout samplerLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    FrameBufferVk spriteBuffer;
    FrameBufferVk lineBuffer;
    // Сколько уже занято в этом кадре: буфер один на кадр, а вызовов
    // отрисовки может быть несколько (разные атласы, слои карты).
    VkDeviceSize spriteUsed = 0;
    VkDeviceSize lineUsed = 0;

    std::vector<TextureVk> textures;

    Camera camera;

    bool createSamplerResources();
    bool ensureBuffer(FrameBufferVk& target, VkDeviceSize needed, VkBufferUsageFlags usage);
    void destroyBuffer(FrameBufferVk& target);
    bool buildPipeline(const std::vector<uint8_t>& vertexSpirv,
                       const std::vector<uint8_t>& fragmentSpirv,
                       bool textured, VkPrimitiveTopology topology,
                       const VkVertexInputBindingDescription* bindings, uint32_t bindingCount,
                       const VkVertexInputAttributeDescription* attributes,
                       uint32_t attributeCount,
                       VkPipelineLayout& outLayout, VkPipeline& outPipeline,
                       uint32_t pushBytes = 16, bool depth = false, bool cull = false,
                       bool blend = true);
    /// Залить данные в свежий буфер, живущий до конца работы устройства.
    bool uploadStaticBuffer(const void* data, VkDeviceSize bytes, VkBufferUsageFlags usage,
                            VkBuffer& outBuffer, VkDeviceMemory& outMemory);
    bool createDepthTarget();
    void destroyMeshes();
    void destroyDrawing();

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    uint32_t imageIndex = 0;
    bool frameOpen = false;
    std::string adapter;
    std::string error;

    bool fail(const char* what, VkResult result = VK_SUCCESS) {
        error = what;
        if (result != VK_SUCCESS) error += " (VkResult " + std::to_string(int(result)) + ")";
        PW_LOG_ERROR("rhi", "%s", error.c_str());
        return false;
    }

    bool createInstance(const DeviceDesc& desc);
    bool pickPhysical();
    bool createLogicalDevice();
    bool createSwapchainTarget(const DeviceDesc& desc);
    bool createOffscreenTarget();
    bool createRenderPass();
    bool createFramebuffers(const std::vector<VkImageView>& views);
    bool createCommandResources();
    void destroy();
};

}  // namespace pw::rhi
