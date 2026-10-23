#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <unordered_map>

class VulkanContext;

// 아틀라스 페이지 하나에 기록한 올리기 횟수(누적, 진단·테스트용)
struct AtlasUploadStats
{
    int iFullUploads = 0;       // 전체를 올린 횟수
    int iPartialUploads = 0;    // 바뀐 영역만 올린 횟수
    int iPartialRects = 0;      // 바뀐 영역만 올릴 때 올린 사각형 수
};

// GlyphCache 의 두 아틀라스(흑백 R8, 컬러 RGBA8 premultiplied)를 GPU 이미지로 갖고 디스크립터 세트(binding 0 = 흑백, 1 = 컬러)를 묶는다.
// VulkanContext::GetAtlasTexture 가 만든다. 렌더 스레드 전용.
class VulkanAtlasTexture
{
public:
    static constexpr int kPageCount = 2;   // EAtlasPage 순서: MONO, COLOR

    explicit VulkanAtlasTexture(VulkanContext& context);
    ~VulkanAtlasTexture();

    VulkanAtlasTexture(const VulkanAtlasTexture&) = delete;
    VulkanAtlasTexture& operator=(const VulkanAtlasTexture&) = delete;

    // 아틀라스의 바뀐 영역(크기가 바뀌었으면 새 이미지 전체)을 올리는 명령을 cmd 에 기록한다.
    // 스테이징 버퍼는 그 프레임 펜스가 끝난 뒤 다시 쓴다
    //   cmd 는 기록 중이어야 하고 cmd 의 이전 제출은 끝나 있어야 한다(펜스를 기다린 뒤 다시 기록하는 명령 버퍼).
    //   스테이징 버퍼는 명령 버퍼마다 하나라서 이 조건이면 다시 써도 안전하다.
    //   처음 쓰는 아틀라스, 크기가 바뀐 아틀라스, Reset(세대 바뀜) 뒤, 다른 GlyphAtlas 객체(같은 주소의 새 GlyphCache)면 전체를 올린다.
    //   이미지를 다시 만들 때는 큐를 기다린 뒤 옛 이미지를 지운다(다른 화면의 제출이 아직 읽고 있을 수 있다).
    //   기록하면서 아틀라스의 바뀐 영역을 꺼내고 "올렸다" 로 표시한다: 그 cmd 를 제출하지 못하면 MarkForFullUpload 를 부른다.
    void RecordUpload(VkCommandBuffer cmd, GlyphCache& cache);
    // 올리기를 기록한 명령 버퍼를 제출하지 못했을 때(끝내기·제출 실패) 부른다. 다음 RecordUpload 가 두 페이지 전체를 올린다
    void MarkForFullUpload();
    VkDescriptorSet GetDescriptorSet() const;
    bool IsReady() const;                          // 두 이미지가 있고 디스크립터 세트가 쓰였다(그릴 수 있다)
    void ForgetCommandBuffer(VkCommandBuffer cmd); // cmd 용 스테이징 버퍼를 놓는다(그 제출이 끝난 뒤)
    AtlasUploadStats GetUploadStats(EAtlasPage page) const;

private:
    struct Page
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        int iSize = 0;
        uint32_t uGeneration = 0;
        uint64_t uAtlasId = 0;       // GlyphAtlas::GetInstanceId(): 다른 아틀라스(같은 주소에 새로 만든 GlyphCache 포함)를 알아챈다
        bool bUploaded = false;      // 전체를 올려 SHADER_READ_ONLY 가 되었다(MarkForFullUpload 가 지운다)
        AtlasUploadStats stats;
    };

    struct Staging
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void* pMapped = nullptr;
        VkDeviceSize size = 0;
    };

    bool CreatePageImage(Page& page, VkFormat format, int size);
    void DestroyPageImage(Page& page);
    Staging* AcquireStaging(VkCommandBuffer cmd, VkDeviceSize size);
    void DestroyStaging(Staging& staging);
    void WriteDescriptors();

    VulkanContext& m_context;
    Page m_arrPages[kPageCount];
    std::unordered_map<VkCommandBuffer, Staging> m_hashStaging;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    bool m_bDescriptorsWritten = false;
};
