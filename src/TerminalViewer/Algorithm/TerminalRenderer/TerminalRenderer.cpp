#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/TerminalRendererCPU.h"
#ifdef TERMINALVIEWER_GPU
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/TerminalRendererGPU.h"
#endif

void ITerminalRenderStrategy::ForgetGlyphCache(const GlyphCache*)
{
}

namespace
{
void ReleaseHandle(const TerminalNativeHandle& handle)
{
    if (handle.funcRelease)
    {
        handle.funcRelease(handle.pHandle);
    }
}

std::unique_ptr<ITerminalRenderStrategy> CreateDefaultGpu(QString* pError)
{
#ifdef TERMINALVIEWER_GPU
    return TerminalRendererGPU::Create(pError);
#else
    // 문구는 Ocean 에서 옮겨 온 그대로 둔다(Ocean 은 OCEAN_TERMINAL_GPU 값을 TERMINALVIEWER_GPU 로 넘긴다).
    *pError = QStringLiteral("GPU 렌더러 없이 만든 빌드다(OCEAN_TERMINAL_GPU=OFF)");
    return nullptr;
#endif
}
}

TerminalRenderer::TerminalRenderer()
{
    std::unique_ptr<ITerminalRenderStrategy> upCpu = std::make_unique<TerminalRendererCPU>();
    m_hashStrategy.emplace(upCpu->GetType(), std::move(upCpu));
}

TerminalRenderer::~TerminalRenderer()
{
    // GPU Strategy(화면의 surface·장치)를 먼저 지운다. 각 Strategy 가 남은 화면의 핸들을 놓는다.
    m_hashStrategy.erase(ETerminalRenderStrategy::GPU);
    m_hashStrategy.clear();
}

void TerminalRenderer::SetStrategy(ETerminalRenderStrategy type)
{
    // GPU 는 처음 고를 때 장치를 만든다(앱 시작 때 만들지 않는다). 만들지 못하면 지금 Strategy 로 남는다.
    if (type == ETerminalRenderStrategy::GPU && !m_hashStrategy.count(type))
    {
        QString strError;
        std::unique_ptr<ITerminalRenderStrategy> upGpu = m_funcGpuFactory ? m_funcGpuFactory(&strError) : CreateDefaultGpu(&strError);
        if (upGpu)
        {
            m_strGpuError.clear();
            m_hashStrategy.emplace(ETerminalRenderStrategy::GPU, std::move(upGpu));
        }
        else
        {
            m_strGpuError = strError.isEmpty() ? QStringLiteral("GPU Strategy 를 만들지 못했다") : strError;
            qWarning("Terminal renderer: GPU unavailable, drawing with CPU: %s", qPrintable(m_strGpuError));   // 로그 한 줄(스펙 §11)
        }
    }
    if (m_hashStrategy.count(type))
    {
        m_eStrategy = type;
    }
}

ETerminalRenderStrategy TerminalRenderer::GetStrategy() const
{
    return m_eStrategy;
}

ETerminalRenderStrategy TerminalRenderer::GetViewStrategy(int viewId) const
{
    const auto it = m_hashViewStrategy.find(viewId);
    return it != m_hashViewStrategy.end() ? it->second : m_eStrategy;
}

bool TerminalRenderer::HasStrategy(ETerminalRenderStrategy type) const
{
    return m_hashStrategy.count(type) > 0;
}

QString TerminalRenderer::GetGpuError() const
{
    return m_strGpuError;
}

ITerminalRenderStrategy* TerminalRenderer::FindViewStrategy(int viewId) const
{
    const auto it = m_hashStrategy.find(GetViewStrategy(viewId));
    return it != m_hashStrategy.end() ? it->second.get() : nullptr;
}

bool TerminalRenderer::Execute(const TerminalRenderInput& in, TerminalRenderOutput& out)
{
    ITerminalRenderStrategy* pStrategy = FindViewStrategy(in.iViewId);
    if (!pStrategy)
    {
        out = TerminalRenderOutput();
        return false;
    }
    return pStrategy->Execute(in, out);
}

bool TerminalRenderer::AttachView(int viewId, const TerminalNativeHandle& handle, const QSize& devicePixelSize)
{
    return AttachView(viewId, handle, devicePixelSize, m_eStrategy);
}

bool TerminalRenderer::AttachView(int viewId, const TerminalNativeHandle& handle, const QSize& devicePixelSize, ETerminalRenderStrategy strategy)
{
    const auto it = m_hashStrategy.find(strategy);
    if (it == m_hashStrategy.end())
    {
        ReleaseHandle(handle);
        return false;
    }
    if (!it->second->AttachView(viewId, handle, devicePixelSize))
    {
        return false;
    }
    m_hashViewStrategy[viewId] = strategy;
    return true;
}

void TerminalRenderer::DetachView(int viewId)
{
    if (ITerminalRenderStrategy* pStrategy = FindViewStrategy(viewId))
    {
        pStrategy->DetachView(viewId);
    }
    m_hashViewStrategy.erase(viewId);
}

void TerminalRenderer::ResizeView(int viewId, const QSize& devicePixelSize)
{
    if (ITerminalRenderStrategy* pStrategy = FindViewStrategy(viewId))
    {
        pStrategy->ResizeView(viewId, devicePixelSize);
    }
}

bool TerminalRenderer::IsReady(int viewId) const
{
    const ITerminalRenderStrategy* pStrategy = FindViewStrategy(viewId);
    return pStrategy && pStrategy->IsReady(viewId);
}

void TerminalRenderer::SetGpuFactory(TerminalGpuStrategyFactory factory)
{
    m_funcGpuFactory = std::move(factory);
}

std::vector<int> TerminalRenderer::DropGpu()
{
    std::vector<int> vecDropped;
    for (auto it = m_hashViewStrategy.begin(); it != m_hashViewStrategy.end();)
    {
        if (it->second == ETerminalRenderStrategy::GPU)
        {
            vecDropped.push_back(it->first);
            it = m_hashViewStrategy.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Strategy 를 지우면 그 화면들의 surface 를 지우고 핸들을 놓은 뒤 장치를 지운다.
    m_hashStrategy.erase(ETerminalRenderStrategy::GPU);
    m_eStrategy = ETerminalRenderStrategy::CPU;
    return vecDropped;
}

void TerminalRenderer::ForgetGlyphCache(const GlyphCache* pCache)
{
    for (auto& pairStrategy : m_hashStrategy)
    {
        pairStrategy.second->ForgetGlyphCache(pCache);
    }
}
