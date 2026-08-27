// pw_rhi — рисование: спрайты, линии, текстуры.
//
// Вся карта галактики укладывается в два вызова отрисовки: один на линии
// гиперпутей, один на спрайты. Это не оптимизация ради оптимизации —
// систем на карте до полумиллиона, и вызов на каждую означал бы, что
// карта не рисуется вообще.

#include "vulkan_impl.h"

namespace pw::rhi {

namespace {

/// Камера в push-константах, а не в uniform-буфере.
///
/// Их всего шестнадцать байт, они меняются каждый кадр, и push-константы
/// ровно для этого и существуют: ни выделения, ни дескрипторов,
/// ни синхронизации между кадрами.
struct CameraPush {
    float centerX, centerY;
    float scaleX, scaleY;
};

}  // namespace

// ---------------------------------------------------------------------------
// Буферы
// ---------------------------------------------------------------------------

void Device::Impl::destroyBuffer(FrameBufferVk& target) {
    if (target.mapped != nullptr) {
        vkUnmapMemory(device, target.memory);
        target.mapped = nullptr;
    }
    if (target.buffer) vkDestroyBuffer(device, target.buffer, nullptr);
    if (target.memory) vkFreeMemory(device, target.memory, nullptr);
    target.buffer = VK_NULL_HANDLE;
    target.memory = VK_NULL_HANDLE;
    target.capacity = 0;
}

bool Device::Impl::ensureBuffer(FrameBufferVk& target, VkDeviceSize needed,
                                VkBufferUsageFlags usage) {
    if (target.capacity >= needed && target.buffer != VK_NULL_HANDLE) return true;

    // Внутри кадра — только запомнить и отказать. Почему пересоздавать
    // здесь нельзя, подробно написано у поля `wanted`.
    if (frameOpen) {
        if (needed > target.wanted) target.wanted = needed;
        return false;
    }


    // Растём с запасом вдвое: иначе кадр, в котором спрайтов стало на один
    // больше, перевыделял бы буфер каждый раз.
    VkDeviceSize capacity = target.capacity > 0 ? target.capacity : needed;
    while (capacity < needed) capacity *= 2;

    // Устройство может ещё читать старый буфер. Кадр закрыт, но команды
    // могли не завершиться — ждём, иначе освободим память из-под работы.
    vkDeviceWaitIdle(device);
    destroyBuffer(target);

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = capacity;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &target.buffer) != VK_SUCCESS) {
        return fail("не удалось создать буфер вершин");
    }

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device, target.buffer, &reqs);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = findMemoryType(physical, reqs.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) {
        return fail("нет памяти, видимой процессору, под буфер вершин");
    }
    if (vkAllocateMemory(device, &alloc, nullptr, &target.memory) != VK_SUCCESS) {
        return fail("не удалось выделить память под буфер вершин");
    }
    vkBindBufferMemory(device, target.buffer, target.memory, 0);

    // Отображаем один раз и держим отображённым: данные меняются каждый
    // кадр, и отображать-отображать заново значило бы платить за это
    // системным вызовом шестьдесят раз в секунду.
    if (vkMapMemory(device, target.memory, 0, capacity, 0, &target.mapped) != VK_SUCCESS) {
        return fail("не удалось отобразить буфер вершин");
    }
    target.capacity = capacity;
    if (target.wanted < capacity) target.wanted = capacity;
    return true;
}

bool Device::Impl::reserveFrameBuffers() {
    // Начальные размеры выбраны так, чтобы обычный кадр не просил добавки
    // ни разу. Просьба всё же возможна — тогда она исполняется здесь,
    // на кадр позже, и это ЕДИНСТВЕННОЕ безопасное место для роста.
    if (spriteBuffer.wanted < kInitialSpriteBytes) spriteBuffer.wanted = kInitialSpriteBytes;
    if (lineBuffer.wanted < kInitialLineBytes) lineBuffer.wanted = kInitialLineBytes;
    if (meshBuffer.wanted < kInitialMeshBytes) meshBuffer.wanted = kInitialMeshBytes;

    return ensureBuffer(spriteBuffer, spriteBuffer.wanted,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) &&
           ensureBuffer(lineBuffer, lineBuffer.wanted, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) &&
           ensureBuffer(meshBuffer, meshBuffer.wanted, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

// ---------------------------------------------------------------------------
// Текстуры
// ---------------------------------------------------------------------------

bool Device::Impl::createSamplerResources() {
    if (sampler != VK_NULL_HANDLE) return true;

    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // Линейная фильтрация: спрайты кораблей испечены в разрешении под
    // средний зум, и при отдалении без фильтрации они рассыпались бы в шум.
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // Края атласа не заворачиваем: спрайты лежат вплотную, и заворот
    // притащил бы в кадр соседний кадр анимации.
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
        return fail("не удалось создать сэмплер");
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &samplerLayout) !=
        VK_SUCCESS) {
        return fail("не удалось создать раскладку дескрипторов");
    }

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 64;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 64;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        return fail("не удалось создать пул дескрипторов");
    }
    return true;
}

TextureHandle Device::createTexture(int width, int height, const Rgba8* pixels) {
    Impl& d = *impl_;
    if (width <= 0 || height <= 0 || pixels == nullptr) {
        d.fail("пустая текстура");
        return kInvalidTexture;
    }
    if (!d.createSamplerResources()) return kInvalidTexture;

    TextureVk texture;
    texture.width = width;
    texture.height = height;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kOffscreenFormat;
    imageInfo.extent = {uint32_t(width), uint32_t(height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d.device, &imageInfo, nullptr, &texture.image) != VK_SUCCESS) {
        d.fail("не удалось создать изображение текстуры");
        return kInvalidTexture;
    }

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(d.device, texture.image, &reqs);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex =
        findMemoryType(d.physical, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(d.device, &alloc, nullptr, &texture.memory) != VK_SUCCESS) {
        vkDestroyImage(d.device, texture.image, nullptr);
        d.fail("не удалось выделить память под текстуру");
        return kInvalidTexture;
    }
    vkBindImageMemory(d.device, texture.image, texture.memory, 0);

    // Промежуточный буфер: текстура лежит в памяти устройства, а туда
    // процессор писать не может. Буфер живёт только на время загрузки —
    // атлас грузится один раз за запуск, экономить здесь нечего.
    const VkDeviceSize bytes = VkDeviceSize(width) * VkDeviceSize(height) * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(d.device, &bufferInfo, nullptr, &staging);

    VkMemoryRequirements stagingReqs{};
    vkGetBufferMemoryRequirements(d.device, staging, &stagingReqs);
    VkMemoryAllocateInfo stagingAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    stagingAlloc.allocationSize = stagingReqs.size;
    stagingAlloc.memoryTypeIndex =
        findMemoryType(d.physical, stagingReqs.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(d.device, &stagingAlloc, nullptr, &stagingMemory);
    vkBindBufferMemory(d.device, staging, stagingMemory, 0);

    void* mapped = nullptr;
    vkMapMemory(d.device, stagingMemory, 0, bytes, 0, &mapped);
    std::memcpy(mapped, pixels, size_t(bytes));
    vkUnmapMemory(d.device, stagingMemory);

    // Одноразовый командный буфер под загрузку.
    VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = d.pool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VkCommandBuffer upload = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(d.device, &cmdInfo, &upload);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(upload, &begin);

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toTransfer);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {uint32_t(width), uint32_t(height), 1};
    vkCmdCopyBufferToImage(upload, staging, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier toShader = toTransfer;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toShader);

    vkEndCommandBuffer(upload);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &upload;
    vkQueueSubmit(d.queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(d.queue);

    vkFreeCommandBuffers(d.device, d.pool, 1, &upload);
    vkDestroyBuffer(d.device, staging, nullptr);
    vkFreeMemory(d.device, stagingMemory, nullptr);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kOffscreenFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(d.device, &viewInfo, nullptr, &texture.view) != VK_SUCCESS) {
        d.fail("не удалось создать вид текстуры");
        return kInvalidTexture;
    }

    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = d.descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &d.samplerLayout;
    if (vkAllocateDescriptorSets(d.device, &setInfo, &texture.set) != VK_SUCCESS) {
        d.fail("не удалось выделить дескриптор текстуры");
        return kInvalidTexture;
    }

    VkDescriptorImageInfo imageDescriptor{};
    imageDescriptor.sampler = d.sampler;
    imageDescriptor.imageView = texture.view;
    imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = texture.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageDescriptor;
    vkUpdateDescriptorSets(d.device, 1, &write, 0, nullptr);

    d.textures.push_back(texture);
    // Ноль зарезервирован под «нет текстуры», поэтому дескриптор — индекс плюс один.
    return TextureHandle(d.textures.size());
}

// ---------------------------------------------------------------------------
// Конвейеры
// ---------------------------------------------------------------------------

bool Device::Impl::buildPipeline(const std::vector<uint8_t>& vertexSpirv,
                                 const std::vector<uint8_t>& fragmentSpirv, bool textured,
                                 VkPrimitiveTopology topology,
                                 const VkVertexInputBindingDescription* bindings,
                                 uint32_t bindingCount,
                                 const VkVertexInputAttributeDescription* attributes,
                                 uint32_t attributeCount, VkPipelineLayout& outLayout,
                                 VkPipeline& outPipeline, uint32_t pushBytes, bool depth,
                                 bool cull, bool blendEnabled, bool depthWrite,
                                 bool additive) {
    if (vertexSpirv.empty() || fragmentSpirv.empty()) {
        return fail("пустой SPIR-V: шейдеры не собраны или не найдены");
    }
    if (textured && !createSamplerResources()) return false;

    auto makeModule = [&](const std::vector<uint8_t>& code, VkShaderModule& out) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size();
        info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        return vkCreateShaderModule(device, &info, nullptr, &out) == VK_SUCCESS;
    };

    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    if (!makeModule(vertexSpirv, vs) || !makeModule(fragmentSpirv, fs)) {
        if (vs) vkDestroyShaderModule(device, vs, nullptr);
        if (fs) vkDestroyShaderModule(device, fs, nullptr);
        return fail("не удалось создать шейдерный модуль");
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

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = bindingCount;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = attributeCount;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;

    VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {uint32_t(width), uint32_t(height)}};
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // Отсечение задних граней включается только для сеток. У спрайтов
    // и линий его быть не должно: квадрат строится в шейдере и может
    // оказаться намотан в любую сторону.
    raster.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Обычное смешивание по альфе: спрайты кораблей и звёзд прозрачны
    // по краям, и без него на карте были бы чёрные прямоугольники.
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = blendEnabled ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    // Сложение вместо замещения: свет складывается, а не закрашивает.
    // Наложенные друг на друга оболочки короны обязаны становиться ярче,
    // а не оставаться такими же, — иначе никакого свечения не выходит.
    blendAttachment.dstColorBlendFactor =
        additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    // Push-константы видны обеим стадиям: сеточному фрагментному шейдеру
    // нужны и положение камеры, и свет, а держать их вторым буфером ради
    // тридцати двух байт — это дескрипторы и синхронизация на пустом месте.
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = pushBytes;

    // Глубина: сетки пишут и проверяют, плоские конвейеры не делают ни того,
    // ни другого. Проход один на всех, поэтому вложение глубины в нём есть
    // всегда, и выключается оно здесь, в состоянии конвейера.
    VkPipelineDepthStencilStateCreateInfo depthState{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthState.depthTestEnable = depth ? VK_TRUE : VK_FALSE;
    depthState.depthWriteEnable = (depth && depthWrite) ? VK_TRUE : VK_FALSE;
    depthState.depthCompareOp = VK_COMPARE_OP_LESS;
    depthState.minDepthBounds = 0.0f;
    depthState.maxDepthBounds = 1.0f;

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (textured) {
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &samplerLayout;
    }
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &outLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
        return fail("не удалось создать раскладку конвейера");
    }

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthState;
    info.pColorBlendState = &blend;
    info.layout = outLayout;
    info.renderPass = renderPass;
    info.subpass = 0;

    const VkResult result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &outPipeline);

    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);

    if (result != VK_SUCCESS) return fail("не удалось создать графический конвейер", result);
    return true;
}

bool Device::createSpritePipeline(const std::vector<uint8_t>& vertexSpirv,
                                  const std::vector<uint8_t>& fragmentSpirv) {
    Impl& d = *impl_;

    // Одна привязка с шагом ПО ЭКЗЕМПЛЯРУ: квадрат строится в шейдере,
    // а из буфера читается только то, чем спрайты отличаются друг от друга.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(SpriteInstance);
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SpriteInstance, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SpriteInstance, halfWidth)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstance, u0)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstance, r)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(SpriteInstance, rotationTurns)},
    };

    return d.buildPipeline(vertexSpirv, fragmentSpirv, /*textured=*/true,
                           VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, &binding, 1, attributes,
                           uint32_t(sizeof(attributes) / sizeof(attributes[0])),
                           d.spriteLayout, d.spritePipeline);
}

bool Device::createLinePipeline(const std::vector<uint8_t>& vertexSpirv,
                                const std::vector<uint8_t>& fragmentSpirv) {
    Impl& d = *impl_;

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(LineVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(LineVertex, x)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(LineVertex, r)},
    };

    return d.buildPipeline(vertexSpirv, fragmentSpirv, /*textured=*/false,
                           VK_PRIMITIVE_TOPOLOGY_LINE_LIST, &binding, 1, attributes,
                           uint32_t(sizeof(attributes) / sizeof(attributes[0])), d.lineLayout,
                           d.linePipeline);
}

// ---------------------------------------------------------------------------
// Отрисовка
// ---------------------------------------------------------------------------

void Device::setCamera(const Camera& camera) { impl_->camera = camera; }

namespace {

CameraPush makePush(const Camera& camera, int width, int height) {
    CameraPush push{};
    push.centerX = camera.centerX;
    push.centerY = camera.centerY;

    const float halfHeight = camera.worldHeight * 0.5f;
    const float aspect = height > 0 ? float(width) / float(height) : 1.0f;

    // Задаём высоту, а не ширину: иначе на широком мониторе игрок видел бы
    // больше карты, чем на обычном, — то есть получал преимущество за форму
    // монитора. Для MMO «без pay to win» это ровно тот же класс проблемы.
    push.scaleX = halfHeight > 0.0f ? 1.0f / (halfHeight * aspect) : 0.0f;
    // Знак задаёт направление оси Y и служит шейдеру ответом на вопрос,
    // какой край текстуры окажется вверху экрана.
    const float sign = camera.yDown ? 1.0f : -1.0f;
    push.scaleY = halfHeight > 0.0f ? sign / halfHeight : 0.0f;
    return push;
}

}  // namespace

void Device::drawSprites(const SpriteInstance* instances, size_t count, TextureHandle atlas) {
    Impl& d = *impl_;
    if (count == 0 || instances == nullptr) return;
    if (!d.frameOpen || d.spritePipeline == VK_NULL_HANDLE) return;
    if (atlas == kInvalidTexture || atlas > d.textures.size()) return;

    VkDeviceSize bytes = VkDeviceSize(count) * sizeof(SpriteInstance);
    if (!d.ensureBuffer(d.spriteBuffer, d.spriteUsed + bytes,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        // Места не хватило, а вырасти внутри кадра нельзя. Рисуем СКОЛЬКО
        // ВЛЕЗЛО, а не бросаем вызов целиком: пропавшая половина карты
        // заметна куда меньше, чем пропавшая карта. Добавку выдадут
        // на следующем кадре, и он будет уже полным.
        if (d.spriteBuffer.capacity <= d.spriteUsed) return;
        count = size_t((d.spriteBuffer.capacity - d.spriteUsed) / sizeof(SpriteInstance));
        if (count == 0) return;
        bytes = VkDeviceSize(count) * sizeof(SpriteInstance);
    }

    std::memcpy(static_cast<uint8_t*>(d.spriteBuffer.mapped) + d.spriteUsed, instances,
                size_t(bytes));

    vkCmdBindPipeline(d.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.spritePipeline);

    const VkDescriptorSet set = d.textures[atlas - 1].set;
    vkCmdBindDescriptorSets(d.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.spriteLayout, 0, 1, &set,
                            0, nullptr);

    // Стадии обязаны совпадать с диапазоном, объявленным в раскладке
    // конвейера, — иначе это нарушение спецификации, а не мелочь:
    // раскладка объявлена как вершинная И фрагментная, значит и класть
    // надо в обе. Проверочные слои ловят это первым же кадром.
    const CameraPush push = makePush(d.camera, d.width, d.height);
    vkCmdPushConstants(d.cmd, d.spriteLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    const VkDeviceSize offset = d.spriteUsed;
    vkCmdBindVertexBuffers(d.cmd, 0, 1, &d.spriteBuffer.buffer, &offset);
    // Шесть вершин квадрата, count экземпляров — вся карта одним вызовом.
    vkCmdDraw(d.cmd, 6, uint32_t(count), 0, 0);

    d.spriteUsed += bytes;
}

void Device::drawLines(const LineVertex* vertices, size_t count) {
    Impl& d = *impl_;
    if (count < 2 || vertices == nullptr) return;
    if (!d.frameOpen || d.linePipeline == VK_NULL_HANDLE) return;

    // Вершины идут парами: нечётный хвост нарисовал бы отрезок в мусор.
    count -= count % 2;

    VkDeviceSize bytes = VkDeviceSize(count) * sizeof(LineVertex);
    if (!d.ensureBuffer(d.lineBuffer, d.lineUsed + bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        // Как и со спрайтами — рисуем сколько влезло. Вершины идут парами,
        // поэтому нечётный хвост отсекаем ещё раз.
        if (d.lineBuffer.capacity <= d.lineUsed) return;
        count = size_t((d.lineBuffer.capacity - d.lineUsed) / sizeof(LineVertex));
        count -= count % 2;
        if (count < 2) return;
        bytes = VkDeviceSize(count) * sizeof(LineVertex);
    }

    std::memcpy(static_cast<uint8_t*>(d.lineBuffer.mapped) + d.lineUsed, vertices,
                size_t(bytes));

    vkCmdBindPipeline(d.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.linePipeline);

    const CameraPush push = makePush(d.camera, d.width, d.height);
    vkCmdPushConstants(d.cmd, d.lineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    const VkDeviceSize offset = d.lineUsed;
    vkCmdBindVertexBuffers(d.cmd, 0, 1, &d.lineBuffer.buffer, &offset);
    vkCmdDraw(d.cmd, uint32_t(count), 1, 0, 0);

    d.lineUsed += bytes;
}

// ---------------------------------------------------------------------------

void Device::Impl::destroyDrawing() {
    if (device == VK_NULL_HANDLE) return;

    destroyBuffer(spriteBuffer);
    destroyBuffer(lineBuffer);
    destroyMeshes();

    for (TextureVk& texture : textures) {
        if (texture.view) vkDestroyImageView(device, texture.view, nullptr);
        if (texture.image) vkDestroyImage(device, texture.image, nullptr);
        if (texture.memory) vkFreeMemory(device, texture.memory, nullptr);
    }
    textures.clear();

    if (spritePipeline) vkDestroyPipeline(device, spritePipeline, nullptr);
    if (spriteLayout) vkDestroyPipelineLayout(device, spriteLayout, nullptr);
    if (linePipeline) vkDestroyPipeline(device, linePipeline, nullptr);
    if (lineLayout) vkDestroyPipelineLayout(device, lineLayout, nullptr);
    if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (samplerLayout) vkDestroyDescriptorSetLayout(device, samplerLayout, nullptr);
    if (sampler) vkDestroySampler(device, sampler, nullptr);

    spritePipeline = VK_NULL_HANDLE;
    spriteLayout = VK_NULL_HANDLE;
    linePipeline = VK_NULL_HANDLE;
    lineLayout = VK_NULL_HANDLE;
    descriptorPool = VK_NULL_HANDLE;
    samplerLayout = VK_NULL_HANDLE;
    sampler = VK_NULL_HANDLE;
}

}  // namespace pw::rhi
