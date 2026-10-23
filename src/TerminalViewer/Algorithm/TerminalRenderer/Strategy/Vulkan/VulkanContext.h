#pragma once

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <QString>

#include <map>
#include <memory>
#include <unordered_map>

struct DrawList;
class GlyphCache;
class VulkanAtlasTexture;

// terminal.vert 의 push constant 블록(정점 단계, 20바이트)
struct TerminalPushConstants
{
    float fViewportWidth = 0.0f;    // 장치 픽셀
    float fViewportHeight = 0.0f;
    float fPadding = 0.0f;
    float fCellHeight = 0.0f;
    float fScrollOffset = 0.0f;     // 정수 장치 픽셀
};
static_assert(sizeof(TerminalPushConstants) == 20);

constexpr uint32_t kTerminalQuadVertexCount = 4;   // 쿼드 하나 = triangle strip 정점 4개, 쿼드마다 인스턴스 하나

// 앱에 하나인 Vulkan 인스턴스·장치·파이프라인. 렌더 스레드에서만 만들고 쓰고 지운다(테스트는 직접 만든다).
// 앱 시작 때 만들지 않는다: GPU Strategy 를 처음 고를 때 만든다.
class VulkanContext
{
public:
    static std::unique_ptr<VulkanContext> Create(QString* pError);   // 실패 이유를 pError 에
    ~VulkanContext();                     // vkDeviceWaitIdle 뒤 만든 역순으로 지움

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

public:
    VkPipeline GetPipeline(VkFormat colorFormat);          // 포맷마다 처음 부를 때 만든다. 실패하면 VK_NULL_HANDLE
    // GlyphCache 하나(배율 하나)에 맞는 아틀라스 텍스처 두 장 + 디스크립터 세트
    VulkanAtlasTexture& GetAtlasTexture(const GlyphCache* pCache);
    void ReleaseAtlasTexture(const GlyphCache* pCache);    // 큐를 기다린 뒤 지운다. GlyphCache 를 버릴 때 부른다
    // 명령 버퍼를 지우기 전에 부른다(그 제출이 끝난 뒤): 아틀라스 텍스처들이 그 명령 버퍼용으로 잡아 둔 스테이징 버퍼를 놓는다
    void ForgetCommandBuffer(VkCommandBuffer cmd);
    // B8G8R8A8_UNORM 색 첨부 하나(COLOR_ATTACHMENT_OPTIMAL)에 그리기 목록을 기록한다: dynamic rendering 으로 extent 전체를
    // list.clearColor 로 지우고, 쿼드가 있고 아틀라스가 준비됐으면 인스턴스 그리기 1회(push constant 화면 크기 = extent).
    // 레이아웃 바꾸기, 아틀라스 올리기, 인스턴스 버퍼 채우기는 부른 쪽이 한다. 화면(VulkanView)과 화면 밖(VulkanOffscreen)이
    // 함께 써서 두 경로가 갈라지지 않게 한다. 그렸으면 참(지우기만 했으면 거짓)
    bool RecordDraw(VkCommandBuffer cmd, VkImageView targetView, VkExtent2D extent, const DrawList& list, const VulkanAtlasTexture& atlas,
                    VkBuffer instanceBuffer);

public:
    VkInstance GetInstance() const;
    VkPhysicalDevice GetPhysicalDevice() const;
    VkDevice GetDevice() const;
    VkQueue GetQueue() const;
    uint32_t GetQueueFamily() const;
    VmaAllocator GetAllocator() const;
    const vkb::DispatchTable& GetDispatch() const;
    const vkb::InstanceDispatchTable& GetInstanceDispatch() const;
    VkPipelineLayout GetPipelineLayout() const;
    VkDescriptorSetLayout GetDescriptorSetLayout() const;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr() const;   // 플랫폼 surface 함수를 찾을 때
    // macOS: VK_EXT_swapchain_maintenance1(+ 인스턴스 VK_EXT_surface_maintenance1)을 켰다 → 스왑체인에 present scaling 을 줄 수 있다
    bool HasPresentScaling() const;
    QString GetDeviceName() const;
    QString GetLibraryUsed() const;
    bool IsValidationEnabled() const;   // 인스턴스에 검증 레이어를 실제로 켰다
    // 검증 레이어가 낸 경고·오류 수(프로세스 전체, TERMINALVIEWER_VK_VALIDATION=1 일 때만 늘어난다)
    static int GetValidationMessageCount();

private:
    VulkanContext();
    bool Initialize(QString* pError);
    bool CreateShaderModules(QString* pError);
    bool CreateLayouts(QString* pError);
    VkPipeline CreatePipeline(VkFormat colorFormat);

    QString m_strLibrary;
    bool m_bValidation = false;
    bool m_bPresentScaling = false;
    vkb::Instance m_instance;
    vkb::InstanceDispatchTable m_instanceDispatch;
    vkb::Device m_device;
    vkb::DispatchTable m_dispatch;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_uQueueFamily = 0;
    VmaAllocator m_allocator = nullptr;
    VkShaderModule m_vertexShader = VK_NULL_HANDLE;
    VkShaderModule m_fragmentShader = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::map<VkFormat, VkPipeline> m_mapPipelines;
    std::unordered_map<const GlyphCache*, std::unique_ptr<VulkanAtlasTexture>> m_hashAtlasTextures;
};
