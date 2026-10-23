#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>

#include <QSize>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

struct DrawList;
class GlyphCache;
class VulkanContext;

// 순수 함수(테스트용 공개). 요청 크기와 surface 능력 → 스왑체인 크기. 0 이면 nullopt(그리지 않고 멈춘다)
std::optional<VkExtent2D> ChooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& caps, const QSize& requested);
VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& available);   // MAILBOX 가 있으면 그것, 아니면 FIFO
uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& caps, VkPresentModeKHR mode);  // MAILBOX 3, FIFO 2 를 caps 범위로 자름

// 화면 하나의 surface, 스왑체인, 프레임 자원(스펙 §8). 렌더 스레드에서만 만들고 쓰고 지운다. VulkanContext 보다 먼저 지운다.
// 숨은 화면은 렌더 스레드가 아예 Render 를 부르지 않는다(FrameScheduler). 이것은 효율이 아니라 정확성 요구다:
// 숨은 탭에 계속 표시하면 macOS 에서 보이는 화면까지 약 1초씩 멈췄다(스펙 §5, 부록 A S1).
// vkGetRefreshCycleDurationGOOGLE, vkCreateMacOSSurfaceMVK 는 메인 스레드를 기다리므로 부르지 않는다(스펙 §5).
class VulkanView
{
public:
    VulkanView(VulkanContext& context, const TerminalNativeHandle& handle, const QSize& devicePixelSize);  // surface 를 만든다(렌더 스레드)
    ~VulkanView();                 // 이 화면 펜스 대기 → 스왑체인·surface 지움 → handle.funcRelease

    VulkanView(const VulkanView&) = delete;
    VulkanView& operator=(const VulkanView&) = delete;

    bool HasSurface() const;       // surface 를 만들었고 이 장치의 큐로 표시할 수 있다(거짓이면 이 화면은 CPU 로)
    bool IsReady() const;          // 다음 프레임 펜스가 끝났고 크기가 0 이 아님
    // 획득(타임아웃 0) → 아틀라스 올리기 → 그리기 → 제출 → 표시. out 의 bNotReady/bSurfaceLost/bDeviceLost 를 채운다.
    // 아틀라스를 올리지 못해 목록을 그리지 못했으면 획득한 이미지는 지우기만 해 내고 bNotReady(같은 목록으로 다시)로 알린다
    void Render(const DrawList& list, GlyphCache& cache, TerminalRenderOutput& out);
    void Resize(const QSize& devicePixelSize);   // 다음 Render 전에 스왑체인을 다시 만든다(크기가 같으면 다시 만들지 않는다)

public:
    VkExtent2D GetSwapchainExtent() const;       // 스왑체인이 없으면 0×0
    VkPresentModeKHR GetPresentMode() const;
    bool UsesPresentScaling() const;             // 지금 스왑체인에 present scaling 을 주었다
    int GetSwapchainCreateCount() const;         // 진단·테스트용: 스왑체인을 만든 횟수

private:
    static constexpr int kFramesInFlight = 2;

    struct Frame
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;              // 만들 때 신호 상태
        VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
        VkBuffer instanceBuffer = VK_NULL_HANDLE;    // DrawQuad 배열. 계속 매핑된 호스트 메모리
        VmaAllocation instanceAllocation = nullptr;
        void* pInstanceMapped = nullptr;
        VkDeviceSize instanceCapacity = 0;
    };

    struct SwapchainImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSemaphore renderDoneSemaphore = VK_NULL_HANDLE;   // 이미지마다(다시 획득되면 앞선 표시가 끝난 것이다)
    };

    bool CreateSurface();
    bool CreateFrameResources();
    bool CheckSurfaceFormat();
    bool EnsureInstanceBuffer(Frame& frame, VkDeviceSize bytes);
    // 이 화면의 펜스가 모두 끝났으면 옛 스왑체인을 먼저 지운 뒤 만든다(oldSwapchain 없음). 펜스는 기다리지 않고 본다.
    // VK_SUCCESS: 만들었다. VK_TIMEOUT: 아직 끝나지 않은 프레임이 있어 옛 스왑체인을 그대로 두었다.
    // VK_NOT_READY: surface 크기가 0 이라 만들지 않았다. 이 둘은 오류가 아니다(다음에 다시). 그 밖: 실패한 결과
    VkResult RecreateSwapchain();
    bool ChoosePresentScaling(VkPresentModeKHR mode);
    VkResult WaitForFrames(uint64_t timeoutNs);   // 0 이면 기다리지 않고 본다(안 끝났으면 VK_TIMEOUT)
    void DestroySwapchain();
    void DestroyFrameResources();
    // 목록을 다 그렸으면 참. 쿼드가 있는데 아틀라스(또는 파이프라인)가 준비되지 않아 지우기만 기록했으면 거짓
    bool Record(Frame& frame, const SwapchainImage& image, const DrawList& list, GlyphCache& cache);
    void LogError(const char* pWhat, VkResult result);   // 화면마다 한 번만 남긴다
    void ReportError(const char* pWhat, VkResult result, TerminalRenderOutput& out);
    // VK_TIMEOUT·VK_NOT_READY → bNotReady(잠깐 막힘일 수 있다, 재시도 간격으로 다시), SURFACE_LOST·NATIVE_WINDOW_IN_USE → surface 잃음,
    // 그 밖(DEVICE_LOST, 메모리 부족, 알 수 없는 오류) → 장치 잃음
    static void MarkError(VkResult result, TerminalRenderOutput& out);

    VulkanContext& m_context;
    TerminalNativeHandle m_handle;
    QSize m_sizeRequested;
    VkSurfaceKHR m_surface;
    VkCommandPool m_commandPool;
    std::array<Frame, kFramesInFlight> m_arrFrames;
    int m_iFrame;
    bool m_bFramesReady;

    VkSwapchainKHR m_swapchain;
    VkExtent2D m_swapchainExtent;
    std::vector<SwapchainImage> m_vecImages;
    VkPresentModeKHR m_presentMode;
    bool m_bNeedRecreate;
    bool m_bPresentScaling;
    int m_iSwapchainCreateCount;
    bool m_bErrorLogged;
};
