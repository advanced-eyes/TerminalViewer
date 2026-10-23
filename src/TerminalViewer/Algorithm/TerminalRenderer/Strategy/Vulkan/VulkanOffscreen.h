#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>

#include <QImage>
#include <QSize>

struct DrawList;
class GlyphCache;
class VulkanContext;

// 화면 밖 이미지에 그려 읽어 온다(GPU–CPU 비교 테스트, 확인 도구의 캡처). Render 는 끝날 때까지 기다린다.
// VulkanContext 보다 먼저 지운다.
class VulkanOffscreen
{
public:
    explicit VulkanOffscreen(VulkanContext& context);
    ~VulkanOffscreen();

    VulkanOffscreen(const VulkanOffscreen&) = delete;
    VulkanOffscreen& operator=(const VulkanOffscreen&) = delete;

    // 같은 파이프라인(B8G8R8A8_UNORM)으로 그려 읽어 온다. 결과는 Format_ARGB32_Premultiplied
    // 실패하거나 크기가 비면 null 이미지
    QImage Render(const DrawList& list, GlyphCache& cache, const QSize& devicePixelSize);

private:
    bool EnsureCommands();
    bool EnsureTarget(const QSize& size);
    bool EnsureInstanceBuffer(VkDeviceSize bytes);
    void DestroyTarget();
    void DestroyInstanceBuffer();

    VulkanContext& m_context;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    bool m_bPending = false;   // 제출했지만 펜스 대기가 시간 안에 끝나지 않았다

    QSize m_targetSize;
    VkImage m_targetImage = VK_NULL_HANDLE;
    VmaAllocation m_targetAllocation = nullptr;
    VkImageView m_targetView = VK_NULL_HANDLE;
    VkBuffer m_readbackBuffer = VK_NULL_HANDLE;
    VmaAllocation m_readbackAllocation = nullptr;
    void* m_pReadbackMapped = nullptr;

    VkBuffer m_instanceBuffer = VK_NULL_HANDLE;
    VmaAllocation m_instanceAllocation = nullptr;
    void* m_pInstanceMapped = nullptr;
    VkDeviceSize m_instanceCapacity = 0;
};
