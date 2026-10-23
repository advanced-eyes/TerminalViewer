#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/TerminalRendererGPU.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanContext.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanView.h"

std::unique_ptr<TerminalRendererGPU> TerminalRendererGPU::Create(QString* pError)
{
    std::unique_ptr<VulkanContext> upContext = VulkanContext::Create(pError);
    if (!upContext)
        return nullptr;
    return std::unique_ptr<TerminalRendererGPU>(new TerminalRendererGPU(std::move(upContext)));
}

TerminalRendererGPU::TerminalRendererGPU(std::unique_ptr<VulkanContext> upContext)
    : m_upContext(std::move(upContext))
{
}

TerminalRendererGPU::~TerminalRendererGPU()
{
    m_hashViews.clear();   // 각 화면이 자기 펜스를 기다리고 스왑체인·surface 를 지운 뒤 핸들을 놓는다
    m_upContext.reset();
}

ETerminalRenderStrategy TerminalRendererGPU::GetType() const
{
    return ETerminalRenderStrategy::GPU;
}

bool TerminalRendererGPU::Execute(const TerminalRenderInput& in, TerminalRenderOutput& out)
{
    out = TerminalRenderOutput();
    const auto it = m_hashViews.find(in.iViewId);
    if (it == m_hashViews.end() || !in.pDrawList || !in.pGlyphCache)
        return false;
    it->second->Render(*in.pDrawList, *in.pGlyphCache, out);
    return out.bPresented;
}

bool TerminalRendererGPU::AttachView(int viewId, const TerminalNativeHandle& handle, const QSize& devicePixelSize)
{
    m_hashViews.erase(viewId);   // 같은 id 로 다시 붙이면 앞의 화면을 지운다
    // VulkanView 가 핸들을 맡는다: 만들지 못해도 지울 때 놓는다.
    auto upView = std::make_unique<VulkanView>(*m_upContext, handle, devicePixelSize);
    if (!upView->HasSurface())
        return false;
    m_hashViews.emplace(viewId, std::move(upView));
    return true;
}

void TerminalRendererGPU::DetachView(int viewId)
{
    m_hashViews.erase(viewId);
}

void TerminalRendererGPU::ResizeView(int viewId, const QSize& devicePixelSize)
{
    const auto it = m_hashViews.find(viewId);
    if (it != m_hashViews.end())
        it->second->Resize(devicePixelSize);
}

bool TerminalRendererGPU::IsReady(int viewId) const
{
    const auto it = m_hashViews.find(viewId);
    return it != m_hashViews.end() && it->second->IsReady();
}

void TerminalRendererGPU::ForgetGlyphCache(const GlyphCache* pCache)
{
    m_upContext->ReleaseAtlasTexture(pCache);
}
