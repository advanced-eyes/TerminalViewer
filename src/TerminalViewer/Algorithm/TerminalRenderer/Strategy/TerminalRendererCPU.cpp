#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/TerminalRendererCPU.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/SoftwareCompositor.h"

namespace
{
// 바뀐 영역을 감싼 사각형의 넓이가 화면의 1/kWholeFramePatchDivisor 이상이면 화면 전체를 조각으로 낸다(절반)
constexpr qint64 kWholeFramePatchDivisor = 2;

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

    // 바뀐 영역을 감싼 사각형이 화면의 절반 이상이면(전체 합성 · 스크롤 애니메이션 · 대량 출력 · 맨 위와 맨 아래 줄) 화면 전체를
    // 조각으로 낸다: 복사하지 않고 이미지를 함께 쓴다(QImage 암시적 공유). 바뀌지 않은 픽셀도 지금 화면 그대로이므로 덮어 써도 같다.
    // 받는 쪽이 조각을 쥐고 있는 동안 다음 합성이 쓰려 하면 그때 QImage 가 떼어 복사하므로(쓰기 시 복사, Compose 의 bits())
    // 받은 조각은 바뀌지 않는다. TerminalSurface 는 받자마자 자기 이미지에 옮기고 놓으므로 보통은 복사가 일어나지 않는다.
    // 절반보다 작으면 그 사각형만 복사한다.
    const QRect rectFrame(QPoint(0, 0), view.imgFrame.size());
    const QRect rectBound = region.boundingRect();
    const qint64 llBoundArea = qint64(rectBound.width()) * rectBound.height();
    const qint64 llFrameArea = qint64(rectFrame.width()) * rectFrame.height();
    if (kWholeFramePatchDivisor * llBoundArea >= llFrameArea)
    {
        out.rectPatch = rectFrame;
        out.imgPatch = view.imgFrame;
    }
    else
    {
        out.rectPatch = rectBound;
        out.imgPatch = view.imgFrame.copy(rectBound);
    }
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
