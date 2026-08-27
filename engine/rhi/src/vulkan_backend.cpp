// pw_rhi — реализация на Vulkan: устройство, цель отрисовки, кадр.
//
// Один бэкенд на пять платформ. На Apple он работает поверх Metal через
// MoltenVK, что требует расширений переносимости — они запрашиваются ниже
// по факту наличия, а не по #ifdef: так же ведут себя и другие реализации
// поверх чужих API, и жёсткая привязка к платформе тут только мешает.
//
// Рисование (спрайты, линии, текстуры) живёт в vulkan_sprites.cpp: оба
// файла делят состояние через внутренний заголовок vulkan_impl.h.

#include "vulkan_impl.h"

namespace pw::rhi {

// ---------------------------------------------------------------------------


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

    // Вложение глубины есть ВСЕГДА, даже когда рисуется одна плоская карта.
    //
    // Проход в движке один, а конвейеров несколько, и совместимость
    // конвейера с проходом определяется набором вложений. Заводить второй
    // проход ради того, чтобы у спрайтов не было глубины, значило бы
    // удваивать кадровые буферы и переключать проход посреди кадра —
    // ради состояния, которое выключается одной строкой в конвейере.
    VkAttachmentDescription depth{};
    depth.format = depthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // Глубина не переживает кадр: она нужна только внутри прохода.
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const VkAttachmentDescription attachments[] = {color, depth};

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;

    if (vkCreateRenderPass(device, &info, nullptr, &renderPass) != VK_SUCCESS) {
        return fail("не удалось создать проход отрисовки");
    }
    return true;
}

bool Device::Impl::createDepthTarget() {
    // Формат выбирается из того, что поддерживает устройство. D32 есть
    // почти везде, но на мобильных бывает только D24S8 или D16 —
    // а мобильные для нас цель, а не «когда-нибудь потом».
    const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                   VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    depthFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physical, candidate, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depthFormat = candidate;
            break;
        }
    }
    if (depthFormat == VK_FORMAT_UNDEFINED) {
        return fail("устройство не поддерживает ни одного формата глубины");
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = depthFormat;
    imageInfo.extent = {uint32_t(width), uint32_t(height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &depthImage) != VK_SUCCESS) {
        return fail("не удалось создать изображение глубины");
    }

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(device, depthImage, &reqs);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex =
        findMemoryType(physical, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) return fail("нет памяти под буфер глубины");
    if (vkAllocateMemory(device, &alloc, nullptr, &depthMemory) != VK_SUCCESS) {
        return fail("не удалось выделить память под буфер глубины");
    }
    vkBindImageMemory(device, depthImage, depthMemory, 0);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &viewInfo, nullptr, &depthView) != VK_SUCCESS) {
        return fail("не удалось создать вид буфера глубины");
    }
    return true;
}

bool Device::Impl::createFramebuffers(const std::vector<VkImageView>& views) {
    framebuffers.resize(views.size());
    for (size_t i = 0; i < views.size(); ++i) {
        const VkImageView attachments[] = {views[i], depthView};
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = renderPass;
        info.attachmentCount = 2;
        info.pAttachments = attachments;
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

    destroyDrawing();

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

    if (depthView) vkDestroyImageView(device, depthView, nullptr);
    if (depthImage) vkDestroyImage(device, depthImage, nullptr);
    if (depthMemory) vkFreeMemory(device, depthMemory, nullptr);

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
        if (!d.createDepthTarget()) return false;
        if (!d.createRenderPass()) return false;
        if (!d.createFramebuffers({d.offView})) return false;
    } else {
        if (!d.createSwapchainTarget(desc)) return false;
        if (!d.createDepthTarget()) return false;
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

    VkClearValue clearValues[2]{};
    clearValues[0].color = {{clear.r, clear.g, clear.b, clear.a}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = d.renderPass;
    pass.framebuffer = d.framebuffers[d.headless ? 0 : d.imageIndex];
    pass.renderArea = {{0, 0}, {uint32_t(d.width), uint32_t(d.height)}};
    pass.clearValueCount = 2;
    pass.pClearValues = clearValues;
    vkCmdBeginRenderPass(d.cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    d.spriteUsed = 0;
    d.lineUsed = 0;
    d.meshUsed = 0;
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
