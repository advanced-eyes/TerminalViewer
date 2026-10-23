#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanContext.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawList.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanAtlasTexture.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanLoader.h"

#include <QFile>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kApiMajor = 1;
constexpr uint32_t kApiMinor = 3;
constexpr const char* kVertexShaderResource = ":/terminalviewer/shaders/terminal.vert.spv";
constexpr const char* kFragmentShaderResource = ":/terminalviewer/shaders/terminal.frag.spv";
constexpr const char* kShaderEntry = "main";
constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
constexpr uint32_t kMonoBinding = 0;
constexpr uint32_t kColorBinding = 1;
constexpr uint32_t kBindingCount = 2;
constexpr uint32_t kStageCount = 2;
constexpr VkFormat kDefaultColorFormat = VK_FORMAT_B8G8R8A8_UNORM;   // 스왑체인·화면 밖 이미지
constexpr float kChannelMax = 255.0f;

std::atomic<int> s_iValidationMessages{0};

VKAPI_ATTR VkBool32 VKAPI_CALL OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* pData, void*)
{
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))
    {
        ++s_iValidationMessages;
        qWarning("Vulkan validation [%s]: %s", pData && pData->pMessageIdName ? pData->pMessageIdName : "",
                 pData && pData->pMessage ? pData->pMessage : "");
    }
    return VK_FALSE;
}

QString ToQString(const std::string& text)
{
    return QString::fromStdString(text);
}

// vk-bootstrap 의 실패 이유(장치 선택은 장치마다 이유가 따로 있다)
template <typename TResult>
QString DescribeError(const TResult& result)
{
    QString strReason = ToQString(result.error().message());
    if (result.vk_result() != VK_SUCCESS)
        strReason += QStringLiteral(" (VkResult %1)").arg(int(result.vk_result()));
    for (const std::string& strDetail : result.detailed_failure_reasons())
        strReason += QStringLiteral("; ") + ToQString(strDetail);
    return strReason;
}

bool ReadSpirv(const char* pResource, std::vector<uint32_t>& outCode)
{
    QFile file(QString::fromLatin1(pResource));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty() || bytes.size() % int(sizeof(uint32_t)) != 0)
        return false;
    outCode.resize(size_t(bytes.size()) / sizeof(uint32_t));
    std::memcpy(outCode.data(), bytes.constData(), size_t(bytes.size()));
    return true;
}
}

// 정적 라이브러리의 qrc(resources/shaders/TerminalViewerShaders.qrc)는 저절로 붙지 않는다. 셰이더를 처음 읽기 전에 한 번 등록한다.
// Q_INIT_RESOURCE 는 이름 공간 밖(전역 함수)에서 불러야 한다: 이름 공간 안에서 부르면 그 이름 공간의 qInitResources_* 를 찾아 링크가 실패한다.
static void RegisterShaderResources()
{
    static std::once_flag once;
    std::call_once(once, [] { Q_INIT_RESOURCE(TerminalViewerShaders); });
}

VulkanContext::VulkanContext() = default;

std::unique_ptr<VulkanContext> VulkanContext::Create(QString* pError)
{
    std::unique_ptr<VulkanContext> upContext(new VulkanContext());
    QString strError;
    if (!upContext->Initialize(&strError))
    {
        if (pError)
            *pError = strError;
        return nullptr;   // 만든 만큼 소멸자가 지운다
    }
    qInfo("Vulkan: %s via %s%s%s", qPrintable(upContext->GetDeviceName()), qPrintable(upContext->GetLibraryUsed()),
          upContext->IsValidationEnabled() ? " (validation)" : "", upContext->HasPresentScaling() ? " (present scaling)" : "");
    return upContext;
}

bool VulkanContext::Initialize(QString* pError)
{
    const auto funcFail = [pError](const QString& reason) {
        *pError = reason;
        return false;
    };

    bool bValidationRequested = false;
    const PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = LoadVulkanLibrary(&m_strLibrary, &bValidationRequested);
    if (!fpGetInstanceProcAddr)
        return funcFail(m_strLibrary);

    // 인스턴스: Vulkan 1.3. headless 가 아니므로 vk-bootstrap 이 VK_KHR_surface 와 플랫폼 surface
    // (VK_EXT_metal_surface / VK_KHR_win32_surface)를 켜고, 로더가 VK_KHR_portability_enumeration 을 알리면
    // (macOS 의 SDK 로더) 그 확장과 ENUMERATE_PORTABILITY 플래그도 켠다.
    vkb::InstanceBuilder instanceBuilder(fpGetInstanceProcAddr);
    instanceBuilder.set_app_name("Ocean").set_engine_name("Ocean").require_api_version(kApiMajor, kApiMinor, 0);
    const vkb::Result<vkb::SystemInfo> systemResult = vkb::SystemInfo::get_system_info(fpGetInstanceProcAddr);
#if defined(__APPLE__)
    // present scaling(스펙 §8): 크기를 바꾸는 동안 옛 프레임이 늘어나지 않고 위 왼쪽에 붙게 한다. 확장이 없으면 켜지 않는다.
    // VK_EXT_surface_maintenance1 은 VK_KHR_get_surface_capabilities2 가 필요하다.
    const bool bSurfaceMaintenance = systemResult &&
                                     systemResult.value().is_extension_available(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
                                     systemResult.value().is_extension_available(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    if (bSurfaceMaintenance)
        instanceBuilder.enable_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME).enable_extension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#else
    const bool bSurfaceMaintenance = false;
#endif
    // 검증: 로더가 레이어를 알릴 때만 켠다(켜면 반드시 켜진다). 그래서 m_bValidation 은 인스턴스에 실제로 켠 레이어를 따른다.
    // 메시지 콜백은 VK_EXT_debug_utils(레이어가 함께 준다)가 있을 때만 단다(없는데 달면 인스턴스 생성이 실패한다).
    if (bValidationRequested)
    {
        if (systemResult && systemResult.value().is_layer_available(kValidationLayerName))
        {
            instanceBuilder.enable_validation_layers(true);
            if (systemResult.value().debug_utils_available)
                instanceBuilder.set_debug_callback(&OnDebugMessage);
            else
                qWarning("Vulkan: validation layer enabled without VK_EXT_debug_utils; messages are not counted");
            m_bValidation = true;
        }
        else
        {
            qWarning("Vulkan: TERMINALVIEWER_VK_VALIDATION=1 but %s is not available via %s; validation off", kValidationLayerName,
                     qPrintable(m_strLibrary));
        }
    }
    const vkb::Result<vkb::Instance> instanceResult = instanceBuilder.build();
    if (!instanceResult)
        return funcFail(QStringLiteral("Vulkan 인스턴스를 만들지 못했다(%1): %2").arg(m_strLibrary, DescribeError(instanceResult)));
    m_instance = instanceResult.value();
    m_instanceDispatch = m_instance.make_table();

    // 장치: 1.3 + dynamicRendering + synchronization2 + 스왑체인. 통합 GPU 를 먼저 고른다(전력), 없으면 아무 GPU.
    // 화면 밖 그리기는 surface 없이 장치를 만든다(present 지원은 화면을 붙일 때 그 surface 로 확인한다).
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    vkb::PhysicalDeviceSelector selector(m_instance);
    selector.set_minimum_version(kApiMajor, kApiMinor)
        .set_required_features_13(features13)
        .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::integrated)
        .allow_any_gpu_device_type(true)
        .defer_surface_initialization();
    const vkb::Result<vkb::PhysicalDevice> physicalResult = selector.select();
    if (!physicalResult)
        return funcFail(QStringLiteral("Vulkan 1.3 장치(dynamicRendering, synchronization2)를 찾지 못했다: %1").arg(DescribeError(physicalResult)));

    vkb::PhysicalDevice physicalDevice = physicalResult.value();
    if (bSurfaceMaintenance)
    {
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchainMaintenance{};
        swapchainMaintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        swapchainMaintenance.swapchainMaintenance1 = VK_TRUE;
        m_bPresentScaling = physicalDevice.is_extension_present(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) &&
                            physicalDevice.enable_extension_features_if_present(swapchainMaintenance) &&
                            physicalDevice.enable_extension_if_present(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    }

    const vkb::Result<vkb::Device> deviceResult = vkb::DeviceBuilder(physicalDevice).build();
    if (!deviceResult)
        return funcFail(QStringLiteral("Vulkan 장치를 만들지 못했다: %1").arg(DescribeError(deviceResult)));
    m_device = deviceResult.value();
    m_dispatch = m_device.make_table();

    const auto queueResult = m_device.get_queue_and_index(vkb::QueueType::graphics);
    if (!queueResult)
        return funcFail(QStringLiteral("graphics 큐가 없다: %1").arg(DescribeError(queueResult)));
    m_queue = queueResult.value().first;
    m_uQueueFamily = queueResult.value().second;

    VmaVulkanFunctions vmaFunctions{};
    vmaFunctions.vkGetInstanceProcAddr = m_instance.fp_vkGetInstanceProcAddr;
    vmaFunctions.vkGetDeviceProcAddr = m_device.fp_vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = m_device.physical_device.physical_device;
    allocatorInfo.device = m_device.device;
    allocatorInfo.instance = m_instance.instance;
    allocatorInfo.vulkanApiVersion = VK_MAKE_API_VERSION(0, kApiMajor, kApiMinor, 0);
    allocatorInfo.pVulkanFunctions = &vmaFunctions;
    const VkResult allocatorResult = vmaCreateAllocator(&allocatorInfo, &m_allocator);
    if (allocatorResult != VK_SUCCESS)
    {
        m_allocator = nullptr;
        return funcFail(QStringLiteral("VMA 를 만들지 못했다 (VkResult %1)").arg(int(allocatorResult)));
    }

    if (!CreateShaderModules(pError) || !CreateLayouts(pError))
        return false;
    // 셰이더·파이프라인을 못 만들면 GPU 초기화 실패와 같다(스펙 §11): 쓰는 포맷의 파이프라인을 미리 만들어 확인한다.
    if (GetPipeline(kDefaultColorFormat) == VK_NULL_HANDLE)
        return funcFail(QStringLiteral("그리기 파이프라인을 만들지 못했다"));
    return true;
}

bool VulkanContext::CreateShaderModules(QString* pError)
{
    RegisterShaderResources();
    const auto funcCreate = [this](const char* pResource, VkShaderModule& outModule) {
        std::vector<uint32_t> vecCode;
        if (!ReadSpirv(pResource, vecCode))
            return false;
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = vecCode.size() * sizeof(uint32_t);
        info.pCode = vecCode.data();
        if (m_dispatch.createShaderModule(&info, nullptr, &outModule) != VK_SUCCESS)
        {
            outModule = VK_NULL_HANDLE;
            return false;
        }
        return true;
    };
    if (!funcCreate(kVertexShaderResource, m_vertexShader) || !funcCreate(kFragmentShaderResource, m_fragmentShader))
    {
        *pError = QStringLiteral("셰이더 모듈을 만들지 못했다(%1, %2)").arg(QLatin1String(kVertexShaderResource), QLatin1String(kFragmentShaderResource));
        return false;
    }
    return true;
}

bool VulkanContext::CreateLayouts(QString* pError)
{
    // 아틀라스 샘플러: nearest(쿼드가 장치 픽셀에 정렬되고 셰이더는 texelFetch 로 읽는다)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (m_dispatch.createSampler(&samplerInfo, nullptr, &m_sampler) != VK_SUCCESS)
    {
        m_sampler = VK_NULL_HANDLE;
        *pError = QStringLiteral("샘플러를 만들지 못했다");
        return false;
    }

    // 디스크립터 세트 1개: binding 0 = 흑백 아틀라스(R8), binding 1 = 컬러 아틀라스(RGBA8). 샘플러는 고정
    VkDescriptorSetLayoutBinding arrBindings[kBindingCount]{};
    for (uint32_t uBinding : {kMonoBinding, kColorBinding})
    {
        arrBindings[uBinding].binding = uBinding;
        arrBindings[uBinding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        arrBindings[uBinding].descriptorCount = 1;
        arrBindings[uBinding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        arrBindings[uBinding].pImmutableSamplers = &m_sampler;
    }
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = kBindingCount;
    setLayoutInfo.pBindings = arrBindings;
    if (m_dispatch.createDescriptorSetLayout(&setLayoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
    {
        m_descriptorSetLayout = VK_NULL_HANDLE;
        *pError = QStringLiteral("디스크립터 세트 레이아웃을 만들지 못했다");
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(TerminalPushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (m_dispatch.createPipelineLayout(&layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
    {
        m_pipelineLayout = VK_NULL_HANDLE;
        *pError = QStringLiteral("파이프라인 레이아웃을 만들지 못했다");
        return false;
    }
    return true;
}

VkPipeline VulkanContext::CreatePipeline(VkFormat colorFormat)
{
    VkPipelineShaderStageCreateInfo arrStages[kStageCount]{};
    arrStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    arrStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    arrStages[0].module = m_vertexShader;
    arrStages[0].pName = kShaderEntry;
    arrStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    arrStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    arrStages[1].module = m_fragmentShader;
    arrStages[1].pName = kShaderEntry;

    // 정점 버퍼 1개 = DrawQuad 배열(stride 20, 인스턴스마다). 정점 셰이더가 gl_VertexIndex 로 모서리를 만든다.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(DrawQuad);
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    const VkVertexInputAttributeDescription arrAttributes[] = {
        {0, 0, VK_FORMAT_R16G16_SINT, uint32_t(offsetof(DrawQuad, iX))},        // inPos (iX, iY)
        {1, 0, VK_FORMAT_R16G16_UINT, uint32_t(offsetof(DrawQuad, uW))},        // inSize (uW, uH)
        {2, 0, VK_FORMAT_R16G16_UINT, uint32_t(offsetof(DrawQuad, uAtlasX))},   // inAtlas (uAtlasX, uAtlasY)
        {3, 0, VK_FORMAT_R32_UINT, uint32_t(offsetof(DrawQuad, uColor))},       // inColor
        {4, 0, VK_FORMAT_R16G16_SINT, uint32_t(offsetof(DrawQuad, eKind))},     // inKindRow (eKind | uReserved<<8, iRow)
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = uint32_t(std::size(arrAttributes));
    vertexInput.pVertexAttributeDescriptions = arrAttributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    // 인덱스 없이 그리므로 primitive restart 는 아무 일도 하지 않는다. Metal 은 이것을 끌 수 없어
    // 끄면 MoltenVK 가 파이프라인마다 경고(VK_ERROR_FEATURE_NOT_PRESENT)를 내므로 켜 둔다(strip 은 core 에서 허용).
    inputAssembly.primitiveRestartEnable = VK_TRUE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // premultiplied: color·alpha 모두 ONE / ONE_MINUS_SRC_ALPHA
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    const VkDynamicState arrDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = uint32_t(std::size(arrDynamic));
    dynamicState.pDynamicStates = arrDynamic;

    // dynamic rendering: 색 첨부 1개
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = kStageCount;
    pipelineInfo.pStages = arrStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = m_dispatch.createGraphicsPipelines(VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        qWarning("Vulkan: failed to create the terminal pipeline for format %d (VkResult %d)", int(colorFormat), int(result));
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

VulkanContext::~VulkanContext()
{
    if (m_device.device != VK_NULL_HANDLE)
        m_dispatch.deviceWaitIdle();
    // 만든 역순: 아틀라스 텍스처 → 파이프라인 → 레이아웃·샘플러 → 셰이더 → VMA → 장치 → 인스턴스(디버그 메신저 포함)
    m_hashAtlasTextures.clear();
    for (const auto& pairPipeline : m_mapPipelines)
    {
        if (pairPipeline.second != VK_NULL_HANDLE)
            m_dispatch.destroyPipeline(pairPipeline.second, nullptr);
    }
    m_mapPipelines.clear();
    if (m_pipelineLayout != VK_NULL_HANDLE)
        m_dispatch.destroyPipelineLayout(m_pipelineLayout, nullptr);
    if (m_descriptorSetLayout != VK_NULL_HANDLE)
        m_dispatch.destroyDescriptorSetLayout(m_descriptorSetLayout, nullptr);
    if (m_sampler != VK_NULL_HANDLE)
        m_dispatch.destroySampler(m_sampler, nullptr);
    if (m_fragmentShader != VK_NULL_HANDLE)
        m_dispatch.destroyShaderModule(m_fragmentShader, nullptr);
    if (m_vertexShader != VK_NULL_HANDLE)
        m_dispatch.destroyShaderModule(m_vertexShader, nullptr);
    if (m_allocator)
        vmaDestroyAllocator(m_allocator);
    vkb::destroy_device(m_device);
    vkb::destroy_instance(m_instance);
}

VkInstance VulkanContext::GetInstance() const
{
    return m_instance.instance;
}

VkPhysicalDevice VulkanContext::GetPhysicalDevice() const
{
    return m_device.physical_device.physical_device;
}

VkDevice VulkanContext::GetDevice() const
{
    return m_device.device;
}

VkQueue VulkanContext::GetQueue() const
{
    return m_queue;
}

uint32_t VulkanContext::GetQueueFamily() const
{
    return m_uQueueFamily;
}

VmaAllocator VulkanContext::GetAllocator() const
{
    return m_allocator;
}

const vkb::DispatchTable& VulkanContext::GetDispatch() const
{
    return m_dispatch;
}

const vkb::InstanceDispatchTable& VulkanContext::GetInstanceDispatch() const
{
    return m_instanceDispatch;
}

VkPipelineLayout VulkanContext::GetPipelineLayout() const
{
    return m_pipelineLayout;
}

VkDescriptorSetLayout VulkanContext::GetDescriptorSetLayout() const
{
    return m_descriptorSetLayout;
}

VkPipeline VulkanContext::GetPipeline(VkFormat colorFormat)
{
    const auto itFound = m_mapPipelines.find(colorFormat);
    if (itFound != m_mapPipelines.end())
        return itFound->second;
    const VkPipeline pipeline = CreatePipeline(colorFormat);
    if (pipeline != VK_NULL_HANDLE)
        m_mapPipelines.emplace(colorFormat, pipeline);
    return pipeline;
}

VulkanAtlasTexture& VulkanContext::GetAtlasTexture(const GlyphCache* pCache)
{
    std::unique_ptr<VulkanAtlasTexture>& upTexture = m_hashAtlasTextures[pCache];
    if (!upTexture)
        upTexture = std::make_unique<VulkanAtlasTexture>(*this);
    return *upTexture;
}

void VulkanContext::ReleaseAtlasTexture(const GlyphCache* pCache)
{
    const auto itFound = m_hashAtlasTextures.find(pCache);
    if (itFound == m_hashAtlasTextures.end())
        return;
    m_dispatch.queueWaitIdle(m_queue);   // 그 텍스처를 읽는 제출이 끝난 뒤 지운다
    m_hashAtlasTextures.erase(itFound);
}

void VulkanContext::ForgetCommandBuffer(VkCommandBuffer cmd)
{
    for (auto& pairTexture : m_hashAtlasTextures)
        pairTexture.second->ForgetCommandBuffer(cmd);
}

bool VulkanContext::RecordDraw(VkCommandBuffer cmd, VkImageView targetView, VkExtent2D extent, const DrawList& list, const VulkanAtlasTexture& atlas,
                               VkBuffer instanceBuffer)
{
    const VkPipeline pipeline = GetPipeline(kDefaultColorFormat);
    const bool bDraw = !list.vecQuads.empty() && atlas.IsReady() && pipeline != VK_NULL_HANDLE && instanceBuffer != VK_NULL_HANDLE;

    // 지우기 색 = 프레임 기본 배경(불투명). 남는 띠까지 extent 전체를 지운다.
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = targetView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color.float32[0] = float(list.clearColor.r) / kChannelMax;
    colorAttachment.clearValue.color.float32[1] = float(list.clearColor.g) / kChannelMax;
    colorAttachment.clearValue.color.float32[2] = float(list.clearColor.b) / kChannelMax;
    colorAttachment.clearValue.color.float32[3] = 1.0f;
    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = VkRect2D{VkOffset2D{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    m_dispatch.cmdBeginRendering(cmd, &renderingInfo);
    if (bDraw)
    {
        m_dispatch.cmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkViewport viewport{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
        const VkRect2D scissor{VkOffset2D{0, 0}, extent};
        m_dispatch.cmdSetViewport(cmd, 0, 1, &viewport);
        m_dispatch.cmdSetScissor(cmd, 0, 1, &scissor);
        TerminalPushConstants push;
        push.fViewportWidth = float(extent.width);
        push.fViewportHeight = float(extent.height);
        push.fPadding = float(list.iPaddingPx);
        push.fCellHeight = float(list.iCellHeight);
        push.fScrollOffset = float(list.iScrollOffsetPx);
        m_dispatch.cmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        const VkDescriptorSet descriptorSet = atlas.GetDescriptorSet();
        m_dispatch.cmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        const VkDeviceSize vertexOffset = 0;
        m_dispatch.cmdBindVertexBuffers(cmd, 0, 1, &instanceBuffer, &vertexOffset);
        m_dispatch.cmdDraw(cmd, kTerminalQuadVertexCount, uint32_t(list.vecQuads.size()), 0, 0);
    }
    m_dispatch.cmdEndRendering(cmd);
    return bDraw;
}

PFN_vkGetInstanceProcAddr VulkanContext::GetInstanceProcAddr() const
{
    return m_instance.fp_vkGetInstanceProcAddr;
}

bool VulkanContext::HasPresentScaling() const
{
    return m_bPresentScaling;
}

QString VulkanContext::GetDeviceName() const
{
    return QString::fromUtf8(m_device.physical_device.properties.deviceName);
}

QString VulkanContext::GetLibraryUsed() const
{
    return m_strLibrary;
}

bool VulkanContext::IsValidationEnabled() const
{
    return m_bValidation;
}

int VulkanContext::GetValidationMessageCount()
{
    return s_iValidationMessages.load();
}
