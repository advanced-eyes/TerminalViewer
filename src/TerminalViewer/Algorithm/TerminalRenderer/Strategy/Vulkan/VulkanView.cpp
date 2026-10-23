#include <QtGlobal>

// 플랫폼 surface 확장의 구조체·함수 형식. 함수는 vkGetInstanceProcAddr 로 찾는다
// (vk-bootstrap 의 함수 표는 이 정의 없이 만든 다른 파일과 같은 배치를 쓰므로 표의 플랫폼 항목은 쓰지 않는다).
#if defined(Q_OS_MACOS)
#define VK_USE_PLATFORM_METAL_EXT
#elif defined(Q_OS_WIN)
#define VK_USE_PLATFORM_WIN32_KHR
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanView.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawList.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanAtlasTexture.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanContext.h"

#if defined(Q_OS_MACOS)
#include <vulkan/vulkan_metal.h>
#elif defined(Q_OS_WIN)
#include <vulkan/vulkan_win32.h>
#endif

#include <algorithm>
#include <cstring>
#include <limits>

namespace
{
constexpr VkFormat kSwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkColorSpaceKHR kSwapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
constexpr uint32_t kUndefinedExtent = std::numeric_limits<uint32_t>::max();   // currentExtent 를 surface 가 정하지 않음
constexpr uint32_t kMailboxImageCount = 3;
constexpr uint32_t kFifoImageCount = 2;
constexpr uint64_t kNoWait = 0;
constexpr uint64_t kTeardownWaitNs = 1'000'000'000;    // 지울 때 펜스를 기다리는 한도. 뒤의 vkQueueWaitIdle 이 남은 일을 끝까지 기다린다
constexpr VkDeviceSize kMinInstanceBytes = 256 * sizeof(DrawQuad);
}

std::optional<VkExtent2D> ChooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& caps, const QSize& requested)
{
    if (requested.width() <= 0 || requested.height() <= 0 || caps.maxImageExtent.width == 0 || caps.maxImageExtent.height == 0)
        return std::nullopt;
    VkExtent2D extent = caps.currentExtent;
    if (caps.currentExtent.width == kUndefinedExtent || caps.currentExtent.height == kUndefinedExtent)
    {
        extent.width = std::clamp(uint32_t(requested.width()), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(uint32_t(requested.height()), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
        return std::nullopt;
    return extent;
}

VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& available)
{
    // MoltenVK 에는 MAILBOX 가 없어 macOS 는 FIFO 다(부록 A S2). FIFO 는 언제나 있다(Vulkan 이 보장).
    return std::find(available.begin(), available.end(), VK_PRESENT_MODE_MAILBOX_KHR) != available.end() ? VK_PRESENT_MODE_MAILBOX_KHR
                                                                                                          : VK_PRESENT_MODE_FIFO_KHR;
}

uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& caps, VkPresentModeKHR mode)
{
    uint32_t uCount = std::max(mode == VK_PRESENT_MODE_MAILBOX_KHR ? kMailboxImageCount : kFifoImageCount, caps.minImageCount);
    if (caps.maxImageCount > 0)   // 0 = 위 한도 없음
        uCount = std::min(uCount, caps.maxImageCount);
    return uCount;
}

VulkanView::VulkanView(VulkanContext& context, const TerminalNativeHandle& handle, const QSize& devicePixelSize)
    : m_context(context)
    , m_handle(handle)
    , m_sizeRequested(devicePixelSize)
    , m_surface(VK_NULL_HANDLE)
    , m_commandPool(VK_NULL_HANDLE)
    , m_arrFrames()
    , m_iFrame(0)
    , m_bFramesReady(false)
    , m_swapchain(VK_NULL_HANDLE)
    , m_swapchainExtent{0, 0}
    , m_vecImages()
    , m_presentMode(VK_PRESENT_MODE_FIFO_KHR)
    , m_bNeedRecreate(true)
    , m_bPresentScaling(false)
    , m_iSwapchainCreateCount(0)
    , m_bErrorLogged(false)
{
    if (!CreateSurface())
        return;
    // 장치는 surface 없이 만들었으므로 surface 마다 이 큐 패밀리로 표시할 수 있는지 본다(못 하면 이 화면은 CPU).
    VkBool32 bSupported = VK_FALSE;
    const VkResult supportResult = m_context.GetInstanceDispatch().getPhysicalDeviceSurfaceSupportKHR(
        m_context.GetPhysicalDevice(), m_context.GetQueueFamily(), m_surface, &bSupported);
    if (supportResult != VK_SUCCESS || !bSupported)
    {
        qWarning("Vulkan: the terminal queue family cannot present to this surface (VkResult %d)", int(supportResult));
        m_context.GetInstanceDispatch().destroySurfaceKHR(m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
        return;
    }
    if (!CheckSurfaceFormat() || !CreateFrameResources())
    {
        DestroyFrameResources();
        m_context.GetInstanceDispatch().destroySurfaceKHR(m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
        return;
    }

    uint32_t uModeCount = 0;
    m_context.GetInstanceDispatch().getPhysicalDeviceSurfacePresentModesKHR(m_context.GetPhysicalDevice(), m_surface, &uModeCount, nullptr);
    std::vector<VkPresentModeKHR> vecModes(uModeCount);
    m_context.GetInstanceDispatch().getPhysicalDeviceSurfacePresentModesKHR(m_context.GetPhysicalDevice(), m_surface, &uModeCount, vecModes.data());
    vecModes.resize(uModeCount);
    m_presentMode = ChoosePresentMode(vecModes);
}

VulkanView::~VulkanView()
{
    if (m_bFramesReady)
    {
        // 마지막 표시의 세마포어 대기까지 끝나게 큐를 기다린 뒤 지운다(떼기는 드물다). 실패해도(장치 잃음) 지운다.
        WaitForFrames(kTeardownWaitNs);
        m_context.GetDispatch().queueWaitIdle(m_context.GetQueue());
    }
    DestroySwapchain();
    DestroyFrameResources();
    if (m_surface != VK_NULL_HANDLE)
        m_context.GetInstanceDispatch().destroySurfaceKHR(m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;
    // surface 를 지운 뒤에 핸들을 놓는다(macOS: 우리가 retain 한 CAMetalLayer). 이 화면의 핸들을 놓는 곳은 여기 하나다.
    if (m_handle.funcRelease)
        m_handle.funcRelease(m_handle.pHandle);
}

bool VulkanView::CreateSurface()
{
    if (!m_handle.pHandle)
        return false;
    const VkInstance instance = m_context.GetInstance();
    const PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = m_context.GetInstanceProcAddr();
#if defined(Q_OS_MACOS)
    // CAMetalLayer 로 만든다. NSView 로 만드는 vkCreateMacOSSurfaceMVK 는 메인 스레드를 기다리므로 쓰지 않는다(스펙 §5).
    const auto fpCreate = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(fpGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
    if (!fpCreate)
    {
        qWarning("Vulkan: vkCreateMetalSurfaceEXT is not available");
        return false;
    }
    VkMetalSurfaceCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    info.pLayer = static_cast<const CAMetalLayer*>(m_handle.pHandle);
    const VkResult result = fpCreate(instance, &info, nullptr, &m_surface);
#elif defined(Q_OS_WIN)
    const auto fpCreate = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(fpGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (!fpCreate)
    {
        qWarning("Vulkan: vkCreateWin32SurfaceKHR is not available");
        return false;
    }
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = static_cast<HWND>(m_handle.pHandle);
    const VkResult result = fpCreate(instance, &info, nullptr, &m_surface);
#else
    Q_UNUSED(instance)
    Q_UNUSED(fpGetInstanceProcAddr)
    const VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;   // 이 플랫폼은 GPU 로 화면에 그리지 않는다(범위 밖)
#endif
    if (result != VK_SUCCESS)
    {
        qWarning("Vulkan: failed to create the terminal surface (VkResult %d)", int(result));
        m_surface = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool VulkanView::CheckSurfaceFormat()
{
    uint32_t uCount = 0;
    const vkb::InstanceDispatchTable& instanceDispatch = m_context.GetInstanceDispatch();
    instanceDispatch.getPhysicalDeviceSurfaceFormatsKHR(m_context.GetPhysicalDevice(), m_surface, &uCount, nullptr);
    std::vector<VkSurfaceFormatKHR> vecFormats(uCount);
    instanceDispatch.getPhysicalDeviceSurfaceFormatsKHR(m_context.GetPhysicalDevice(), m_surface, &uCount, vecFormats.data());
    vecFormats.resize(uCount);
    for (const VkSurfaceFormatKHR& format : vecFormats)
    {
        if ((format.format == kSwapchainFormat && format.colorSpace == kSwapchainColorSpace) || format.format == VK_FORMAT_UNDEFINED)
            return true;
    }
    qWarning("Vulkan: the surface does not support B8G8R8A8_UNORM / SRGB_NONLINEAR");
    return false;
}

bool VulkanView::CreateFrameResources()
{
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_context.GetQueueFamily();
    if (dispatch.createCommandPool(&poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
    {
        m_commandPool = VK_NULL_HANDLE;
        return false;
    }
    for (Frame& frame : m_arrFrames)
    {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = m_commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        if (dispatch.allocateCommandBuffers(&allocateInfo, &frame.cmd) != VK_SUCCESS)
        {
            frame.cmd = VK_NULL_HANDLE;
            return false;
        }
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // 첫 프레임은 기다리지 않는다
        if (dispatch.createFence(&fenceInfo, nullptr, &frame.fence) != VK_SUCCESS)
        {
            frame.fence = VK_NULL_HANDLE;
            return false;
        }
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (dispatch.createSemaphore(&semaphoreInfo, nullptr, &frame.acquireSemaphore) != VK_SUCCESS)
        {
            frame.acquireSemaphore = VK_NULL_HANDLE;
            return false;
        }
    }
    m_bFramesReady = true;
    return true;
}

void VulkanView::DestroyFrameResources()
{
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();
    for (Frame& frame : m_arrFrames)
    {
        if (frame.cmd != VK_NULL_HANDLE)
            m_context.ForgetCommandBuffer(frame.cmd);   // 아틀라스 텍스처가 이 명령 버퍼용으로 잡은 스테이징 버퍼
        if (frame.instanceBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_context.GetAllocator(), frame.instanceBuffer, frame.instanceAllocation);
        if (frame.acquireSemaphore != VK_NULL_HANDLE)
            dispatch.destroySemaphore(frame.acquireSemaphore, nullptr);
        if (frame.fence != VK_NULL_HANDLE)
            dispatch.destroyFence(frame.fence, nullptr);
        frame = Frame();
    }
    if (m_commandPool != VK_NULL_HANDLE)
        dispatch.destroyCommandPool(m_commandPool, nullptr);   // 명령 버퍼도 함께 풀린다
    m_commandPool = VK_NULL_HANDLE;
    m_bFramesReady = false;
}

bool VulkanView::EnsureInstanceBuffer(Frame& frame, VkDeviceSize bytes)
{
    if (frame.instanceBuffer != VK_NULL_HANDLE && frame.instanceCapacity >= bytes)
        return true;
    // 이 프레임의 이전 제출은 끝났다(펜스 확인 뒤): 옛 버퍼를 지워도 된다.
    const VkDeviceSize newCapacity = std::max({bytes, frame.instanceCapacity * 2, kMinInstanceBytes});   // 모자라면 2배로
    if (frame.instanceBuffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(m_context.GetAllocator(), frame.instanceBuffer, frame.instanceAllocation);
    frame.instanceBuffer = VK_NULL_HANDLE;
    frame.instanceAllocation = nullptr;
    frame.pInstanceMapped = nullptr;
    frame.instanceCapacity = 0;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = newCapacity;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocated{};
    if (vmaCreateBuffer(m_context.GetAllocator(), &bufferInfo, &allocationInfo, &frame.instanceBuffer, &frame.instanceAllocation, &allocated) != VK_SUCCESS)
    {
        frame.instanceBuffer = VK_NULL_HANDLE;
        frame.instanceAllocation = nullptr;
        return false;
    }
    frame.pInstanceMapped = allocated.pMappedData;
    frame.instanceCapacity = newCapacity;
    return true;
}

VkResult VulkanView::WaitForFrames(uint64_t timeoutNs)
{
    std::vector<VkFence> vecFences;
    for (const Frame& frame : m_arrFrames)
    {
        if (frame.fence != VK_NULL_HANDLE)
            vecFences.push_back(frame.fence);
    }
    if (vecFences.empty())
        return VK_SUCCESS;
    return m_context.GetDispatch().waitForFences(uint32_t(vecFences.size()), vecFences.data(), VK_TRUE, timeoutNs);
}

void VulkanView::DestroySwapchain()
{
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();
    for (SwapchainImage& image : m_vecImages)
    {
        if (image.view != VK_NULL_HANDLE)
            dispatch.destroyImageView(image.view, nullptr);
        if (image.renderDoneSemaphore != VK_NULL_HANDLE)
            dispatch.destroySemaphore(image.renderDoneSemaphore, nullptr);
    }
    m_vecImages.clear();
    if (m_swapchain != VK_NULL_HANDLE)
        dispatch.destroySwapchainKHR(m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
    m_swapchainExtent = VkExtent2D{0, 0};
    m_bPresentScaling = false;
}

bool VulkanView::ChoosePresentScaling(VkPresentModeKHR mode)
{
#if defined(Q_OS_MACOS)
    // 위 왼쪽에 붙임: gravity X = MIN, Y = MAX(Qt 창 안에서 Y = MAX 가 위였다, 부록 A S1 (e)). 지원할 때만 준다.
    if (!m_context.HasPresentScaling())
        return false;
    VkSurfacePresentModeEXT presentMode{};
    presentMode.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT;
    presentMode.presentMode = mode;
    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surfaceInfo.pNext = &presentMode;
    surfaceInfo.surface = m_surface;
    VkSurfacePresentScalingCapabilitiesEXT scaling{};
    scaling.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT;
    VkSurfaceCapabilities2KHR caps{};
    caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    caps.pNext = &scaling;
    if (m_context.GetInstanceDispatch().getPhysicalDeviceSurfaceCapabilities2KHR(m_context.GetPhysicalDevice(), &surfaceInfo, &caps) != VK_SUCCESS)
        return false;
    return (scaling.supportedPresentScaling & VK_PRESENT_SCALING_ONE_TO_ONE_BIT_EXT) &&
           (scaling.supportedPresentGravityX & VK_PRESENT_GRAVITY_MIN_BIT_EXT) && (scaling.supportedPresentGravityY & VK_PRESENT_GRAVITY_MAX_BIT_EXT);
#else
    Q_UNUSED(mode)
    return false;
#endif
}

VkResult VulkanView::RecreateSwapchain()
{
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();
    // 이 화면의 펜스가 모두 끝난 뒤 옛 스왑체인을 먼저 지운다. 그리고 oldSwapchain 없이 새로 만든다:
    // MoltenVK 1.4.0 은 같은 크기로 oldSwapchain 을 넘기면 레이어가 1×1 이 되어 화면이 빈다(스펙 §8, 부록 A S1 (c)).
    // 펜스는 기다리지 않고 본다: 아직이면 옛 스왑체인을 그대로 두고 VK_TIMEOUT(Render 가 bNotReady 로, 재시도 간격 뒤 다시).
    // MoltenVK 는 제출·nextDrawable 에서 ~1초 멈춘 적이 있으므로(부록 A S1) 오래 걸려도 장치 잃음으로 보지 않고 렌더 스레드도 막지 않는다.
    // 실패하면 m_bNeedRecreate 를 그대로 두고 결과를 돌려준다(Render 가 surface 잃음·장치 잃음으로 알린다).
    if (m_swapchain != VK_NULL_HANDLE)
    {
        VkResult waitResult = WaitForFrames(kNoWait);
        if (waitResult == VK_TIMEOUT)
            return VK_TIMEOUT;   // 오류가 아니다(로그 없음)
        if (waitResult == VK_SUCCESS)
            waitResult = dispatch.queueWaitIdle(m_context.GetQueue());   // 옛 이미지의 표시 세마포어 대기까지 끝난 뒤 지운다
        if (waitResult != VK_SUCCESS)
        {
            LogError("failed to wait for the terminal frames before recreating the swapchain", waitResult);
            return waitResult;
        }
        DestroySwapchain();
    }

    VkSurfaceCapabilitiesKHR caps{};
    const VkResult capsResult = m_context.GetInstanceDispatch().getPhysicalDeviceSurfaceCapabilitiesKHR(m_context.GetPhysicalDevice(), m_surface, &caps);
    if (capsResult != VK_SUCCESS)
    {
        LogError("failed to query the terminal surface", capsResult);
        return capsResult;
    }
    const std::optional<VkExtent2D> optExtent = ChooseSwapchainExtent(caps, m_sizeRequested);
    if (!optExtent)
        return VK_NOT_READY;   // 크기가 0 이다(최소화 등): 오류가 아니다. 그리지 않고 다음에 다시 본다

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = m_surface;
    info.minImageCount = ChooseImageCount(caps, m_presentMode);
    info.imageFormat = kSwapchainFormat;
    info.imageColorSpace = kSwapchainColorSpace;
    info.imageExtent = *optExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                               : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = m_presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    VkSwapchainPresentScalingCreateInfoEXT scalingInfo{};
    const bool bScaling = ChoosePresentScaling(m_presentMode);
    if (bScaling)
    {
        scalingInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT;
        scalingInfo.scalingBehavior = VK_PRESENT_SCALING_ONE_TO_ONE_BIT_EXT;
        scalingInfo.presentGravityX = VK_PRESENT_GRAVITY_MIN_BIT_EXT;
        scalingInfo.presentGravityY = VK_PRESENT_GRAVITY_MAX_BIT_EXT;
        info.pNext = &scalingInfo;
    }

    const VkResult createResult = dispatch.createSwapchainKHR(&info, nullptr, &m_swapchain);
    if (createResult != VK_SUCCESS)
    {
        m_swapchain = VK_NULL_HANDLE;
        LogError("failed to create the terminal swapchain", createResult);
        return createResult;
    }
    m_swapchainExtent = *optExtent;
    m_bPresentScaling = bScaling;
    ++m_iSwapchainCreateCount;

    uint32_t uImageCount = 0;
    VkResult imagesResult = dispatch.getSwapchainImagesKHR(m_swapchain, &uImageCount, nullptr);
    std::vector<VkImage> vecImages(uImageCount);
    if (imagesResult == VK_SUCCESS)
        imagesResult = dispatch.getSwapchainImagesKHR(m_swapchain, &uImageCount, vecImages.data());
    if (imagesResult != VK_SUCCESS)
    {
        DestroySwapchain();
        LogError("failed to get the terminal swapchain images", imagesResult);
        return imagesResult;
    }
    vecImages.resize(uImageCount);
    for (const VkImage image : vecImages)
    {
        SwapchainImage swapchainImage;
        swapchainImage.image = image;
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kSwapchainFormat;
        viewInfo.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkResult result = dispatch.createImageView(&viewInfo, nullptr, &swapchainImage.view);
        if (result != VK_SUCCESS)
            swapchainImage.view = VK_NULL_HANDLE;
        else
            result = dispatch.createSemaphore(&semaphoreInfo, nullptr, &swapchainImage.renderDoneSemaphore);
        if (result != VK_SUCCESS)
            swapchainImage.renderDoneSemaphore = VK_NULL_HANDLE;
        m_vecImages.push_back(swapchainImage);
        if (result != VK_SUCCESS)
        {
            DestroySwapchain();
            LogError("failed to create the terminal swapchain image views", result);
            return result;
        }
    }
    m_bNeedRecreate = false;
    return VK_SUCCESS;
}

bool VulkanView::HasSurface() const
{
    return m_surface != VK_NULL_HANDLE && m_bFramesReady;
}

bool VulkanView::IsReady() const
{
    if (!HasSurface() || m_sizeRequested.isEmpty())
        return false;
    // 오류(장치 잃음)도 "준비됨" 으로 본다: Render 가 오류를 알린다.
    return m_context.GetDispatch().getFenceStatus(m_arrFrames[size_t(m_iFrame)].fence) != VK_NOT_READY;
}

void VulkanView::Resize(const QSize& devicePixelSize)
{
    if (devicePixelSize == m_sizeRequested)
        return;   // 크기가 같으면 다시 만들지 않는다
    m_sizeRequested = devicePixelSize;
    m_bNeedRecreate = true;
}

VkExtent2D VulkanView::GetSwapchainExtent() const
{
    return m_swapchainExtent;
}

VkPresentModeKHR VulkanView::GetPresentMode() const
{
    return m_presentMode;
}

bool VulkanView::UsesPresentScaling() const
{
    return m_bPresentScaling;
}

int VulkanView::GetSwapchainCreateCount() const
{
    return m_iSwapchainCreateCount;
}

void VulkanView::LogError(const char* pWhat, VkResult result)
{
    if (m_bErrorLogged)
        return;
    m_bErrorLogged = true;
    qWarning("Vulkan: %s (VkResult %d)", pWhat, int(result));
}

void VulkanView::ReportError(const char* pWhat, VkResult result, TerminalRenderOutput& out)
{
    LogError(pWhat, result);
    MarkError(result, out);
}

void VulkanView::MarkError(VkResult result, TerminalRenderOutput& out)
{
    // 시간 안에 안 끝남은 잠깐 막힘일 수 있다(부록 A S1): GPU 를 버리지 않고 같은 목록으로 다시 그린다.
    // surface 만 못 쓰게 됐으면 그 화면을 새 핸들로 다시 붙인다. 장치 잃음·메모리 부족·알 수 없는 실패는
    // GPU 를 버리고 모든 화면을 CPU 로 그린다(스펙 §11, 앱은 GPU 오류로 끝나지 않는다).
    if (result == VK_TIMEOUT || result == VK_NOT_READY)
        out.bNotReady = true;
    else if (result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        out.bSurfaceLost = true;
    else
        out.bDeviceLost = true;
}

void VulkanView::Render(const DrawList& list, GlyphCache& cache, TerminalRenderOutput& out)
{
    out = TerminalRenderOutput();
    if (!HasSurface() || m_sizeRequested.isEmpty())
        return;
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();
    Frame& frame = m_arrFrames[size_t(m_iFrame)];

    // 1) 이 프레임 펜스(타임아웃 0). 안 끝났으면 건너뛰고 같은 목록으로 다음에 다시
    const VkResult fenceResult = dispatch.getFenceStatus(frame.fence);
    if (fenceResult == VK_NOT_READY)
    {
        out.bNotReady = true;
        return;
    }
    if (fenceResult != VK_SUCCESS)
    {
        ReportError("failed to read the terminal frame fence", fenceResult, out);
        return;
    }

    // 2) 크기가 바뀌었거나 OUT_OF_DATE/SUBOPTIMAL 이었으면 스왑체인을 다시 만든다.
    //    끝나지 않은 프레임(VK_TIMEOUT)·크기 0(VK_NOT_READY)은 "다음에 다시"(bNotReady, 렌더 스레드의 재시도 간격)이고,
    //    실패는 surface 잃음·장치 잃음으로 알린다(로그는 RecreateSwapchain 이 화면마다 한 번 남긴다).
    if (m_bNeedRecreate || m_swapchain == VK_NULL_HANDLE)
    {
        const VkResult recreateResult = RecreateSwapchain();
        if (recreateResult != VK_SUCCESS)
        {
            MarkError(recreateResult, out);
            return;
        }
    }

    // 3) 인스턴스 버퍼는 획득 전에 마련한다(획득한 뒤에는 실패해도 제출해야 하므로)
    const VkDeviceSize instanceBytes = VkDeviceSize(list.vecQuads.size()) * sizeof(DrawQuad);
    if (instanceBytes > 0 && !EnsureInstanceBuffer(frame, instanceBytes))
    {
        ReportError("failed to allocate the terminal instance buffer", VK_ERROR_OUT_OF_DEVICE_MEMORY, out);   // 스펙 §11
        return;
    }

    // 4) 이미지 획득(타임아웃 0)
    uint32_t uImageIndex = 0;
    const VkResult acquireResult = dispatch.acquireNextImageKHR(m_swapchain, kNoWait, frame.acquireSemaphore, VK_NULL_HANDLE, &uImageIndex);
    if (acquireResult == VK_NOT_READY || acquireResult == VK_TIMEOUT)
    {
        out.bNotReady = true;
        return;
    }
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        m_bNeedRecreate = true;
        out.bNotReady = true;
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
    {
        ReportError("failed to acquire a terminal swapchain image", acquireResult, out);
        return;
    }
    if (acquireResult == VK_SUBOPTIMAL_KHR)
        m_bNeedRecreate = true;   // 이번 이미지는 그리고 표시한 뒤 다음 Render 전에 다시 만든다
    const SwapchainImage& image = m_vecImages[uImageIndex];

    // 5) 기록 → 제출
    if (instanceBytes > 0)
    {
        std::memcpy(frame.pInstanceMapped, list.vecQuads.data(), size_t(instanceBytes));
        vmaFlushAllocation(m_context.GetAllocator(), frame.instanceAllocation, 0, instanceBytes);
    }
    dispatch.resetCommandBuffer(frame.cmd, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult result = dispatch.beginCommandBuffer(frame.cmd, &beginInfo);
    if (result != VK_SUCCESS)
    {
        ReportError("failed to begin the terminal command buffer", result, out);
        return;
    }
    const bool bDrawn = Record(frame, image, list, cache);
    VulkanAtlasTexture& atlas = m_context.GetAtlasTexture(&cache);
    result = dispatch.endCommandBuffer(frame.cmd);
    if (result != VK_SUCCESS)
    {
        atlas.MarkForFullUpload();   // 기록한 아틀라스 올리기가 GPU 에 닿지 않는다
        ReportError("failed to end the terminal command buffer", result, out);
        return;
    }

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = frame.acquireSemaphore;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = image.renderDoneSemaphore;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandInfo.commandBuffer = frame.cmd;
    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitInfo;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;
    dispatch.resetFences(1, &frame.fence);
    result = dispatch.queueSubmit2(m_context.GetQueue(), 1, &submitInfo, frame.fence);
    if (result != VK_SUCCESS)
    {
        atlas.MarkForFullUpload();   // 기록한 아틀라스 올리기가 GPU 에 닿지 않았다
        ReportError("failed to submit the terminal frame", result, out);
        return;
    }
    m_iFrame = (m_iFrame + 1) % kFramesInFlight;

    // 6) 표시
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &image.renderDoneSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &uImageIndex;
    const VkResult presentResult = dispatch.queuePresentKHR(m_context.GetQueue(), &presentInfo);
    if (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR)
    {
        if (presentResult == VK_SUBOPTIMAL_KHR)
            m_bNeedRecreate = true;
        if (!bDrawn)
        {
            // 아틀라스를 GPU 에 올리지 못해(이미지·스테이징 메모리 실패) 지우기만 했다. 획득한 이미지는 돌려줘야 하므로 냈지만
            // 표시로 알리지 않는다: 렌더 스레드가 같은 목록으로 다시 그리고, 그때 RecordUpload 가 못 올린 페이지를 처음부터 올린다.
            // 기록한 올리기는 제출되었으므로 MarkForFullUpload 는 필요 없다.
            out.bNotReady = true;
            return;
        }
        out.bPresented = true;
        out.iPresentMode = int(m_presentMode);
        return;
    }
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // 보이지 않았다: 스왑체인을 다시 만들고 같은 목록을 다시 그린다
        m_bNeedRecreate = true;
        out.bNotReady = true;
        return;
    }
    ReportError("failed to present the terminal frame", presentResult, out);
}

bool VulkanView::Record(Frame& frame, const SwapchainImage& image, const DrawList& list, GlyphCache& cache)
{
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();
    const VkCommandBuffer cmd = frame.cmd;

    // 아틀라스의 바뀐 영역을 올린다(이 명령 버퍼의 이전 제출은 끝났다)
    VulkanAtlasTexture& atlas = m_context.GetAtlasTexture(&cache);
    atlas.RecordUpload(cmd, cache);

    // 이미지 전체를 지우므로 이전 내용은 버린다(UNDEFINED → 색 첨부). 획득 세마포어를 같은 단계에서 기다린다.
    VkImageMemoryBarrier2 toAttachment{};
    toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toAttachment.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.srcAccessMask = VK_ACCESS_2_NONE;
    toAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.image = image.image;
    toAttachment.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &toAttachment;
    dispatch.cmdPipelineBarrier2(cmd, &dependency);

    // 지우기·그리기는 화면 밖 그리기(VulkanOffscreen, 픽셀 비교 테스트)와 같은 함수로 기록한다(화면 크기 = 스왑체인 크기)
    const bool bDrew = m_context.RecordDraw(cmd, image.view, m_swapchainExtent, list, atlas, frame.instanceBuffer);

    // 색 첨부 → 표시
    VkImageMemoryBarrier2 toPresent = toAttachment;
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    toPresent.dstAccessMask = VK_ACCESS_2_NONE;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dependency.pImageMemoryBarriers = &toPresent;
    dispatch.cmdPipelineBarrier2(cmd, &dependency);
    return bDrew || list.vecQuads.empty();   // 쿼드가 없는 목록은 지우기만으로 다 그린 것이다
}
