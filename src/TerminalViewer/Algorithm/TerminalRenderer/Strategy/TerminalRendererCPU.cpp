#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/TerminalRendererCPU.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/SoftwareCompositor.h"

namespace
{
void ReleaseHandle(const TerminalNativeHandle& handle)
{
    if (handle.funcRelease)
    {
        handle.funcRelease(handle.pHandle);
    }
}
}

TerminalRendererCPU::~TerminalRendererCPU()
{
    for (const auto& pairView : m_hashViews)
    {
        ReleaseHandle(pairView.second.handle);
    }
}

ETerminalRenderStrategy TerminalRendererCPU::GetType() const
{
    return ETerminalRenderStrategy::CPU;
}

bool TerminalRendererCPU::Execute(const TerminalRenderInput& in, TerminalRenderOutput& out)
{
    out = TerminalRenderOutput();
    auto it = m_hashViews.find(in.iViewId);
    if (it == m_hashViews.end() || !in.pDrawList || !in.pGlyphCache)
    {
        return false;
    }

    View& view = it->second;
    const QRegion region = SoftwareCompositor::Compose(*in.pDrawList, in.pGlyphCache->GetAtlas(EAtlasPage::MONO),
                                                       in.pGlyphCache->GetAtlas(EAtlasPage::COLOR), view.imgFrame, view.size);
    if (region.isEmpty())
    {
        return true;
    }

    out.rectPatch = region.boundingRect();
    out.imgPatch = view.imgFrame.copy(out.rectPatch);
    out.bPresented = true;
    return true;
}

bool TerminalRendererCPU::AttachView(int viewId, const TerminalNativeHandle& handle, const QSize& devicePixelSize)
{
    auto it = m_hashViews.find(viewId);
    if (it != m_hashViews.end())
    {
        ReleaseHandle(it->second.handle);   // 같은 id 로 다시 붙이면 앞의 핸들을 놓는다
    }
    m_hashViews[viewId] = View{devicePixelSize, QImage(), handle};
    return true;
}

void TerminalRendererCPU::DetachView(int viewId)
{
    auto it = m_hashViews.find(viewId);
    if (it == m_hashViews.end())
    {
        return;
    }
    const TerminalNativeHandle handle = it->second.handle;
    m_hashViews.erase(it);
    ReleaseHandle(handle);
}

void TerminalRendererCPU::ResizeView(int viewId, const QSize& devicePixelSize)
{
    auto it = m_hashViews.find(viewId);
    if (it != m_hashViews.end())
    {
        it->second.size = devicePixelSize;
        it->second.imgFrame = QImage();
    }
}

bool TerminalRendererCPU::IsReady(int) const
{
    return true;
}
