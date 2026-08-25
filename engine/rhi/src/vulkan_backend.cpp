// pw_rhi — реализация на Vulkan.
//
// Один бэкенд на пять платформ. На Apple он работает поверх Metal через
// MoltenVK, что требует расширений переносимости — они запрашиваются ниже
// по факту наличия, а не по #ifdef: так же ведут себя и другие реализации
// поверх чужих API, и жёсткая привязка к платформе тут только мешает.

#include "pw/rhi/rhi.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>

#include "pw/core/log.h"
#include "pw/platform/window.h"

namespace pw::rhi {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

/// Формат цели. RGBA8 выбран ради считывания: байты ложатся в память в том
/// же порядке, что ждёт PNG, и не нужно менять местами каналы.
constexpr VkFormat kOffscreenFormat = VK_FORMAT_R8G8B8A8_UNORM;

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
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

bool hasInstanceExtension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

bool hasInstanceLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> props(count);
    vkEnumerateInstanceLayerProperties(&count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.layerName, name) == 0) return true;
    }
    return false;
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t typeBits,
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

}  // namespace

// ---------------------------------------------------------------------------

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

    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

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

// --- экземпляр -------------------------------------------------------------

bool Device::Impl::createInstance(const DeviceDesc& desc) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "PlanetWar";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "pw";
    app.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> extensions;
    if (!headless) {
        const int count = Window::vulkanInstanceExtensions(nullptr, 0);
        std::vector<const char*> names(size_t(count > 0 ? count : 0));
        if (count > 0) Window::vulkanInstanceExtensions(names.data(), count);
        extensions.insert(extensions.end(), names.begin(), names.end());
    }

    std::vector<const char*> layers;
    if (desc.validation) {
        if (hasInstanceLayer(kValidationLayer) &&
            hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            layers.push_back(kValidationLayer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            PW_LOG_WARN("rhi", "слои проверки запрошены, но недоступны");
        }
    }

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};

    // MoltenVK на маках и айфонах — реализация поверх Metal, и она объявляет
    // себя «непереносимой». Без этих двух строк Vulkan просто не увидит
    // ни одного устройства на Apple. Проверяем наличие, а не платформу:
    // так же ведут себя и другие обёртки над чужими графическими API.
    if (hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    info.pApplicationInfo = &app;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    info.enabledLayerCount = uint32_t(layers.size());
    info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    const VkResult result = vkCreateInstance(&info, nullptr, &instance);
    if (result != VK_SUCCESS) return fail("не удалось создать экземпляр Vulkan", result);

    if (!layers.empty()) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT dbg{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbg.pfnUserCallback = debugCallback;
            create(instance, &dbg, nullptr, &messenger);
        }
    }
    return true;
}

// --- физическое устройство -------------------------------------------------

bool Device::Impl::pickPhysical() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) return fail("устройств с поддержкой Vulkan не найдено");

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    // Дискретная видеокарта лучше встроенной, встроенная лучше программной.
    // Но программную не отвергаем: именно на ней рендер проверяется в CI,
    // где никакой видеокарты нет вовсе.
    auto score = [](VkPhysicalDeviceType type) {
        switch (type) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 3;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 1;
            default:                                     return 0;
        }
    };

    int best = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);

        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());

        for (uint32_t i = 0; i < families; ++i) {
            if (!(queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (!headless && surface != VK_NULL_HANDLE) {
                VkBool32 supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported);
                if (!supported) continue;
            }
            const int value = score(props.deviceType);
            if (value > best) {
                best = value;
                physical = candidate;
                graphicsFamily = i;
                adapter = props.deviceName;
            }
            break;
        }
    }

    if (physical == VK_NULL_HANDLE) return fail("нет устройства с графической очередью");
    PW_LOG_INFO("rhi", "устройство: %s", adapter.c_str());
    return true;
}

bool Device::Impl::createLogicalDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = graphicsFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    std::vector<const char*> extensions;
    if (!headless) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // Обязательное требование MoltenVK: реализация неполная относительно
    // спецификации, и это надо объявить явно.
    if (hasDeviceExtension(physical, "VK_KHR_portability_subset")) {
        extensions.push_back("VK_KHR_portability_subset");
    }

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    info.pEnabledFeatures = &features;

    const VkResult result = vkCreateDevice(physical, &info, nullptr, &device);
    if (result != VK_SUCCESS) return fail("не удалось создать логическое устройство", result);

    vkGetDeviceQueue(device, graphicsFamily, 0, &queue);
    return true;
}

// --- цель отрисовки --------------------------------------------------------

bool Device::Impl::createSwapchainTarget(const DeviceDesc& desc) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data());
    if (formats.empty()) return fail("поверхность не предлагает ни одного формата");

    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    format = chosen.format;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {  // размер диктуем мы
        extent.width = uint32_t(std::clamp(desc.width, int(caps.minImageExtent.width),
                                           int(caps.maxImageExtent.width)));
        extent.height = uint32_t(std::clamp(desc.height, int(caps.minImageExtent.height),
                                            int(caps.maxImageExtent.height)));
    }
    width = int(extent.width);
    height = int(extent.height);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO поддерживается всегда и не рвёт кадр. Для стратегии этого хватает:
    // соревновательное преимущество тут даёт не частота кадров, а решения.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;

    const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);
    if (result != VK_SUCCESS) return fail("не удалось создать цепочку показа", result);

    uint32_t actual = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &actual, nullptr);
    swapImages.resize(actual);
    vkGetSwapchainImagesKHR(device, swapchain, &actual, swapImages.data());

    swapViews.resize(actual);
    for (uint32_t i = 0; i < actual; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = swapImages[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = chosen.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &view, nullptr, &swapViews[i]) != VK_SUCCESS) {
            return fail("не удалось создать представление изображения цепочки");
        }
    }

    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device, &sem, nullptr, &acquired);
    vkCreateSemaphore(device, &sem, nullptr, &rendered);

    PW_LOG_INFO("rhi", "цепочка показа: %dx%d, изображений %u", width, height, actual);
    return true;
}

bool Device::Impl::createOffscreenTarget() {
    format = kOffscreenFormat;

    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    image.extent = {uint32_t(width), uint32_t(height), 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC — чтобы кадр можно было скопировать в память и сравнить
    // с эталоном. Ради этого весь безголовый путь и существует.
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &image, nullptr, &offImage) != VK_SUCCESS) {
        return fail("не удалось создать изображение цели");
    }

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(device, offImage, &reqs);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex =
        findMemoryType(physical, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) return fail("нет подходящей памяти для цели");
    if (vkAllocateMemory(device, &alloc, nullptr, &offMemory) != VK_SUCCESS) {
        return fail("не удалось выделить память под цель");
    }
    vkBindImageMemory(device, offImage, offMemory, 0);

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = offImage;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view, nullptr, &offView) != VK_SUCCESS) {
        return fail("не удалось создать представление цели");
    }

    // Буфер для считывания: видим процессору, чтобы прочитать пиксели.
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = VkDeviceSize(width) * VkDeviceSize(height) * 4;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &buffer, nullptr, &readBuffer) != VK_SUCCESS) {
        return fail("не удалось создать буфер считывания");
    }

    VkMemoryRequirements bufReqs{};
    vkGetBufferMemoryRequirements(device, readBuffer, &bufReqs);
    VkMemoryAllocateInfo bufAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bufAlloc.allocationSize = bufReqs.size;
    bufAlloc.memoryTypeIndex = findMemoryType(
        physical, bufReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bufAlloc.memoryTypeIndex == UINT32_MAX) return fail("нет памяти, видимой процессору");
    if (vkAllocateMemory(device, &bufAlloc, nullptr, &readMemory) != VK_SUCCESS) {
        return fail("не удалось выделить память под считывание");
    }
    vkBindBufferMemory(device, readBuffer, readMemory, 0);

    PW_LOG_INFO("rhi", "безголовая цель: %dx%d", width, height);
    return true;
}

bool Device::Impl::createRenderPass() {
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Куда изображение приходит в конце прохода: в оконном режиме — на экран,
    // в безголовом — источником копирования в память.
    color.finalLayout = headless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                 : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;

    if (vkCreateRenderPass(device, &info, nullptr, &renderPass) != VK_SUCCESS) {
        return fail("не удалось создать проход отрисовки");
    }
    return true;
}

bool Device::Impl::createFramebuffers(const std::vector<VkImageView>& views) {
    framebuffers.resize(views.size());
    for (size_t i = 0; i < views.size(); ++i) {
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = renderPass;
        info.attachmentCount = 1;
        info.pAttachments = &views[i];
        info.width = uint32_t(width);
        info.height = uint32_t(height);
        info.layers = 1;
        if (vkCreateFramebuffer(device, &info, nullptr, &framebuffers[i]) != VK_SUCCESS) {
            return fail("не удалось создать кадровый буфер");
        }
    }
    return true;
}

bool Device::Impl::createCommandResources() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsFamily;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return fail("не удалось создать пул команд");
    }

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &alloc, &cmd) != VK_SUCCESS) {
        return fail("не удалось выделить буфер команд");
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    // Сигнальный с самого начала. beginFrame первым делом ждёт этот барьер,
    // и несигнальный барьер, которого никто ещё не отправлял в очередь,
    // подвесил бы программу на первом же кадре навсегда.
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        return fail("не удалось создать барьер синхронизации");
    }
    return true;
}

void Device::Impl::destroy() {
    if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);

    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
    for (VkFramebuffer fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    framebuffers.clear();
    if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);

    for (VkImageView view : swapViews) vkDestroyImageView(device, view, nullptr);
    swapViews.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    if (acquired) vkDestroySemaphore(device, acquired, nullptr);
    if (rendered) vkDestroySemaphore(device, rendered, nullptr);

    if (offView) vkDestroyImageView(device, offView, nullptr);
    if (offImage) vkDestroyImage(device, offImage, nullptr);
    if (offMemory) vkFreeMemory(device, offMemory, nullptr);
    if (readBuffer) vkDestroyBuffer(device, readBuffer, nullptr);
    if (readMemory) vkFreeMemory(device, readMemory, nullptr);

    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    if (device) vkDestroyDevice(device, nullptr);

    if (messenger) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(instance, messenger, nullptr);
    }
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);

    *this = Impl{};
}

// --- публичный интерфейс ---------------------------------------------------

Device::Device() : impl_(new Impl()) {}

Device::~Device() {
    shutdown();
    delete impl_;
}

bool Device::init(const DeviceDesc& desc) {
    Impl& d = *impl_;
    d.headless = (desc.window == nullptr) || desc.window->headless();
    d.width = desc.width;
    d.height = desc.height;

    if (!d.createInstance(desc)) return false;

    if (!d.headless) {
        void* raw = nullptr;
        if (!desc.window->createVulkanSurface(d.instance, &raw)) {
            return d.fail("не удалось создать поверхность окна");
        }
        d.surface = static_cast<VkSurfaceKHR>(raw);
        desc.window->framebufferSize(d.width, d.height);
    }

    if (!d.pickPhysical()) return false;
    if (!d.createLogicalDevice()) return false;

    if (d.headless) {
        if (!d.createOffscreenTarget()) return false;
        if (!d.createRenderPass()) return false;
        if (!d.createFramebuffers({d.offView})) return false;
    } else {
        if (!d.createSwapchainTarget(desc)) return false;
        if (!d.createRenderPass()) return false;
        if (!d.createFramebuffers(d.swapViews)) return false;
    }

    return d.createCommandResources();
}

void Device::shutdown() {
    if (impl_ && impl_->instance != VK_NULL_HANDLE) impl_->destroy();
}

bool Device::valid() const { return impl_ && impl_->device != VK_NULL_HANDLE; }
const std::string& Device::adapterName() const { return impl_->adapter; }
bool Device::headless() const { return impl_->headless; }
int Device::targetWidth() const { return impl_->width; }
int Device::targetHeight() const { return impl_->height; }
const std::string& Device::lastError() const { return impl_->error; }

bool Device::createPipeline(const std::vector<uint8_t>& vertexSpirv,
                            const std::vector<uint8_t>& fragmentSpirv) {
    Impl& d = *impl_;
    if (vertexSpirv.empty() || fragmentSpirv.empty()) {
        return d.fail("пустой SPIR-V: шейдеры не собраны или не найдены");
    }

    auto makeModule = [&](const std::vector<uint8_t>& code, VkShaderModule& out) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size();
        info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        return vkCreateShaderModule(d.device, &info, nullptr, &out) == VK_SUCCESS;
    };

    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    if (!makeModule(vertexSpirv, vs) || !makeModule(fragmentSpirv, fs)) {
        if (vs) vkDestroyShaderModule(d.device, vs, nullptr);
        if (fs) vkDestroyShaderModule(d.device, fs, nullptr);
        return d.fail("не удалось создать шейдерный модуль");
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // Буфера вершин нет: координаты треугольника лежат в самом шейдере.
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{0.0f, 0.0f, float(d.width), float(d.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {uint32_t(d.width), uint32_t(d.height)}};
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreatePipelineLayout(d.device, &layoutInfo, nullptr, &d.layout) != VK_SUCCESS) {
        vkDestroyShaderModule(d.device, vs, nullptr);
        vkDestroyShaderModule(d.device, fs, nullptr);
        return d.fail("не удалось создать раскладку конвейера");
    }

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.layout = d.layout;
    info.renderPass = d.renderPass;
    info.subpass = 0;

    const VkResult result =
        vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &info, nullptr, &d.pipeline);

    vkDestroyShaderModule(d.device, vs, nullptr);
    vkDestroyShaderModule(d.device, fs, nullptr);

    if (result != VK_SUCCESS) return d.fail("не удалось создать графический конвейер", result);
    return true;
}

bool Device::beginFrame(const ClearColor& clear) {
    Impl& d = *impl_;
    if (d.frameOpen) return d.fail("beginFrame вызван повторно без endFrame");

    vkWaitForFences(d.device, 1, &d.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(d.device, 1, &d.fence);

    d.imageIndex = 0;
    if (!d.headless) {
        const VkResult acquire = vkAcquireNextImageKHR(
            d.device, d.swapchain, UINT64_MAX, d.acquired, VK_NULL_HANDLE, &d.imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            // Окно изменило размер. Пересоздание цепочки — следующий шаг,
            // пока честно сообщаем, что кадр пропущен.
            return d.fail("цепочка показа устарела: нужен пересоздание при изменении размера");
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            return d.fail("не удалось получить изображение цепочки", acquire);
        }
    }

    vkResetCommandBuffer(d.cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(d.cmd, &begin) != VK_SUCCESS) {
        return d.fail("не удалось начать запись команд");
    }

    VkClearValue clearValue{};
    clearValue.color = {{clear.r, clear.g, clear.b, clear.a}};

    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = d.renderPass;
    pass.framebuffer = d.framebuffers[d.headless ? 0 : d.imageIndex];
    pass.renderArea = {{0, 0}, {uint32_t(d.width), uint32_t(d.height)}};
    pass.clearValueCount = 1;
    pass.pClearValues = &clearValue;
    vkCmdBeginRenderPass(d.cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    d.frameOpen = true;
    return true;
}

void Device::draw(uint32_t vertexCount) {
    Impl& d = *impl_;
    if (!d.frameOpen || d.pipeline == VK_NULL_HANDLE) return;
    vkCmdBindPipeline(d.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
    vkCmdDraw(d.cmd, vertexCount, 1, 0, 0);
}

bool Device::endFrame() {
    Impl& d = *impl_;
    if (!d.frameOpen) return d.fail("endFrame без beginFrame");
    d.frameOpen = false;

    vkCmdEndRenderPass(d.cmd);

    // В безголовом режиме сразу копируем результат в буфер, видимый
    // процессору, — тем же буфером команд, без лишней синхронизации.
    if (d.headless) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {uint32_t(d.width), uint32_t(d.height), 1};
        vkCmdCopyImageToBuffer(d.cmd, d.offImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               d.readBuffer, 1, &region);
    }

    if (vkEndCommandBuffer(d.cmd) != VK_SUCCESS) return d.fail("не удалось завершить запись команд");

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &d.cmd;
    if (!d.headless) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &d.acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &d.rendered;
    }

    if (vkQueueSubmit(d.queue, 1, &submit, d.fence) != VK_SUCCESS) {
        return d.fail("не удалось отправить команды в очередь");
    }

    if (!d.headless) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &d.rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &d.swapchain;
        present.pImageIndices = &d.imageIndex;
        vkQueuePresentKHR(d.queue, &present);
    } else {
        // Считывание идёт сразу после кадра, поэтому дожидаемся его здесь.
        vkWaitForFences(d.device, 1, &d.fence, VK_TRUE, UINT64_MAX);
    }
    return true;
}

bool Device::readback(std::vector<Rgba8>& out) const {
    Impl& d = *impl_;
    if (!d.headless) return d.fail("считывание доступно только в безголовом режиме");

    void* mapped = nullptr;
    if (vkMapMemory(d.device, d.readMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        return d.fail("не удалось отобразить память считывания");
    }

    out.resize(size_t(d.width) * size_t(d.height));
    std::memcpy(out.data(), mapped, out.size() * sizeof(Rgba8));
    vkUnmapMemory(d.device, d.readMemory);
    return true;
}

}  // namespace pw::rhi
