#include "TerminalViewer/Thread/TerminalRenderThread.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawListBuilder.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/FrameScheduler.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/ScrollAnimator.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalPlatform.h"
#include "TerminalViewer/Thread/Channel.h"

#include <QThread>
#include <QtDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod (winmm)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr int kStopWaitMs = 4000;                        // 안전판. DoWork 는 지금 그리는 화면 하나만 마치고 돌아온다
constexpr int kDefaultFontPointSize = 14;
constexpr int kOverscanAbove = 0;
constexpr int kOverscanBelow = 1;                        // 스크롤 오프셋으로 반쯤 보이는 아래 줄
constexpr double kNsPerSecond = 1'000'000'000.0;
constexpr int kMaxConsecutiveSurfaceLosses = 3;          // 표시 없이 이만큼 잇달아 surface 를 잃으면 그 화면은 CPU 로

qint64 NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::chrono::steady_clock::time_point ToTimePoint(qint64 ns)
{
    return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(ns)));
}

#ifdef Q_OS_WIN
// Windows 의 대기(condition_variable::wait_until)는 시스템 타이머 해상도(기본 약 15.6ms) 단위로만 깨어난다.
// 그대로 두면 마감까지 기다릴 때마다 한 틱씩 늦어 60Hz 애니메이션이 30fps, 120Hz 가 60fps 로 떨어진다.
// 다음 마감이 가까운 동안(애니메이션 프레임을 이어 그리는 동안)만 1ms 해상도를 요청하고, 한가하면 돌려준다.
constexpr qint64 kFineTimerDeadlineNs = 50'000'000;     // 이보다 가까운 마감이면 1ms 해상도로 기다린다
constexpr UINT kFineTimerPeriodMs = 1;

class FineTimerResolution
{
public:
    FineTimerResolution()
        : m_bFine(false)
    {
    }
    ~FineTimerResolution() { SetFine(false); }

    FineTimerResolution(const FineTimerResolution&) = delete;
    FineTimerResolution& operator=(const FineTimerResolution&) = delete;

public:
    void SetFine(bool fine)
    {
        if (fine == m_bFine)
        {
            return;
        }
        if (fine)
        {
            m_bFine = timeBeginPeriod(kFineTimerPeriodMs) == TIMERR_NOERROR;
        }
        else
        {
            timeEndPeriod(kFineTimerPeriodMs);
            m_bFine = false;
        }
    }

private:
    bool m_bFine;
};
#endif

QFont DefaultFont()
{
    return QFont(QStringLiteral("Cascadia Mono"), kDefaultFontPointSize);
}

void ReleaseHandle(const TerminalNativeHandle& handle)
{
    if (handle.funcRelease)
    {
        handle.funcRelease(handle.pHandle);
    }
}

bool HasRowBelow(const TerminalFrame& frame)
{
    for (const TerminalRow& row : frame.vecRows)
    {
        if (row.iViewportY >= frame.iRows)
        {
            return true;
        }
    }
    return false;
}

bool CursorBlinks(const TerminalFrame& frame)
{
    return frame.cursor.bVisible && frame.cursor.bBlinking && !frame.optPreedit;
}

// DetachViewAndWait 가 기다리는 완료 표시
class DetachAck
{
public:
    void Signal()
    {
        {
            std::lock_guard<std::mutex> lock(m_mtxDone);
            m_bDone = true;
        }
        m_cvDone.notify_all();
    }

    bool WaitFor(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mtxDone);
        return m_cvDone.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)), [this] { return m_bDone; });
    }

private:
    std::mutex m_mtxDone;
    std::condition_variable m_cvDone;
    bool m_bDone = false;
};
}

enum class ETerminalRenderCommand { ATTACH, DETACH, RESIZE, VISIBLE, FOCUS, WAKE, SCROLL, REFRESH_RATE, FONT, THEME, FORGET_SOURCE };

struct TerminalRenderCommand
{
    ETerminalRenderCommand eType = ETerminalRenderCommand::WAKE;
    int iViewId = 0;
    ITerminalFrameSource* pSource = nullptr;
    TerminalNativeHandle handle;
    QSize size;
    qreal dDevicePixelRatio = 1.0;
    bool bValue = false;
    double dPixels = 0.0;
    double dRefreshRateHz = 0.0;
    qint64 llTimeNs = 0;
    std::optional<QFont> optFont;
    TerminalTheme theme;
    std::shared_ptr<DetachAck> spAck;
};

struct TerminalRenderShared
{
    TerminalViewer::Channel<TerminalRenderCommand> channel;
    std::atomic<bool> bStopRequested{ false };
    std::atomic<bool> bStatsEnabled{ false };
    std::atomic<int> iStrategy{ int(ETerminalRenderStrategy::CPU) };   // GetActiveStrategy
    std::atomic<bool> bGpuFailed{ false };   // GPU 를 쓸 수 없게 되었다(시작 실패·장치 잃음). 이 프로세스에서는 다시 시도하지 않는다
    TerminalGpuStrategyFactory funcGpuFactory;   // Start 전에만 바꾼다(빈 함수 = 기본)
    mutable std::mutex mtxGpuError;
    QString strGpuError;
};

namespace
{
// 처리하지 못한 명령의 뒷정리: 붙이려던 핸들은 놓고, 분리를 기다리는 쪽은 깨운다.
void DiscardCommand(TerminalRenderCommand& command)
{
    if (command.eType == ETerminalRenderCommand::ATTACH)
    {
        ReleaseHandle(command.handle);
    }
    if (command.spAck)
    {
        command.spAck->Signal();
    }
}

void DiscardPending(TerminalViewer::Channel<TerminalRenderCommand>& channel)
{
    TerminalRenderCommand command;
    while (channel.TryPop(command))
    {
        DiscardCommand(command);
    }
}
}

////////////////////////////////////////////////////////////////////////////////
// WorkerImpl
////////////////////////////////////////////////////////////////////////////////
class TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl
{
public:
    struct View
    {
        ITerminalFrameSource* pSource = nullptr;
        QSize size;
        qreal dDevicePixelRatio = 1.0;
        bool bVisible = true;
        bool bFocused = false;
        std::shared_ptr<GlyphCache> spCache;
        DrawListBuilder builder;
        ScrollAnimator animator;
        TerminalFrame frame;
        DrawList list;
        int iPendingLines = 0;              // ScrollPixels 를 처리하다 넘은 줄. 다음 프레임에 ScrollViewport
        bool bPendingExecute = false;       // 이전 Execute 가 bNotReady: 같은 목록으로 다시
        bool bRebuildOnRetry = false;       // 다시 그리기를 기다리는 동안 새 상태(Wake·스크롤)가 왔다: 옛 목록 대신 다시 캡처한다
        TerminalFrameStats pendingStats;
        qint64 llWakeNs = 0;                // 아직 그리지 않은 첫 Wake 시각(측정용)
        bool bHasBuilt = false;             // 아래 값으로 만든 목록이 있다
        int iBuiltOffsetPx = 0;
        bool bBuiltBlinkOn = true;
        bool bBuiltFocused = false;
        bool bAtlasOverflowLogged = false;
        bool bSurfaceLost = false;          // surface 를 잃었다: GUI 가 새 핸들로 다시 붙일 때까지 그리지 않는다
    };

    bool IsStopping() const;
    void Run();
    void Shutdown();
    void Apply(TerminalRenderCommand& command);
    void AttachView(TerminalRenderCommand& command);
    void DetachView(int viewId);
    void ResizeView(int viewId, const QSize& devicePixelSize, qreal devicePixelRatio);
    void SetVisible(int viewId, bool visible);
    void SetFocus(int viewId, bool focused);
    void Wake(int viewId, qint64 wakeNs);
    void ScrollPixels(int viewId, double pixels);
    void SetRefreshRate(int viewId, double hz);
    void SetFont(const QFont& newFont);
    void SetTheme(const TerminalTheme& newTheme);
    void UpdateVisibility(int viewId, const View& view);
    std::shared_ptr<GlyphCache> CacheFor(qreal devicePixelRatio);
    void PruneCaches();
    void RenderDueViews();
    void RenderView(int viewId, View& view);
    void Capture(View& view, bool full);
    void Present(int viewId, View& view, qint64 nowNs, TerminalFrameStats stats);
    void FinishFrame(int viewId, View& view, qint64 nowNs);
    bool EnsureGpu();
    void MarkGpuUnusable(const QString& reason);
    void FallBackToCpu(const QString& reason);
    void DropGpu();

    TerminalRenderThreadWorker* pWorker = nullptr;
    std::atomic<bool> doRunThread{ false };
    TerminalRenderShared* pShared = nullptr;
    ETerminalRendererMode eMode = ETerminalRendererMode::AUTO;   // AUTO 면 네이티브 핸들을 가진 화면을 GPU 로 그린다
    QFont font = DefaultFont();
    TerminalTheme theme;

    // 아래는 DoWork 동안 렌더 스레드만 쓴다.
    std::unique_ptr<TerminalRenderer> upRenderer;
    FrameScheduler scheduler;
    std::map<int, View> mapViews;
    std::map<qreal, std::shared_ptr<GlyphCache>> mapCaches;   // 배율별로 화면끼리 공유
    // 공급원(= 그 위젯, 다시 붙여도 같다)별로 표시 없이 잇달아 surface 를 잃은 횟수. 표시하면, 공급원이 놓이면 지운다
    std::unordered_map<ITerminalFrameSource*, int> hashSurfaceLosses;
};

bool TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::IsStopping() const
{
    return !doRunThread || (pShared && pShared->bStopRequested);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::Run()
{
    // TerminalRenderer 와 VulkanContext 는 렌더 스레드에서 만들고 쓰고 지운다. GPU 는 처음 GPU 화면이 붙을 때 만든다.
    upRenderer = std::make_unique<TerminalRenderer>();
    upRenderer->SetGpuFactory(pShared->funcGpuFactory);

    TerminalRenderCommand command;
#ifdef Q_OS_WIN
    FineTimerResolution timerResolution;
#endif
    while (!IsStopping())
    {
        // 1) 쌓인 명령을 모두 반영한다. 같은 화면의 Wake 는 스케줄러에서 하나로 합쳐진다.
        while (!IsStopping() && pShared->channel.TryPop(command))
        {
            Apply(command);
        }
        if (IsStopping())
        {
            break;
        }

        // 2) 그릴 화면을 차례로 그린다(깨어난 시점의 최신 상태 하나만).
        RenderDueViews();
        if (IsStopping())
        {
            break;
        }

        // 3) 다음 할 일(애니메이션·깜빡임·재시도 마감)까지, 마감이 없으면 명령이 올 때까지 기다린다.
        const std::optional<qint64> optDeadline = scheduler.NextDeadline(NowNs());
#ifdef Q_OS_WIN
        timerResolution.SetFine(optDeadline && *optDeadline - NowNs() < kFineTimerDeadlineNs);
#endif
        const bool bReceived = optDeadline ? pShared->channel.PopUntil(command, ToTimePoint(*optDeadline))
                                           : pShared->channel.Pop(command);
        if (bReceived)
        {
            Apply(command);
        }
    }
    Shutdown();
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::Shutdown()
{
    for (auto& [iViewId, view] : mapViews)
    {
        upRenderer->DetachView(iViewId);       // Strategy 가 핸들을 놓는다(GPU: surface 를 지운 뒤)
        emit pWorker->ViewDetached(iViewId);   // 멈출 때 남은 화면도 더는 공급원을 쓰지 않는다
    }
    mapViews.clear();
    scheduler = FrameScheduler();
    mapCaches.clear();
    upRenderer.reset();
    DiscardPending(pShared->channel);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::Apply(TerminalRenderCommand& command)
{
    switch (command.eType)
    {
    case ETerminalRenderCommand::ATTACH:
        AttachView(command);
        break;
    case ETerminalRenderCommand::DETACH:
        DetachView(command.iViewId);
        if (command.spAck)
        {
            command.spAck->Signal();
        }
        // 렌더 스레드가 그 화면을 다 뗐다(핸들을 놓았고 공급원을 더 쓰지 않는다). 없던 화면이어도 알린다.
        emit pWorker->ViewDetached(command.iViewId);
        break;
    case ETerminalRenderCommand::RESIZE:
        ResizeView(command.iViewId, command.size, command.dDevicePixelRatio);
        break;
    case ETerminalRenderCommand::VISIBLE:
        SetVisible(command.iViewId, command.bValue);
        break;
    case ETerminalRenderCommand::FOCUS:
        SetFocus(command.iViewId, command.bValue);
        break;
    case ETerminalRenderCommand::WAKE:
        Wake(command.iViewId, command.llTimeNs);
        break;
    case ETerminalRenderCommand::SCROLL:
        ScrollPixels(command.iViewId, command.dPixels);
        break;
    case ETerminalRenderCommand::REFRESH_RATE:
        SetRefreshRate(command.iViewId, command.dRefreshRateHz);
        break;
    case ETerminalRenderCommand::FONT:
        if (command.optFont)
        {
            SetFont(*command.optFont);
        }
        break;
    case ETerminalRenderCommand::THEME:
        SetTheme(command.theme);
        break;
    case ETerminalRenderCommand::FORGET_SOURCE:
        hashSurfaceLosses.erase(command.pSource);   // 주인이 놓을 공급원: 같은 주소의 새 공급원이 옛 셈을 이어받지 않게
        break;
    }
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::AttachView(TerminalRenderCommand& command)
{
    const int iViewId = command.iViewId;
    if (!command.pSource || mapViews.count(iViewId))
    {
        ReleaseHandle(command.handle);
        return;
    }
    // AUTO 에서 네이티브 핸들을 가진 화면은 GPU 로 그린다. 처음 그런 화면이 붙을 때 GPU 를 만든다(앱 시작 때 만들지 않는다).
    const bool bGpuView = eMode == ETerminalRendererMode::AUTO && command.handle.pHandle != nullptr;
    if (bGpuView && !EnsureGpu())
    {
        ReleaseHandle(command.handle);   // Strategy 에 넘기지 못한 핸들은 렌더 스레드가 놓는다
        emit pWorker->ViewNeedsCpuFallback(iViewId);
        return;
    }
    View& view = mapViews[iViewId];
    view.pSource = command.pSource;
    view.size = command.size;
    view.dDevicePixelRatio = command.dDevicePixelRatio > 0 ? command.dDevicePixelRatio : 1.0;
    view.spCache = CacheFor(view.dDevicePixelRatio);
    // 핸들은 Strategy 가 맡는다(실패해도 이미 놓았다). GPU 는 surface 를 못 만들거나 표시할 수 없으면 거짓 → 그 화면만 CPU 로(스펙 §11)
    const ETerminalRenderStrategy eStrategy = bGpuView ? ETerminalRenderStrategy::GPU : ETerminalRenderStrategy::CPU;
    if (!upRenderer->AttachView(iViewId, command.handle, view.size, eStrategy))
    {
        mapViews.erase(iViewId);
        PruneCaches();
        emit pWorker->ViewNeedsCpuFallback(iViewId);
        return;
    }
    scheduler.AddView(iViewId);
    UpdateVisibility(iViewId, view);
}

bool TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::EnsureGpu()
{
    if (upRenderer->HasStrategy(ETerminalRenderStrategy::GPU))
    {
        return true;
    }
    if (pShared->bGpuFailed)
    {
        return false;   // 이 프로세스에서는 다시 시도하지 않는다(다음 실행에서 다시 시도, 스펙 §11)
    }
    upRenderer->SetStrategy(ETerminalRenderStrategy::GPU);
    if (upRenderer->HasStrategy(ETerminalRenderStrategy::GPU))
    {
        pShared->iStrategy = int(ETerminalRenderStrategy::GPU);
        return true;
    }
    FallBackToCpu(upRenderer->GetGpuError());
    return false;
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::MarkGpuUnusable(const QString& reason)
{
    // 신호를 내기 전에 상태부터 바꾼다: 받는 쪽(새 위젯을 만드는 패널)이 GetActiveStrategy·GetGpuError 로 CPU 를 본다.
    pShared->bGpuFailed = true;
    {
        std::lock_guard<std::mutex> lock(pShared->mtxGpuError);
        pShared->strGpuError = reason;
    }
    pShared->iStrategy = int(ETerminalRenderStrategy::CPU);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::FallBackToCpu(const QString& reason)
{
    MarkGpuUnusable(reason);
    emit pWorker->StrategyChanged(ETerminalRenderStrategy::CPU);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::DropGpu()
{
    // 장치를 잃었다(또는 장치 메모리 부족): GPU 자원을 모두 버리고 GPU 화면을 모두 CPU 위젯으로 바꿔 달라고 한다(스펙 §11).
    // 버린 화면의 surface 는 Strategy 가 지우고 핸들을 놓았다. GUI 가 떼면 ViewDetached 가 온다.
    qWarning("TerminalRenderThread: GPU device lost; drawing every terminal view with CPU");
    const std::vector<int> vecDropped = upRenderer->DropGpu();
    for (const int iViewId : vecDropped)
    {
        scheduler.RemoveView(iViewId);
        mapViews.erase(iViewId);
    }
    PruneCaches();
    MarkGpuUnusable(QStringLiteral("Vulkan 장치를 잃었다(DEVICE_LOST, 장치 메모리 부족 또는 알 수 없는 Vulkan 오류)"));
    for (const int iViewId : vecDropped)
    {
        emit pWorker->ViewNeedsCpuFallback(iViewId);
    }
    emit pWorker->StrategyChanged(ETerminalRenderStrategy::CPU);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::DetachView(int viewId)
{
    auto it = mapViews.find(viewId);
    if (it == mapViews.end())
    {
        return;
    }
    upRenderer->DetachView(viewId);   // Strategy 가 핸들을 놓는다(GPU: surface 를 지운 뒤)
    scheduler.RemoveView(viewId);
    mapViews.erase(it);
    PruneCaches();
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::ResizeView(int viewId, const QSize& devicePixelSize, qreal devicePixelRatio)
{
    auto it = mapViews.find(viewId);
    if (it == mapViews.end())
    {
        return;
    }
    View& view = it->second;
    const qreal dRatio = devicePixelRatio > 0 ? devicePixelRatio : 1.0;
    if (devicePixelSize == view.size && dRatio == view.dDevicePixelRatio)
    {
        return;
    }
    if (dRatio != view.dDevicePixelRatio)
    {
        // 배율이 바뀌면 그 배율의 글리프 캐시로 바꾸고, 셀 높이가 달라지므로 스크롤 오프셋을 처음부터 다시 잰다.
        view.dDevicePixelRatio = dRatio;
        view.spCache = CacheFor(dRatio);
        view.animator.Reset();
        PruneCaches();
    }
    view.size = devicePixelSize;
    upRenderer->ResizeView(viewId, devicePixelSize);
    view.builder.Invalidate();
    view.bPendingExecute = false;
    scheduler.OnConfigChanged(viewId);
    UpdateVisibility(viewId, view);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::SetVisible(int viewId, bool visible)
{
    auto it = mapViews.find(viewId);
    if (it == mapViews.end())
    {
        return;
    }
    View& view = it->second;
    if (visible && !view.bVisible)
    {
        // 다시 보이면 전체를 다시 그린다.
        view.builder.Invalidate();
        view.bPendingExecute = false;
    }
    view.bVisible = visible;
    UpdateVisibility(viewId, view);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::SetFocus(int viewId, bool focused)
{
    auto it = mapViews.find(viewId);
    if (it == mapViews.end())
    {
        return;
    }
    it->second.bFocused = focused;
    scheduler.OnFocus(viewId, focused);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::Wake(int viewId, qint64 wakeNs)
{
    auto it = mapViews.find(viewId);
    if (it == mapViews.end())
    {
        return;
    }
    View& view = it->second;
    if (view.llWakeNs == 0)
    {
        view.llWakeNs = wakeNs;
    }
    view.bRebuildOnRetry = view.bRebuildOnRetry || view.bPendingExecute;
    scheduler.OnWake(viewId);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::ScrollPixels(int viewId, double pixels)
{
    auto it = mapViews.find(viewId);
    if (it == mapViews.end())
    {
        return;
    }
    View& view = it->second;
    // 애니메이터의 시각을 지금으로 맞춘 뒤 목표를 옮긴다(오래 쉬다 온 입력이 한 번에 뛰지 않게).
    view.iPendingLines += view.animator.Advance(NowNs(), view.spCache->GetMetrics().iCellHeight);
    view.animator.AddPixels(pixels);
    view.bRebuildOnRetry = view.bRebuildOnRetry || view.bPendingExecute;
    scheduler.OnWake(viewId);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::SetRefreshRate(int viewId, double hz)
{
    // 주사율은 GUI 스레드가 QScreen::refreshRate() 로 얻어 넘긴다. 렌더 스레드는 화면에 묻지 않는다(스펙 §5).
    if (!mapViews.count(viewId) || !(hz > 0.0))
    {
        return;
    }
    scheduler.SetFrameInterval(viewId, qint64(std::llround(kNsPerSecond / hz)));
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::SetFont(const QFont& newFont)
{
    if (newFont == font)
    {
        return;
    }
    font = newFont;
    std::vector<const GlyphCache*> vecOld;
    for (auto& [dRatio, spCache] : mapCaches)
    {
        vecOld.push_back(spCache.get());
        spCache = std::make_shared<GlyphCache>(font, dRatio);
    }
    for (auto& [iViewId, view] : mapViews)
    {
        view.spCache = CacheFor(view.dDevicePixelRatio);
        view.builder.Invalidate();
        view.animator.Reset();
        view.bPendingExecute = false;
        scheduler.OnConfigChanged(iViewId);
    }
    // 옛 캐시는 이제 지워졌다. 그 아틀라스 텍스처도 지운다(새 캐시로 그리기 전에).
    for (const GlyphCache* pOld : vecOld)
    {
        upRenderer->ForgetGlyphCache(pOld);
    }
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::SetTheme(const TerminalTheme& newTheme)
{
    if (newTheme == theme)
    {
        return;
    }
    theme = newTheme;
    for (auto& [iViewId, view] : mapViews)
    {
        view.builder.Invalidate();
        view.bPendingExecute = false;
        scheduler.OnConfigChanged(iViewId);
    }
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::UpdateVisibility(int viewId, const View& view)
{
    // 크기가 0 인 화면과 surface 를 잃은 화면은 숨은 화면처럼 아예 그리지 않는다(정상 크기로 돌아오면 전체를 다시 그린다).
    scheduler.OnVisible(viewId, view.bVisible && !view.size.isEmpty() && !view.bSurfaceLost);
}

std::shared_ptr<GlyphCache> TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::CacheFor(qreal devicePixelRatio)
{
    std::shared_ptr<GlyphCache>& spCache = mapCaches[devicePixelRatio];
    if (!spCache)
    {
        spCache = std::make_shared<GlyphCache>(font, devicePixelRatio);
    }
    return spCache;
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::PruneCaches()
{
    for (auto it = mapCaches.begin(); it != mapCaches.end();)
    {
        if (it->second.use_count() > 1)
        {
            ++it;
            continue;
        }
        upRenderer->ForgetGlyphCache(it->second.get());   // GPU: 그 캐시의 아틀라스 텍스처를 지운다
        it = mapCaches.erase(it);
    }
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::RenderDueViews()
{
    for (const int iViewId : scheduler.CollectDueViews(NowNs()))
    {
        if (IsStopping())
        {
            return;
        }
        auto it = mapViews.find(iViewId);
        if (it == mapViews.end())
        {
            scheduler.RemoveView(iViewId);
            continue;
        }
        RenderView(iViewId, it->second);
    }
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::Capture(View& view, bool full)
{
    TerminalCaptureRequest request;
    request.iOverscanAbove = kOverscanAbove;
    request.iOverscanBelow = kOverscanBelow;
    request.bFull = full;
    view.pSource->CaptureFrame(request, view.frame);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::RenderView(int viewId, View& view)
{
    const qint64 llNow = NowNs();
    if (!upRenderer->IsReady(viewId))
    {
        scheduler.OnNotReady(viewId);
        return;
    }
    // 이전 Execute 가 bNotReady 였으면 그 목록은 아직 표시되지 않았다: 다시 만들더라도 이번에는 바뀐 것이 없어도 표시한다.
    bool bMustPresent = false;
    if (view.bPendingExecute)
    {
        if (view.list.uAtlasGeneration != view.spCache->GetGeneration())
        {
            // 그사이 같은 캐시를 쓰는 다른 화면이 아틀라스를 비웠다: 목록의 글리프 자리가 틀리므로 처음부터 다시 만든다.
            view.builder.Invalidate();
            view.bHasBuilt = false;
        }
        else if (!view.bRebuildOnRetry && view.bFocused == view.bBuiltFocused && scheduler.IsBlinkOn(viewId, llNow) == view.bBuiltBlinkOn)
        {
            // 기다리는 동안 바뀐 것이 없다: 캡처 없이 같은 목록으로 다시 그린다.
            Present(viewId, view, llNow, view.pendingStats);
            return;
        }
        // 기다리는 동안 새 상태(Wake·스크롤·포커스·깜빡임 전환)가 생겼다: 옛 목록을 버리고 지금 상태를 캡처해 다시 만든다(스펙 §5
        // "깨어난 시점의 최신 상태"). 옛 목록을 내고 OnRendered 가 깨움을 지우면 새 상태가 다음 일까지 화면에 나오지 않는다.
        // 행 캐시는 마지막 빌드(버린 목록)와 맞으므로 이어서 부분 캡처해도 된다.
        view.bPendingExecute = false;
        bMustPresent = true;
    }

    TerminalFrameStats stats;
    stats.iViewId = viewId;
    stats.llWakeNs = view.llWakeNs;
    GlyphCache& cache = *view.spCache;

    // 1) 스크롤 애니메이션. 줄 경계를 넘었으면 캡처 전에 뷰포트를 옮겨 이번 프레임이 옮겨진 줄을 그리게 한다.
    const int iLines = view.iPendingLines + view.animator.Advance(llNow, cache.GetMetrics().iCellHeight);
    view.iPendingLines = 0;
    if (iLines != 0 && view.pSource->ScrollViewport(iLines) != iLines)
    {
        view.animator.StopAtEdge();   // 스크롤백 끝에 닿음
    }

    // 2) 캡처
    Capture(view, view.builder.NeedsFullFrame());
    if (view.frame.eDirty != EFrameDirty::NONE)
    {
        scheduler.OnActivity(viewId, llNow);   // 출력이 있으면 커서를 보이게 하고 깜빡임 주기를 다시 시작
    }
    if (view.animator.GetOffsetPx() != 0 && !HasRowBelow(view.frame))
    {
        view.animator.StopAtEdge();   // 반쯤 보일 아래 줄이 없다
    }
    const int iOffsetPx = view.animator.GetOffsetPx();
    const bool bBlinkOn = scheduler.IsBlinkOn(viewId, llNow);
    if (!bMustPresent && view.bHasBuilt && view.frame.eDirty == EFrameDirty::NONE && iOffsetPx == view.iBuiltOffsetPx &&
        bBlinkOn == view.bBuiltBlinkOn && view.bFocused == view.bBuiltFocused)
    {
        FinishFrame(viewId, view, llNow);   // 바뀐 것이 없으면 그리지 않는다(표시 0회)
        return;
    }
    stats.llCaptureDoneNs = NowNs();

    // 3) 그리기 목록. 행 캐시를 쓸 수 없으면 바로 FULL 로 한 번 더 캡처·빌드한다.
    DrawListBuildStats buildStats;
    EBuildResult eResult = view.builder.Build(view.frame, theme, view.bFocused, bBlinkOn, iOffsetPx, cache, view.list, buildStats);
    if (eResult == EBuildResult::NEED_FULL_FRAME)
    {
        Capture(view, true);
        stats.llCaptureDoneNs = NowNs();
        eResult = view.builder.Build(view.frame, theme, view.bFocused, bBlinkOn, iOffsetPx, cache, view.list, buildStats);
    }
    if (eResult == EBuildResult::NEED_FULL_FRAME)
    {
        // FULL 로도 안 된다(화면 하나에 아틀라스보다 많은 글리프). 되풀이하지 않고 이 프레임을 건너뛴 뒤 다음 차례에 다시 해 본다.
        if (!view.bAtlasOverflowLogged)
        {
            qWarning("TerminalRenderThread: view %d needs more glyphs than the atlas can hold; frame skipped", viewId);
            view.bAtlasOverflowLogged = true;
        }
        FinishFrame(viewId, view, llNow);
        return;
    }
    if (bMustPresent)
    {
        // 버린 목록의 바뀐 줄도 이 목록이 그려야 한다: 부분 합성(vecDirtyRows)에 기대지 않게 전체 다시 그리기로 표시한다
        // (GPU 는 매 프레임 전체를 그리므로 영향이 없다).
        view.list.bFullRedraw = true;
    }
    stats.llBuildDoneNs = NowNs();
    stats.iQuadCount = int(view.list.vecQuads.size());
    stats.iRebuiltRows = buildStats.iRebuiltRows;
    view.bRebuildOnRetry = false;
    view.bHasBuilt = true;
    view.iBuiltOffsetPx = iOffsetPx;
    view.bBuiltBlinkOn = bBlinkOn;
    view.bBuiltFocused = view.bFocused;

    // 4) 실행(CPU 합성 / GPU 제출·표시)
    Present(viewId, view, llNow, stats);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::Present(int viewId, View& view, qint64 nowNs, TerminalFrameStats stats)
{
    TerminalRenderInput input;
    input.iViewId = viewId;
    input.pDrawList = &view.list;
    input.pGlyphCache = view.spCache.get();
    TerminalRenderOutput output;
    stats.llExecuteStartNs = NowNs();
    const bool bExecuted = upRenderer->Execute(input, output);
    stats.llExecuteEndNs = NowNs();
    if (output.bDeviceLost)
    {
        DropGpu();   // 이 화면(view)도 지워졌다. 더 쓰지 않는다
        return;
    }
    if (output.bNotReady)
    {
        view.bPendingExecute = true;
        ++stats.iNotReadyRetries;
        view.pendingStats = stats;
        scheduler.OnNotReady(viewId);
        return;
    }
    view.bPendingExecute = false;
    stats.llPresentCallNs = stats.llExecuteEndNs;
    stats.iPresentMode = output.iPresentMode;
    if (output.bSurfaceLost)
    {
        // GUI 가 새 핸들로 다시 붙일 때까지(새 id) 이 화면은 그리지 않는다.
        view.bSurfaceLost = true;
        FinishFrame(viewId, view, nowNs);
        UpdateVisibility(viewId, view);
        // 다시 붙여도 또 잃으면(Windows 는 HWND 를 다시 쓰므로 NATIVE_WINDOW_IN_USE 가 이어질 수 있다) 끝없이 되풀이하지 않는다:
        // 표시 없이 kMaxConsecutiveSurfaceLosses 번 잇달아 잃으면 다시 붙이라고 하지 않고 그 화면만 CPU 로 바꿔 달라고 한다.
        const int iLosses = ++hashSurfaceLosses[view.pSource];
        if (iLosses >= kMaxConsecutiveSurfaceLosses)
        {
            hashSurfaceLosses.erase(view.pSource);
            qWarning("TerminalRenderThread: view %d lost its surface %d times in a row without presenting; drawing it with CPU", viewId,
                     iLosses);
            emit pWorker->ViewNeedsCpuFallback(viewId);
            return;
        }
        emit pWorker->ViewSurfaceLost(viewId);
        return;
    }
    if (output.bPresented && !hashSurfaceLosses.empty())
    {
        hashSurfaceLosses.erase(view.pSource);   // 표시했다: 잇달아 잃은 셈을 다시 시작한다
    }
    if (bExecuted && output.bPresented)
    {
        if (!output.imgPatch.isNull())
        {
            // CPU 화면의 조각. QImage 는 암시적 공유라 큐 신호로 넘겨도 픽셀을 복사하지 않는다.
            emit pWorker->FrameReady(viewId, output.imgPatch, output.rectPatch);
        }
        if (pShared->bStatsEnabled)
        {
            emit pWorker->FrameStats(stats);
        }
    }
    FinishFrame(viewId, view, nowNs);
}

void TerminalRenderThreadWorker::TerminalRenderThreadWorkerImpl::FinishFrame(int viewId, View& view, qint64 nowNs)
{
    scheduler.OnRendered(viewId, nowNs, view.animator.IsAnimating(), CursorBlinks(view.frame));
    view.llWakeNs = 0;
}

////////////////////////////////////////////////////////////////////////////////
// Worker
////////////////////////////////////////////////////////////////////////////////
TerminalRenderThreadWorker::TerminalRenderThreadWorker(QObject* parent)
    : QObject(parent)
    , m_pimpl(std::make_unique<TerminalRenderThreadWorkerImpl>())
{
    m_pimpl->pWorker = this;
}

TerminalRenderThreadWorker::~TerminalRenderThreadWorker() = default;

void TerminalRenderThreadWorker::SetContext(TerminalRenderShared* pShared, ETerminalRendererMode mode, const QFont& font, const TerminalTheme& theme)
{
    m_pimpl->pShared = pShared;
    m_pimpl->eMode = mode;
    m_pimpl->font = font;
    m_pimpl->theme = theme;
}

void TerminalRenderThreadWorker::Stop()
{
    // 아무 스레드에서나 부른다: 깃발과 채널만 건드린다.
    m_pimpl->doRunThread = false;
    if (m_pimpl->pShared)
    {
        m_pimpl->pShared->bStopRequested = true;
        m_pimpl->pShared->channel.Stop();
    }
}

bool TerminalRenderThreadWorker::IsThreadAlive() const
{
    return m_pimpl->doRunThread;
}

void TerminalRenderThreadWorker::DoWork()
{
    if (!m_pimpl->pShared)
    {
        emit WorkFinished(false);
        return;
    }
    m_pimpl->doRunThread = true;
    m_pimpl->Run();
    m_pimpl->doRunThread = false;
    emit WorkFinished(true);
}

////////////////////////////////////////////////////////////////////////////////
// ThreadImpl
////////////////////////////////////////////////////////////////////////////////
class TerminalRenderThread::TerminalRenderThreadImpl
{
public:
    QThread workerThread;
    std::unique_ptr<TerminalRenderThreadWorker> worker;   // 기본 삭제자: wait() 이후에만 reset
    TerminalRenderShared shared;
    ETerminalRendererMode eMode = ETerminalRendererMode::AUTO;
    std::atomic<int> iLastViewId{ 0 };
    std::atomic<bool> bRunning{ false };

    std::mutex mtxState;                     // 아래 다섯
    std::unordered_set<int> usetAttached;     // FrameReady 를 내보낼 화면. 떼어 낸 화면의 늦은 조각은 버린다
    struct SourceUse
    {
        int iViews = 0;   // 아직 떨어지지 않은 화면 수(AttachView ~ ViewDetached)
        int iHolds = 0;   // RetainSource 수(살아 있는 위젯)
    };
    std::unordered_map<int, ITerminalFrameSource*> hashViewSources;     // 붙인 화면 → 공급원(그 id 의 ViewDetached 까지)
    std::unordered_map<ITerminalFrameSource*, SourceUse> hashSourceUses;   // 공급원 → 쓰는 쪽 수
    QFont font = DefaultFont();              // 다시 시작할 때 워커에 넘길 마지막 글꼴·테마
    TerminalTheme theme;

    ~TerminalRenderThreadImpl()
    {
        stopWorker();
    }

    // AUTO 는 GPU 를 쓸 수 없게 될 때까지 GPU 로 알린다(새 화면을 GPU 위젯으로 만들게). 단 GPU 를 끈 빌드, 네이티브 화면
    // (CAMetalLayer/HWND)을 만들 수 없는 QPA, Vulkan 라이브러리를 열 수 없는 환경(OCEAN_VK_DISABLE=1 포함)이면 처음부터 CPU 다:
    // GPU 위젯을 만들었다 바로 바꿔 끼우면 그 사이 모든 조상 위젯이 네이티브가 되어 세션 내내 남는다.
    // 라이브러리는 AUTO 이고 네이티브 화면을 만들 수 있을 때만 열고 닫아 본다(인스턴스는 만들지 않는다. GUI 스레드에서 부른다).
    // GPU Strategy 를 넣어 준 테스트·도구는 GPU 다.
    void UpdateInitialStrategy()
    {
        const auto funcGpuPossible = [this]()
        {
            return bool(shared.funcGpuFactory) || (IsNativeSurfaceSupported() && IsGpuLibraryAvailable());
        };
        const bool bGpu = eMode == ETerminalRendererMode::AUTO && !shared.bGpuFailed && funcGpuPossible();
        shared.iStrategy = int(bGpu ? ETerminalRenderStrategy::GPU : ETerminalRenderStrategy::CPU);
    }

    bool IsAttached(int viewId)
    {
        std::lock_guard<std::mutex> lock(mtxState);
        return usetAttached.count(viewId) > 0;
    }

    void Forget(int viewId)
    {
        std::lock_guard<std::mutex> lock(mtxState);
        usetAttached.erase(viewId);
    }

    void TrackSource(int viewId, ITerminalFrameSource* pSource)
    {
        if (!pSource)
        {
            return;   // 렌더 스레드가 붙이지 않는다
        }
        std::lock_guard<std::mutex> lock(mtxState);
        hashViewSources[viewId] = pSource;
        ++hashSourceUses[pSource].iViews;
    }

    // 쓰는 쪽이 하나도 없으면 지우고 참(SourceReleased 를 낼 때다). mtxState 를 잡고 부른다
    bool EraseIfUnusedLocked(std::unordered_map<ITerminalFrameSource*, SourceUse>::iterator itSource)
    {
        if (itSource->second.iViews > 0 || itSource->second.iHolds > 0)
        {
            return false;
        }
        hashSourceUses.erase(itSource);
        return true;
    }

    // 그 화면이 다 떨어졌다. 그 공급원을 쓰는 쪽이 더 없으면 그 공급원을 돌려준다
    ITerminalFrameSource* ReleaseSourceOf(int viewId)
    {
        std::lock_guard<std::mutex> lock(mtxState);
        const auto itView = hashViewSources.find(viewId);
        if (itView == hashViewSources.end())
        {
            return nullptr;   // 이미 셌거나(반복된 ViewDetached) Stop 이 모두 셌다
        }
        ITerminalFrameSource* pSource = itView->second;
        hashViewSources.erase(itView);
        const auto itSource = hashSourceUses.find(pSource);
        if (itSource == hashSourceUses.end())
        {
            return nullptr;
        }
        --itSource->second.iViews;
        return EraseIfUnusedLocked(itSource) ? pSource : nullptr;
    }

    void HoldSource(ITerminalFrameSource* pSource)
    {
        std::lock_guard<std::mutex> lock(mtxState);
        ++hashSourceUses[pSource].iHolds;
    }

    // 잡은 것을 놓는다. 쓰는 쪽이 더 없으면 참
    bool UnholdSource(ITerminalFrameSource* pSource)
    {
        std::lock_guard<std::mutex> lock(mtxState);
        const auto itSource = hashSourceUses.find(pSource);
        if (itSource == hashSourceUses.end() || itSource->second.iHolds == 0)
        {
            return false;
        }
        --itSource->second.iHolds;
        return EraseIfUnusedLocked(itSource);
    }

    // Stop 이 끝났다: 렌더 스레드는 어느 공급원도 더 쓰지 않는다. 잡혀 있지 않은 공급원을 돌려준다(잡힌 것은 놓을 때 알린다)
    std::vector<ITerminalFrameSource*> ReleaseAllSources()
    {
        std::lock_guard<std::mutex> lock(mtxState);
        std::vector<ITerminalFrameSource*> vecSources;
        hashViewSources.clear();
        for (auto itSource = hashSourceUses.begin(); itSource != hashSourceUses.end();)
        {
            itSource->second.iViews = 0;
            if (itSource->second.iHolds > 0)
            {
                ++itSource;
                continue;
            }
            vecSources.push_back(itSource->first);
            itSource = hashSourceUses.erase(itSource);
        }
        return vecSources;
    }

    // 렌더 스레드가 그 공급원에 대해 기억한 것(surface 잃은 셈)을 지운다
    void ForgetSourceOnRenderThread(ITerminalFrameSource* pSource)
    {
        TerminalRenderCommand command;
        command.eType = ETerminalRenderCommand::FORGET_SOURCE;
        command.pSource = pSource;
        shared.channel.Push(std::move(command));
    }

    void connectWorker(TerminalRenderThread* q_ptr)
    {
        if (!worker)
        {
            return;
        }
        // 화면별 신호는 GUI 쪽에서 이미 뗀 화면이면 버린다(떼기 전에 렌더 스레드가 낸 늦은 신호).
        QObject::connect(worker.get(), &TerminalRenderThreadWorker::FrameReady, q_ptr,
                         [this, q_ptr](int viewId, const QImage& imgPatch, const QRect& rectPatch)
                         {
                             if (IsAttached(viewId))
                             {
                                 emit q_ptr->FrameReady(viewId, imgPatch, rectPatch);
                             }
                         });
        QObject::connect(worker.get(), &TerminalRenderThreadWorker::FrameStats, q_ptr,
                         [this, q_ptr](const TerminalFrameStats& stats)
                         {
                             if (IsAttached(stats.iViewId))
                             {
                                 emit q_ptr->FrameStats(stats);
                             }
                         });
        QObject::connect(worker.get(), &TerminalRenderThreadWorker::ViewNeedsCpuFallback, q_ptr,
                         [this, q_ptr](int viewId)
                         {
                             if (IsAttached(viewId))
                             {
                                 emit q_ptr->ViewNeedsCpuFallback(viewId);
                             }
                         });
        QObject::connect(worker.get(), &TerminalRenderThreadWorker::ViewSurfaceLost, q_ptr,
                         [this, q_ptr](int viewId)
                         {
                             if (IsAttached(viewId))
                             {
                                 emit q_ptr->ViewSurfaceLost(viewId);
                             }
                         });
        QObject::connect(worker.get(), &TerminalRenderThreadWorker::StrategyChanged, q_ptr, &TerminalRenderThread::StrategyChanged);
        QObject::connect(worker.get(), &TerminalRenderThreadWorker::ViewDetached, q_ptr,
                         [this, q_ptr](int viewId)
                         {
                             ITerminalFrameSource* pReleased = ReleaseSourceOf(viewId);
                             emit q_ptr->ViewDetached(viewId);
                             if (pReleased)
                             {
                                 ForgetSourceOnRenderThread(pReleased);
                                 emit q_ptr->SourceReleased(pReleased);
                             }
                         });
        // started 는 새 스레드에서 run() 전에 나온다. 직접 연결이라 DoWork 가 그 스레드에서 바로 돈다.
        // (큐로 부르면 Stop 이 DoWork 보다 먼저 오면 남은 호출이 다음 Start 때 두 번 돌 수 있다.)
        QObject::connect(&workerThread, &QThread::started, worker.get(), &TerminalRenderThreadWorker::DoWork, Qt::DirectConnection);
    }

    void stopWorker()
    {
        if (worker)
        {
            worker->Stop();
        }
        else
        {
            shared.bStopRequested = true;
            shared.channel.Stop();
        }
        if (workerThread.isRunning())
        {
            workerThread.quit();
            if (!workerThread.wait(kStopWaitMs))
            {
                workerThread.terminate();
                workerThread.wait();
            }
        }
        worker.reset();
        bRunning = false;
        // 스레드가 끝난 뒤 남은 명령: 붙이려던 핸들을 놓고 분리를 기다리는 쪽을 깨운다.
        DiscardPending(shared.channel);
        std::lock_guard<std::mutex> lock(mtxState);
        usetAttached.clear();
    }
};

////////////////////////////////////////////////////////////////////////////////
// Thread
////////////////////////////////////////////////////////////////////////////////
TerminalRenderThread::TerminalRenderThread(ETerminalRendererMode mode, QObject* parent)
    : QObject(parent)
    , m_pimpl(std::make_unique<TerminalRenderThreadImpl>())
{
    m_pimpl->eMode = mode;
    m_pimpl->UpdateInitialStrategy();
    // 렌더 스레드 -> GUI 스레드 큐 연결로 전달되는 타입
    qRegisterMetaType<TerminalFrameStats>("TerminalFrameStats");
    qRegisterMetaType<TerminalTheme>("TerminalTheme");
    qRegisterMetaType<ETerminalRenderStrategy>("ETerminalRenderStrategy");
    qRegisterMetaType<ITerminalFrameSource*>("ITerminalFrameSource*");
}

TerminalRenderThread::~TerminalRenderThread()
{
    Stop();
}

void TerminalRenderThread::Start()
{
    if (m_pimpl->workerThread.isRunning())
    {
        return;
    }
    // 스레드가 끝난(join 된) 뒤에만 채널을 다시 연다.
    m_pimpl->shared.channel.Restart();
    m_pimpl->shared.bStopRequested = false;
    if (!m_pimpl->worker)
    {
        m_pimpl->worker = std::make_unique<TerminalRenderThreadWorker>();
        {
            std::lock_guard<std::mutex> lock(m_pimpl->mtxState);
            m_pimpl->worker->SetContext(&m_pimpl->shared, m_pimpl->eMode, m_pimpl->font, m_pimpl->theme);
        }
        m_pimpl->connectWorker(this);
        m_pimpl->worker->moveToThread(&m_pimpl->workerThread);
    }
    m_pimpl->bRunning = true;
    m_pimpl->workerThread.start();
}

void TerminalRenderThread::Stop()
{
    m_pimpl->stopWorker();
    // 스레드가 끝났다: 잡혀 있지 않은 공급원을 모두 알린다(이 뒤에 오는 늦은 ViewDetached 는 다시 알리지 않는다).
    // 렌더 스레드의 기억(surface 잃은 셈)은 워커와 함께 사라졌다.
    for (ITerminalFrameSource* pSource : m_pimpl->ReleaseAllSources())
    {
        emit SourceReleased(pSource);
    }
}

void TerminalRenderThread::RetainSource(ITerminalFrameSource* pSource)
{
    if (pSource)
    {
        m_pimpl->HoldSource(pSource);
    }
}

void TerminalRenderThread::ReleaseSource(ITerminalFrameSource* pSource)
{
    if (pSource && m_pimpl->UnholdSource(pSource))
    {
        m_pimpl->ForgetSourceOnRenderThread(pSource);
        emit SourceReleased(pSource);
    }
}

void TerminalRenderThread::SetGpuStrategyFactory(TerminalGpuStrategyFactory factory)
{
    if (m_pimpl->workerThread.isRunning())
    {
        qWarning("TerminalRenderThread::SetGpuStrategyFactory: call before Start");
        return;
    }
    m_pimpl->shared.funcGpuFactory = std::move(factory);
    m_pimpl->UpdateInitialStrategy();
}

int TerminalRenderThread::AttachView(ITerminalFrameSource* pSource, const TerminalNativeHandle& handle, const QSize& devicePixelSize, qreal devicePixelRatio)
{
    const int iViewId = ++m_pimpl->iLastViewId;
    {
        std::lock_guard<std::mutex> lock(m_pimpl->mtxState);
        m_pimpl->usetAttached.insert(iViewId);
    }
    m_pimpl->TrackSource(iViewId, pSource);
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::ATTACH;
    command.iViewId = iViewId;
    command.pSource = pSource;
    command.handle = handle;
    command.size = devicePixelSize;
    command.dDevicePixelRatio = devicePixelRatio;
    m_pimpl->shared.channel.Push(std::move(command));
    return iViewId;
}

void TerminalRenderThread::DetachView(int viewId)
{
    m_pimpl->Forget(viewId);
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::DETACH;
    command.iViewId = viewId;
    m_pimpl->shared.channel.Push(std::move(command));
}

bool TerminalRenderThread::DetachViewAndWait(int viewId, int timeoutMs)
{
    m_pimpl->Forget(viewId);
    auto spAck = std::make_shared<DetachAck>();
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::DETACH;
    command.iViewId = viewId;
    command.spAck = spAck;
    m_pimpl->shared.channel.Push(std::move(command));
    // 넣은 뒤에 본다: Stop 은 bRunning 을 내린 뒤 남은 명령을 정리하므로 여기서 참이면 반드시 누군가 깨운다.
    if (!m_pimpl->bRunning)
    {
        return spAck->WaitFor(0);
    }
    return spAck->WaitFor(timeoutMs);
}

void TerminalRenderThread::ResizeView(int viewId, const QSize& devicePixelSize, qreal devicePixelRatio)
{
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::RESIZE;
    command.iViewId = viewId;
    command.size = devicePixelSize;
    command.dDevicePixelRatio = devicePixelRatio;
    m_pimpl->shared.channel.Push(std::move(command));
}

void TerminalRenderThread::SetVisible(int viewId, bool visible)
{
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::VISIBLE;
    command.iViewId = viewId;
    command.bValue = visible;
    m_pimpl->shared.channel.Push(std::move(command));
}

void TerminalRenderThread::SetFocus(int viewId, bool focused)
{
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::FOCUS;
    command.iViewId = viewId;
    command.bValue = focused;
    m_pimpl->shared.channel.Push(std::move(command));
}

void TerminalRenderThread::Wake(int viewId)
{
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::WAKE;
    command.iViewId = viewId;
    command.llTimeNs = NowNs();
    m_pimpl->shared.channel.Push(std::move(command));
}

void TerminalRenderThread::ScrollPixels(int viewId, double pixels)
{
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::SCROLL;
    command.iViewId = viewId;
    command.dPixels = pixels;
    m_pimpl->shared.channel.Push(std::move(command));
}

void TerminalRenderThread::SetRefreshRate(int viewId, double hz)
{
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::REFRESH_RATE;
    command.iViewId = viewId;
    command.dRefreshRateHz = hz;
    m_pimpl->shared.channel.Push(std::move(command));
}

void TerminalRenderThread::SetFont(const QFont& font)
{
    bool bChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_pimpl->mtxState);
        bChanged = m_pimpl->font != font;
        m_pimpl->font = font;
    }
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::FONT;
    command.optFont = font;
    m_pimpl->shared.channel.Push(std::move(command));
    if (bChanged)
    {
        emit CellLayoutChanged();
    }
}

void TerminalRenderThread::SetTheme(const TerminalTheme& theme)
{
    bool bPaddingChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_pimpl->mtxState);
        bPaddingChanged = m_pimpl->theme.iPaddingLogicalPx != theme.iPaddingLogicalPx;
        m_pimpl->theme = theme;
    }
    TerminalRenderCommand command;
    command.eType = ETerminalRenderCommand::THEME;
    command.theme = theme;
    m_pimpl->shared.channel.Push(std::move(command));
    if (bPaddingChanged)
    {
        emit CellLayoutChanged();
    }
}

void TerminalRenderThread::SetStatsEnabled(bool enabled)
{
    m_pimpl->shared.bStatsEnabled = enabled;
}

ETerminalRenderStrategy TerminalRenderThread::GetActiveStrategy() const
{
    return ETerminalRenderStrategy(m_pimpl->shared.iStrategy.load());
}

QString TerminalRenderThread::GetGpuError() const
{
    std::lock_guard<std::mutex> lock(m_pimpl->shared.mtxGpuError);
    return m_pimpl->shared.strGpuError;
}

QFont TerminalRenderThread::GetFont() const
{
    std::lock_guard<std::mutex> lock(m_pimpl->mtxState);
    return m_pimpl->font;
}

TerminalTheme TerminalRenderThread::GetTheme() const
{
    std::lock_guard<std::mutex> lock(m_pimpl->mtxState);
    return m_pimpl->theme;
}
