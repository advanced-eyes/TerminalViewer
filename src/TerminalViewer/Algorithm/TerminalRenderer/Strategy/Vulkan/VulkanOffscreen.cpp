#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanOffscreen.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawList.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanAtlasTexture.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanContext.h"

#include <QtGlobal>

#include <algorithm>
#include <cstring>
#include <limits>

// 읽어 온 B8G8R8A8 바이트(B,G,R,A)를 그대로 ARGB32(0xAARRGGBB) 로 쓴다: 리틀 엔디언에서만 맞다.
#if Q_BYTE_ORDER != Q_LITTLE_ENDIAN
#error "VulkanOffscreen assumes a little-endian host (B8G8R8A8 bytes == QImage::Format_ARGB32_Premultiplied)"
#endif

namespace
{
constexpr VkFormat kTargetFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr int kTargetBytesPerPixel = 4;
constexpr uint64_t kFenceTimeoutNs = 10ull * 1000 * 1000 * 1000;   // 10초 안에 안 끝나면 포기한다
constexpr VkDeviceSize kMinInstanceBytes = 256 * sizeof(DrawQuad);
}

VulkanOffscreen::VulkanOffscreen(VulkanContext& context)
    : m_context(context)
{
}

VulkanOffscreen::~VulkanOffscreen()
{
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();
    // 제출한 일이 남아 있으면 끝날 때까지(시간 제한 없이) 기다린 뒤 지운다. 펜스 대기가 실패하면(장치 잃음 등) 큐를 기다린다.
    if (m_bPending && m_fence != VK_NULL_HANDLE &&
        dispatch.waitForFences(1, &m_fence, VK_TRUE, std::numeric_limits<uint64_t>::max()) != VK_SUCCESS)
        dispatch.queueWaitIdle(m_context.GetQueue());
    if (m_commandBuffer != VK_NULL_HANDLE)
        m_context.ForgetCommandBuffer(m_commandBuffer);
    DestroyInstanceBuffer();
    DestroyTarget();
    if (m_fence != VK_NULL_HANDLE)
        dispatch.destroyFence(m_fence, nullptr);
    if (m_commandPool != VK_NULL_HANDLE)
        dispatch.destroyCommandPool(m_commandPool, nullptr);   // 명령 버퍼도 함께 풀린다
}

bool VulkanOffscreen::EnsureCommands()
{
    if (m_commandBuffer != VK_NULL_HANDLE)
        return true;
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_context.GetQueueFamily();
    if (m_commandPool == VK_NULL_HANDLE && dispatch.createCommandPool(&poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
    {
        m_commandPool = VK_NULL_HANDLE;
        return false;
    }
    if (m_fence == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (dispatch.createFence(&fenceInfo, nullptr, &m_fence) != VK_SUCCESS)
        {
            m_fence = VK_NULL_HANDLE;
            return false;
        }
    }
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    if (dispatch.allocateCommandBuffers(&allocateInfo, &m_commandBuffer) != VK_SUCCESS)
    {
        m_commandBuffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool VulkanOffscreen::EnsureTarget(const QSize& size)
{
    if (m_targetImage != VK_NULL_HANDLE && m_targetSize == size)
        return true;
    DestroyTarget();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kTargetFormat;
    imageInfo.extent = VkExtent3D{uint32_t(size.width()), uint32_t(size.height()), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo imageAllocation{};
    imageAllocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(m_context.GetAllocator(), &imageInfo, &imageAllocation, &m_targetImage, &m_targetAllocation, nullptr) != VK_SUCCESS)
    {
        m_targetImage = VK_NULL_HANDLE;
        m_targetAllocation = nullptr;
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_targetImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kTargetFormat;
    viewInfo.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (m_context.GetDispatch().createImageView(&viewInfo, nullptr, &m_targetView) != VK_SUCCESS)
    {
        m_targetView = VK_NULL_HANDLE;
        DestroyTarget();
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = VkDeviceSize(size.width()) * VkDeviceSize(size.height()) * kTargetBytesPerPixel;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo bufferAllocation{};
    bufferAllocation.usage = VMA_MEMORY_USAGE_AUTO;
    bufferAllocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocated{};
    if (vmaCreateBuffer(m_context.GetAllocator(), &bufferInfo, &bufferAllocation, &m_readbackBuffer, &m_readbackAllocation, &allocated) != VK_SUCCESS)
    {
        m_readbackBuffer = VK_NULL_HANDLE;
        m_readbackAllocation = nullptr;
        DestroyTarget();
        return false;
    }
    m_pReadbackMapped = allocated.pMappedData;
    m_targetSize = size;
    return true;
}

bool VulkanOffscreen::EnsureInstanceBuffer(VkDeviceSize bytes)
{
    if (m_instanceBuffer != VK_NULL_HANDLE && m_instanceCapacity >= bytes)
        return true;
    const VkDeviceSize newCapacity = std::max({bytes, m_instanceCapacity * 2, kMinInstanceBytes});   // 모자라면 2배로
    DestroyInstanceBuffer();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = newCapacity;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocated{};
    if (vmaCreateBuffer(m_context.GetAllocator(), &bufferInfo, &allocationInfo, &m_instanceBuffer, &m_instanceAllocation, &allocated) != VK_SUCCESS)
    {
        m_instanceBuffer = VK_NULL_HANDLE;
        m_instanceAllocation = nullptr;
        return false;
    }
    m_pInstanceMapped = allocated.pMappedData;
    m_instanceCapacity = newCapacity;
    return true;
}

void VulkanOffscreen::DestroyTarget()
{
    if (m_readbackBuffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(m_context.GetAllocator(), m_readbackBuffer, m_readbackAllocation);
    if (m_targetView != VK_NULL_HANDLE)
        m_context.GetDispatch().destroyImageView(m_targetView, nullptr);
    if (m_targetImage != VK_NULL_HANDLE)
        vmaDestroyImage(m_context.GetAllocator(), m_targetImage, m_targetAllocation);
    m_readbackBuffer = VK_NULL_HANDLE;
    m_readbackAllocation = nullptr;
    m_pReadbackMapped = nullptr;
    m_targetView = VK_NULL_HANDLE;
    m_targetImage = VK_NULL_HANDLE;
    m_targetAllocation = nullptr;
    m_targetSize = QSize();
}

void VulkanOffscreen::DestroyInstanceBuffer()
{
    if (m_instanceBuffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(m_context.GetAllocator(), m_instanceBuffer, m_instanceAllocation);
    m_instanceBuffer = VK_NULL_HANDLE;
    m_instanceAllocation = nullptr;
    m_pInstanceMapped = nullptr;
    m_instanceCapacity = 0;
}

QImage VulkanOffscreen::Render(const DrawList& list, GlyphCache& cache, const QSize& devicePixelSize)
{
    if (devicePixelSize.isEmpty())
        return QImage();
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();

    // 앞선 제출이 시간 안에 안 끝났으면 그것부터 기다린다(명령 버퍼·버퍼를 다시 쓰기 전에)
    if (m_bPending)
    {
        if (dispatch.waitForFences(1, &m_fence, VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS)
            return QImage();
        m_bPending = false;
    }

    const VkPipeline pipeline = m_context.GetPipeline(kTargetFormat);
    const VkDeviceSize instanceBytes = VkDeviceSize(list.vecQuads.size()) * sizeof(DrawQuad);
    if (pipeline == VK_NULL_HANDLE || !EnsureCommands() || !EnsureTarget(devicePixelSize))
        return QImage();
    if (instanceBytes > 0 && !EnsureInstanceBuffer(instanceBytes))
        return QImage();
    if (dispatch.resetCommandBuffer(m_commandBuffer, 0) != VK_SUCCESS)
        return QImage();

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dispatch.beginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS)
        return QImage();

    // 아틀라스를 올린다. 기록한 올리기는 그리지 못하더라도 제출한다(텍스처가 기록한 상태와 맞추려고).
    VulkanAtlasTexture& atlas = m_context.GetAtlasTexture(&cache);
    atlas.RecordUpload(m_commandBuffer, cache);
    const bool bAtlasReady = atlas.IsReady();
    if (instanceBytes > 0 && bAtlasReady)
    {
        std::memcpy(m_pInstanceMapped, list.vecQuads.data(), size_t(instanceBytes));
        vmaFlushAllocation(m_context.GetAllocator(), m_instanceAllocation, 0, instanceBytes);
    }

    VkImageMemoryBarrier2 toAttachment{};
    toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toAttachment.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;   // 앞선 읽어 오기 뒤
    toAttachment.srcAccessMask = VK_ACCESS_2_NONE;
    toAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.image = m_targetImage;
    toAttachment.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &toAttachment;
    dispatch.cmdPipelineBarrier2(m_commandBuffer, &dependency);

    // 지우기 색 = 프레임 기본 배경(불투명). 지우기·그리기는 화면(VulkanView)과 같은 함수로 기록한다.
    const VkExtent2D extent{uint32_t(devicePixelSize.width()), uint32_t(devicePixelSize.height())};
    m_context.RecordDraw(m_commandBuffer, m_targetView, extent, list, atlas, m_instanceBuffer);

    // 화면 밖 이미지 → 호스트 버퍼
    VkImageMemoryBarrier2 toTransfer = toAttachment;
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toTransfer.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    dependency.pImageMemoryBarriers = &toTransfer;
    dispatch.cmdPipelineBarrier2(m_commandBuffer, &dependency);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;   // 빈틈 없이: 줄마다 너비 × 4 바이트
    region.bufferImageHeight = 0;
    region.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = VkExtent3D{extent.width, extent.height, 1};
    dispatch.cmdCopyImageToBuffer(m_commandBuffer, m_targetImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readbackBuffer, 1, &region);

    VkBufferMemoryBarrier2 toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = m_readbackBuffer;
    toHost.offset = 0;
    toHost.size = VK_WHOLE_SIZE;
    VkDependencyInfo hostDependency{};
    hostDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    hostDependency.bufferMemoryBarrierCount = 1;
    hostDependency.pBufferMemoryBarriers = &toHost;
    dispatch.cmdPipelineBarrier2(m_commandBuffer, &hostDependency);

    if (dispatch.endCommandBuffer(m_commandBuffer) != VK_SUCCESS)
    {
        atlas.MarkForFullUpload();   // 기록한 아틀라스 올리기가 GPU 에 닿지 않는다
        return QImage();
    }

    VkCommandBufferSubmitInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandInfo.commandBuffer = m_commandBuffer;
    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandInfo;
    dispatch.resetFences(1, &m_fence);
    const VkResult submitResult = dispatch.queueSubmit2(m_context.GetQueue(), 1, &submitInfo, m_fence);
    if (submitResult != VK_SUCCESS)
    {
        qWarning("Vulkan: offscreen submit failed (VkResult %d)", int(submitResult));
        atlas.MarkForFullUpload();   // 기록한 아틀라스 올리기가 GPU 에 닿지 않았다
        return QImage();
    }
    m_bPending = true;
    const VkResult waitResult = dispatch.waitForFences(1, &m_fence, VK_TRUE, kFenceTimeoutNs);
    if (waitResult != VK_SUCCESS)
    {
        qWarning("Vulkan: offscreen render did not finish (VkResult %d)", int(waitResult));
        return QImage();
    }
    m_bPending = false;
    if (instanceBytes > 0 && !bAtlasReady)
        return QImage();   // 아틀라스를 못 만들어 글리프를 그리지 못했다

    vmaInvalidateAllocation(m_context.GetAllocator(), m_readbackAllocation, 0, VK_WHOLE_SIZE);
    QImage image(devicePixelSize, QImage::Format_ARGB32_Premultiplied);
    const size_t uRowBytes = size_t(extent.width) * kTargetBytesPerPixel;
    const uchar* pSource = static_cast<const uchar*>(m_pReadbackMapped);
    for (int iY = 0; iY < image.height(); ++iY)
        std::memcpy(image.scanLine(iY), pSource + size_t(iY) * uRowBytes, uRowBytes);
    return image;
}
