#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanAtlasTexture.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanContext.h"

#include <QImage>
#include <QRect>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
constexpr uint32_t kMonoBinding = 0;
constexpr uint32_t kColorBinding = 1;
constexpr VkDeviceSize kStagingAlignment = 16;            // 복사 영역마다 시작 오프셋을 맞춘다(텍셀 크기 1·4 의 배수)
constexpr VkDeviceSize kStagingMinBytes = 64 * 1024;      // 작은 업로드마다 버퍼를 다시 만들지 않는다
constexpr VkDeviceSize kStagingKeepBytes = 1024 * 1024;   // 이보다 큰 버퍼(전체 업로드용)는 다음 차례에 필요한 만큼으로 줄인다

VkFormat PageFormat(int pageIndex)
{
    return pageIndex == int(EAtlasPage::MONO) ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
}

int BytesPerPixel(EAtlasFormat format)
{
    return format == EAtlasFormat::A8 ? 1 : 4;   // A8: 알파, RGBA8: 바이트 순서 R,G,B,A (premultiplied)
}

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

struct PageUpload
{
    bool bRecreate = false;          // 이미지를 (다시) 만든다
    bool bFull = false;              // 전체를 올린다(옛 내용 버림)
    std::vector<QRect> vecRects;     // 올릴 영역(아틀라스 좌표)
    std::vector<VkBufferImageCopy> vecCopies;
};
}

VulkanAtlasTexture::VulkanAtlasTexture(VulkanContext& context)
    : m_context(context)
{
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = uint32_t(kPageCount);
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (dispatch.createDescriptorPool(&poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
    {
        m_descriptorPool = VK_NULL_HANDLE;
        qWarning("Vulkan: failed to create the atlas descriptor pool");
        return;
    }
    const VkDescriptorSetLayout setLayout = m_context.GetDescriptorSetLayout();
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    if (dispatch.allocateDescriptorSets(&allocateInfo, &m_descriptorSet) != VK_SUCCESS)
    {
        m_descriptorSet = VK_NULL_HANDLE;
        qWarning("Vulkan: failed to allocate the atlas descriptor set");
    }
}

VulkanAtlasTexture::~VulkanAtlasTexture()
{
    // 이 텍스처를 쓰는 제출은 모두 끝나 있어야 한다(VulkanContext 가 장치·큐를 기다린 뒤 지운다).
    for (auto& pairStaging : m_hashStaging)
        DestroyStaging(pairStaging.second);
    m_hashStaging.clear();
    for (Page& page : m_arrPages)
        DestroyPageImage(page);
    if (m_descriptorPool != VK_NULL_HANDLE)
        m_context.GetDispatch().destroyDescriptorPool(m_descriptorPool, nullptr);   // 세트도 함께 풀린다
}

bool VulkanAtlasTexture::CreatePageImage(Page& page, VkFormat format, int size)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = VkExtent3D{uint32_t(size), uint32_t(size), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(m_context.GetAllocator(), &imageInfo, &allocationInfo, &page.image, &page.allocation, nullptr) != VK_SUCCESS)
    {
        page.image = VK_NULL_HANDLE;
        page.allocation = nullptr;
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = page.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (m_context.GetDispatch().createImageView(&viewInfo, nullptr, &page.view) != VK_SUCCESS)
    {
        page.view = VK_NULL_HANDLE;
        DestroyPageImage(page);
        return false;
    }
    page.iSize = size;
    page.bUploaded = false;
    return true;
}

void VulkanAtlasTexture::DestroyPageImage(Page& page)
{
    if (page.view != VK_NULL_HANDLE)
        m_context.GetDispatch().destroyImageView(page.view, nullptr);
    if (page.image != VK_NULL_HANDLE)
        vmaDestroyImage(m_context.GetAllocator(), page.image, page.allocation);
    const AtlasUploadStats stats = page.stats;   // 횟수는 이미지를 다시 만들어도 이어 센다
    page = Page();
    page.stats = stats;
}

VulkanAtlasTexture::Staging* VulkanAtlasTexture::AcquireStaging(VkCommandBuffer cmd, VkDeviceSize size)
{
    // 이 cmd 의 이전 제출은 끝났다(RecordUpload 의 조건): 그 버퍼를 다시 쓰거나 크기를 바꿔도 된다.
    auto itFound = m_hashStaging.find(cmd);
    if (itFound != m_hashStaging.end())
    {
        Staging& staging = itFound->second;
        const bool bFits = staging.size >= size;
        const bool bOversized = staging.size > kStagingKeepBytes && staging.size > size * 2;
        if (size > 0 && bFits && !bOversized)
            return &staging;
        if (size == 0 && !bOversized)
            return nullptr;
        DestroyStaging(staging);
        m_hashStaging.erase(itFound);
    }
    if (size == 0)
        return nullptr;

    Staging staging;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = std::max(size, kStagingMinBytes);
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocated{};
    if (vmaCreateBuffer(m_context.GetAllocator(), &bufferInfo, &allocationInfo, &staging.buffer, &staging.allocation, &allocated) != VK_SUCCESS)
        return nullptr;
    staging.pMapped = allocated.pMappedData;
    staging.size = bufferInfo.size;
    return &(m_hashStaging[cmd] = staging);
}

void VulkanAtlasTexture::DestroyStaging(Staging& staging)
{
    if (staging.buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(m_context.GetAllocator(), staging.buffer, staging.allocation);
    staging = Staging();
}

void VulkanAtlasTexture::WriteDescriptors()
{
    m_bDescriptorsWritten = false;
    if (m_descriptorSet == VK_NULL_HANDLE)
        return;
    for (const Page& page : m_arrPages)
    {
        if (page.view == VK_NULL_HANDLE)
            return;
    }
    VkDescriptorImageInfo arrImages[kPageCount]{};
    VkWriteDescriptorSet arrWrites[kPageCount]{};
    for (int iPage = 0; iPage < kPageCount; ++iPage)
    {
        arrImages[iPage].sampler = VK_NULL_HANDLE;   // 레이아웃의 고정 샘플러
        arrImages[iPage].imageView = m_arrPages[iPage].view;
        arrImages[iPage].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        arrWrites[iPage].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        arrWrites[iPage].dstSet = m_descriptorSet;
        arrWrites[iPage].dstBinding = iPage == int(EAtlasPage::MONO) ? kMonoBinding : kColorBinding;
        arrWrites[iPage].descriptorCount = 1;
        arrWrites[iPage].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        arrWrites[iPage].pImageInfo = &arrImages[iPage];
    }
    m_context.GetDispatch().updateDescriptorSets(kPageCount, arrWrites, 0, nullptr);
    m_bDescriptorsWritten = true;
}

void VulkanAtlasTexture::RecordUpload(VkCommandBuffer cmd, GlyphCache& cache)
{
    const vkb::DispatchTable& dispatch = m_context.GetDispatch();

    // 1) 페이지마다 무엇을 올릴지 정한다
    PageUpload arrUploads[kPageCount];
    VkDeviceSize totalBytes = 0;
    bool bAnyRecreate = false;
    for (int iPage = 0; iPage < kPageCount; ++iPage)
    {
        GlyphAtlas& atlas = cache.GetAtlas(EAtlasPage(iPage));
        Page& page = m_arrPages[iPage];
        PageUpload& upload = arrUploads[iPage];
        // TakeDirtyRects 가 크기 바뀜 표시를 지우므로 먼저 읽는다. 새 아틀라스는 바뀐 영역이 없다고 하므로 처음엔 전체를 올린다.
        const bool bResized = atlas.WasResizedSinceLastTake();
        const std::vector<QRect> vecDirty = atlas.TakeDirtyRects();
        upload.bRecreate = page.image == VK_NULL_HANDLE || page.iSize != atlas.GetSize();
        upload.bFull = upload.bRecreate || bResized || !page.bUploaded || page.uGeneration != atlas.GetGeneration() ||
                       page.uAtlasId != atlas.GetInstanceId();
        if (upload.bFull)
        {
            upload.vecRects.push_back(QRect(0, 0, atlas.GetSize(), atlas.GetSize()));
        }
        else
        {
            const QRect bounds(0, 0, page.iSize, page.iSize);
            for (const QRect& rect : vecDirty)
            {
                const QRect clipped = rect.intersected(bounds);
                if (!clipped.isEmpty())
                    upload.vecRects.push_back(clipped);
            }
        }
        const int iBytesPerPixel = BytesPerPixel(atlas.GetFormat());
        for (const QRect& rect : upload.vecRects)
            totalBytes = AlignUp(totalBytes, kStagingAlignment) + VkDeviceSize(rect.width()) * VkDeviceSize(rect.height()) * VkDeviceSize(iBytesPerPixel);
        bAnyRecreate = bAnyRecreate || upload.bRecreate;
    }

    // 2) 크기가 바뀌었거나 처음이면 이미지를 (다시) 만든다. 옛 이미지를 다른 화면의 제출이 아직 읽고 있을 수 있으므로
    //    큐를 기다린 뒤 지운다(크기 변화는 드물다). 디스크립터 세트도 이때만 다시 쓴다(이 cmd 에서 묶기 전이다).
    if (bAnyRecreate)
    {
        dispatch.queueWaitIdle(m_context.GetQueue());
        for (int iPage = 0; iPage < kPageCount; ++iPage)
        {
            if (!arrUploads[iPage].bRecreate)
                continue;
            Page& page = m_arrPages[iPage];
            DestroyPageImage(page);
            if (!CreatePageImage(page, PageFormat(iPage), cache.GetAtlas(EAtlasPage(iPage)).GetSize()))
            {
                qWarning("Vulkan: failed to create the %s atlas image (%d px)", iPage == int(EAtlasPage::MONO) ? "mono" : "color",
                         cache.GetAtlas(EAtlasPage(iPage)).GetSize());
                arrUploads[iPage].vecRects.clear();
            }
        }
        WriteDescriptors();
    }

    // 3) 스테이징 버퍼에 픽셀을 쓴다(이 cmd 의 이전 제출은 끝났다)
    Staging* pStaging = AcquireStaging(cmd, totalBytes);
    if (totalBytes > 0 && !pStaging)
    {
        qWarning("Vulkan: failed to allocate %llu bytes of atlas staging memory", static_cast<unsigned long long>(totalBytes));
        // 바뀐 영역은 이미 꺼냈으므로 다음 차례에 전체를 올리게 한다(그때까지 그리지 않는다)
        for (int iPage = 0; iPage < kPageCount; ++iPage)
        {
            if (!arrUploads[iPage].vecRects.empty())
                m_arrPages[iPage].bUploaded = false;
        }
        return;
    }
    VkDeviceSize offset = 0;
    for (int iPage = 0; iPage < kPageCount; ++iPage)
    {
        const GlyphAtlas& atlas = cache.GetAtlas(EAtlasPage(iPage));
        const QImage& image = atlas.GetImage();
        const int iBytesPerPixel = BytesPerPixel(atlas.GetFormat());
        PageUpload& upload = arrUploads[iPage];
        for (const QRect& rect : upload.vecRects)
        {
            offset = AlignUp(offset, kStagingAlignment);
            uchar* pDst = static_cast<uchar*>(pStaging->pMapped) + offset;
            const size_t uRowBytes = size_t(rect.width()) * size_t(iBytesPerPixel);
            for (int iY = rect.top(); iY <= rect.bottom(); ++iY)
            {
                std::memcpy(pDst, image.constScanLine(iY) + size_t(rect.left()) * size_t(iBytesPerPixel), uRowBytes);
                pDst += uRowBytes;
            }
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.bufferRowLength = 0;     // 빈틈 없이
            copy.bufferImageHeight = 0;
            copy.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = VkOffset3D{rect.left(), rect.top(), 0};
            copy.imageExtent = VkExtent3D{uint32_t(rect.width()), uint32_t(rect.height()), 1};
            upload.vecCopies.push_back(copy);
            offset += VkDeviceSize(uRowBytes) * VkDeviceSize(rect.height());
        }
    }
    if (pStaging)
        vmaFlushAllocation(m_context.GetAllocator(), pStaging->allocation, 0, VK_WHOLE_SIZE);

    // 4) 배리어 → 복사 → 배리어. 앞선 제출의 프래그먼트 읽기가 끝난 뒤 쓰고, 다 쓴 뒤 프래그먼트가 읽는다.
    std::vector<VkImageMemoryBarrier2> vecBefore;
    std::vector<VkImageMemoryBarrier2> vecAfter;
    for (int iPage = 0; iPage < kPageCount; ++iPage)
    {
        const PageUpload& upload = arrUploads[iPage];
        if (upload.vecCopies.empty())
            continue;
        VkImageMemoryBarrier2 before{};
        before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        before.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        before.srcAccessMask = VK_ACCESS_2_NONE;   // 읽기 뒤 쓰기: 실행 순서만 지키면 된다
        before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        before.oldLayout = upload.bFull ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.image = m_arrPages[iPage].image;
        before.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vecBefore.push_back(before);

        VkImageMemoryBarrier2 after = before;
        after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        after.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        after.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vecAfter.push_back(after);
    }
    if (vecBefore.empty())
        return;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = uint32_t(vecBefore.size());
    dependency.pImageMemoryBarriers = vecBefore.data();
    dispatch.cmdPipelineBarrier2(cmd, &dependency);
    for (int iPage = 0; iPage < kPageCount; ++iPage)
    {
        const PageUpload& upload = arrUploads[iPage];
        if (!upload.vecCopies.empty())
            dispatch.cmdCopyBufferToImage(cmd, pStaging->buffer, m_arrPages[iPage].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          uint32_t(upload.vecCopies.size()), upload.vecCopies.data());
    }
    dependency.imageMemoryBarrierCount = uint32_t(vecAfter.size());
    dependency.pImageMemoryBarriers = vecAfter.data();
    dispatch.cmdPipelineBarrier2(cmd, &dependency);

    // 5) 기록한 대로 상태를 옮긴다(이 cmd 는 제출된다)
    for (int iPage = 0; iPage < kPageCount; ++iPage)
    {
        if (arrUploads[iPage].vecCopies.empty())
            continue;
        const GlyphAtlas& atlas = cache.GetAtlas(EAtlasPage(iPage));
        Page& page = m_arrPages[iPage];
        page.uGeneration = atlas.GetGeneration();
        page.uAtlasId = atlas.GetInstanceId();
        page.bUploaded = true;
        if (arrUploads[iPage].bFull)
        {
            ++page.stats.iFullUploads;
        }
        else
        {
            ++page.stats.iPartialUploads;
            page.stats.iPartialRects += int(arrUploads[iPage].vecCopies.size());
        }
    }
}

void VulkanAtlasTexture::MarkForFullUpload()
{
    // 기록한 올리기가 GPU 에 닿지 않았다: 이미지 내용과 레이아웃을 모르므로 다음에 두 페이지 전체를(UNDEFINED 에서) 올리고
    // 그때까지는 그리지 않는다(IsReady 거짓).
    for (Page& page : m_arrPages)
        page.bUploaded = false;
}

AtlasUploadStats VulkanAtlasTexture::GetUploadStats(EAtlasPage page) const
{
    return m_arrPages[int(page)].stats;
}

VkDescriptorSet VulkanAtlasTexture::GetDescriptorSet() const
{
    return m_descriptorSet;
}

bool VulkanAtlasTexture::IsReady() const
{
    if (m_descriptorSet == VK_NULL_HANDLE || !m_bDescriptorsWritten)
        return false;
    for (const Page& page : m_arrPages)
    {
        if (!page.bUploaded)
            return false;
    }
    return true;
}

void VulkanAtlasTexture::ForgetCommandBuffer(VkCommandBuffer cmd)
{
    const auto itFound = m_hashStaging.find(cmd);
    if (itFound == m_hashStaging.end())
        return;
    DestroyStaging(itFound->second);
    m_hashStaging.erase(itFound);
}
