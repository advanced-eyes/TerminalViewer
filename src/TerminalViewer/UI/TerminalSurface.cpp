#include "TerminalViewer/UI/TerminalSurface.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalPlatform.h"
#include "TerminalViewer/Thread/TerminalRenderThread.h"
#include "TerminalViewer/UI/TerminalSurfaceNative.h"

#include <QExposeEvent>
#include <QGuiApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QWindow>

#include <algorithm>
#include <cstring>

namespace
{
constexpr int kDetachWaitMs = 500;       // 네이티브 창이 사라지기 직전 렌더 스레드를 기다리는 한도(스펙 §10)
constexpr double kMinRefreshHz = 24.0;   // 넘기는 주사율(ScreenRefreshRate)을 이 범위로 자른다
constexpr double kMaxRefreshHz = 360.0;
}

#if defined(Q_OS_WIN)
TerminalNativeHandle CreateNativeHandle(QWidget* pNativeWidget, qreal)
{
    TerminalNativeHandle handle;
    if (!pNativeWidget || !IsNativeSurfaceSupported())
        return handle;
    handle.pHandle = reinterpret_cast<void*>(pNativeWidget->winId());   // HWND. 창은 Qt 가 갖는다
    handle.funcRelease = [](void*) {};
    return handle;
}

void UpdateNativeScale(const TerminalNativeHandle&, qreal)
{
    // HWND 는 장치 픽셀 크기로 스왑체인을 만든다(currentExtent). 따로 맞출 배율이 없다.
}
#elif !defined(Q_OS_MACOS)
TerminalNativeHandle CreateNativeHandle(QWidget*, qreal)
{
    return TerminalNativeHandle();   // 이 플랫폼은 GPU 로 화면에 그리지 않는다(범위 밖)
}

void UpdateNativeScale(const TerminalNativeHandle&, qreal)
{
}
#endif

bool IsRenderVisible(bool widgetShown, bool topLevelExposed)
{
    return widgetShown && topLevelExposed;
}

bool UpdateTopLevelExposed(bool exposed, QEvent::Type eventType, bool windowExposed, bool windowMinimized)
{
    switch (eventType)
    {
    case QEvent::Expose:
        return windowExposed;
    case QEvent::Show:
        return true;   // 다시 보인 창: 드러나지 않았다고 다시 알릴 때까지 그린다
    case QEvent::WindowStateChange:
        return !windowMinimized;   // Dock 에서 돌아온 창: 위와 같다
    default:
        return exposed;
    }
}

bool IsTopLevelExposeVisible(const QString& platformName, bool regionEmpty, bool windowExposed)
{
    if (platformName == QLatin1String("windows"))
        return windowExposed;   // 창을 꽉 채운 네이티브 자식 창 때문에 영역이 비어도 드러나 있다(헤더 주석)
    return !regionEmpty && windowExposed;
}

TerminalSurface::TerminalSurface(TerminalRenderThread* pRenderThread, ITerminalFrameSource* pSource, bool useGpu, QWidget* parent)
    : QWidget(parent)
    , m_pRenderThread(pRenderThread)
    , m_pSource(pSource)
    , m_bGpu(useGpu)
    , m_iViewId(0)
    , m_pNativeHandle(nullptr)
    , m_dDevicePixelRatio(0.0)
    , m_sizeGrid()
    , m_imgFrame()
    , m_bShownOnce(false)
    , m_bShown(false)
    , m_bTopLevelExposed(true)
    , m_bRenderVisible(false)
    , m_bAttaching(false)
    , m_bFallbackRequested(false)
    , m_bReattachPending(false)
    , m_pSurfaceWindow()
    , m_pTopLevelWindow()
    , m_pScreenWindow()
    , m_connScreen()
    , m_metrics()
    , m_fontMetrics()
    , m_dMetricsRatio(0.0)
{
    setFocusPolicy(Qt::StrongFocus);
    if (m_bGpu)
    {
        // 렌더 스레드가 이 네이티브 창에 직접 표시한다. Qt 는 그리지 않는다.
        // 조상이 네이티브가 되는 Qt 기본 동작은 그대로 둔다(WA_DontCreateNativeAncestors 금지, 부록 A S1 (h)).
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_NoSystemBackground);
    }
    if (!m_pRenderThread)
        return;
    connect(m_pRenderThread, &TerminalRenderThread::FrameReady, this, &TerminalSurface::OnFrameReady);
    connect(m_pRenderThread, &TerminalRenderThread::ViewNeedsCpuFallback, this, &TerminalSurface::OnViewNeedsCpuFallback);
    connect(m_pRenderThread, &TerminalRenderThread::ViewSurfaceLost, this, &TerminalSurface::OnViewSurfaceLost);
    connect(m_pRenderThread, &TerminalRenderThread::CellLayoutChanged, this, &TerminalSurface::UpdateGrid);
    m_pRenderThread->RetainSource(m_pSource);   // 사는 동안 잡아 둔다: 떨어져 있어도 다시 붙을 수 있다(헤더의 공급원 규칙)
}

TerminalSurface::~TerminalSurface()
{
    if (m_pSurfaceWindow)
        m_pSurfaceWindow->removeEventFilter(this);
    if (m_pTopLevelWindow)
        m_pTopLevelWindow->removeEventFilter(this);
    if (!m_pRenderThread)
        return;
    if (m_iViewId != 0)
    {
        if (m_bGpu)
            m_pRenderThread->DetachViewAndWait(m_iViewId, kDetachWaitMs);   // QWidget 소멸자가 곧 네이티브 창을 지운다
        else
            m_pRenderThread->DetachView(m_iViewId);
    }
    // 마지막에 놓는다: 렌더 스레드도 더 쓰지 않으면 SourceReleased 가 지금 오고, 아니면 떼기를 마친 뒤 온다.
    // 이 뒤로 이 위젯은 공급원을 쓰지 않는다.
    m_pRenderThread->ReleaseSource(m_pSource);
}

int TerminalSurface::GetViewId() const
{
    return m_iViewId;
}

bool TerminalSurface::IsGpu() const
{
    return m_bGpu;
}

QSize TerminalSurface::GetGridSize() const
{
    const qreal dRatio = devicePixelRatioF();
    const QFont fontCell = m_pRenderThread ? m_pRenderThread->GetFont() : font();
    if (dRatio != m_dMetricsRatio || fontCell != m_fontMetrics)
    {
        m_metrics = CellMetrics::Compute(fontCell, dRatio);
        m_fontMetrics = fontCell;
        m_dMetricsRatio = dRatio;
    }
    const int iPaddingLogical = m_pRenderThread ? m_pRenderThread->GetTheme().iPaddingLogicalPx : TerminalTheme().iPaddingLogicalPx;
    return m_metrics.ComputeGrid(DevicePixelSize(), qRound(iPaddingLogical * dRatio));   // DrawListBuilder 와 같은 여백 반올림
}

QSize TerminalSurface::DevicePixelSize() const
{
    const qreal dRatio = devicePixelRatioF();
    return QSize(qRound(width() * dRatio), qRound(height() * dRatio));
}

QPaintEngine* TerminalSurface::paintEngine() const
{
    return m_bGpu ? nullptr : QWidget::paintEngine();
}

void TerminalSurface::paintEvent(QPaintEvent* event)
{
    if (m_bGpu || m_imgFrame.isNull())
        return;
    QPainter painter(this);
    const qreal dRatio = m_imgFrame.devicePixelRatio();
    const QRect rectExposed = event->rect();
    const QRectF rectfSource(rectExposed.x() * dRatio, rectExposed.y() * dRatio, rectExposed.width() * dRatio, rectExposed.height() * dRatio);
    painter.drawImage(QRectF(rectExposed), m_imgFrame, rectfSource);
}

void TerminalSurface::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    UpdateScale();
    SendResize();
    UpdateGrid();
}

void TerminalSurface::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_bShownOnce = true;
    m_bShown = true;
    if (m_iViewId == 0)
    {
        Attach();
        return;
    }
    WatchWindows();
    UpdateRenderVisibility();
}

void TerminalSurface::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    // 숨김·비활성 탭·최소화(Qt 가 spontaneous hide 를 보낸다): 렌더 스레드가 이 화면을 그리지 않는다(스펙 §5 정확성 요구).
    // 최상위 창이 가려진 것은 eventFilter 가 따로 본다
    m_bShown = false;
    UpdateRenderVisibility();
}

void TerminalSurface::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    if (m_iViewId != 0)
        m_pRenderThread->SetFocus(m_iViewId, true);
}

void TerminalSurface::focusOutEvent(QFocusEvent* event)
{
    QWidget::focusOutEvent(event);
    if (m_iViewId != 0)
        m_pRenderThread->SetFocus(m_iViewId, false);
}

bool TerminalSurface::event(QEvent* event)
{
    if (event->type() == QEvent::WinIdChange)
        OnWinIdChange();
    return QWidget::event(event);
}

bool TerminalSurface::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_pSurfaceWindow && event->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
    {
        DetachForSurfaceDestruction();
    }
    const QEvent::Type eType = event->type();
    if (watched == m_pTopLevelWindow && (eType == QEvent::Expose || eType == QEvent::Show || eType == QEvent::WindowStateChange))
    {
        // 최상위 창이 가려지거나(다른 창·다른 Space) 최소화되면 cocoa 는 빈 영역의 Expose 를 보낸다(isExposed 거짓).
        // 그런 창에 표시하면 macOS 에서 다른 화면까지 약 1초씩 멈췄다(스펙 §15 위험 5, 부록 A S1 (d)).
        // Windows 는 창을 꽉 채운 GPU 자식 창 때문에 빈 영역의 Expose 가 와도 드러나 있다(IsTopLevelExposeVisible)
        const bool bWindowExposed = eType == QEvent::Expose &&
                                    IsTopLevelExposeVisible(QGuiApplication::platformName(), static_cast<QExposeEvent*>(event)->region().isEmpty(),
                                                            m_pTopLevelWindow->isExposed());
        const bool bWindowMinimized = m_pTopLevelWindow->windowStates().testFlag(Qt::WindowMinimized);
        m_bTopLevelExposed = UpdateTopLevelExposed(m_bTopLevelExposed, eType, bWindowExposed, bWindowMinimized);
        UpdateRenderVisibility();
    }
    return QWidget::eventFilter(watched, event);
}

void TerminalSurface::Attach()
{
    if (m_iViewId != 0 || !m_pRenderThread || !m_pSource || m_bFallbackRequested)
        return;
    m_bAttaching = true;   // CreateNativeHandle 의 winId() 가 네이티브 창을 만들며 보내는 WinIdChange 는 무시한다
    m_bReattachPending = false;
    m_dDevicePixelRatio = devicePixelRatioF();
    TerminalNativeHandle handle;
    if (m_bGpu)
    {
        handle = CreateNativeHandle(this, m_dDevicePixelRatio);
        if (!handle.pHandle)
        {
            // 네이티브 창·레이어를 만들 수 없다(지원하지 않는 플랫폼·QPA): 이 화면은 CPU 위젯으로 바꿔 달라
            m_bAttaching = false;
            RequestCpuFallback();
            return;
        }
    }
    m_pNativeHandle = handle.pHandle;
    m_iViewId = m_pRenderThread->AttachView(m_pSource, handle, DevicePixelSize(), m_dDevicePixelRatio);
    m_pRenderThread->SetFocus(m_iViewId, hasFocus());
    m_bAttaching = false;
    WatchWindows();
    m_bRenderVisible = IsRenderVisible(m_bShown, m_bTopLevelExposed);
    m_pRenderThread->SetVisible(m_iViewId, m_bRenderVisible);
    SendRefreshRate();
    UpdateGrid();
}

void TerminalSurface::DetachForSurfaceDestruction()
{
    if (m_iViewId == 0)
        return;
    // 네이티브 창이 곧 사라진다: 렌더 스레드가 이 화면의 스왑체인·surface 를 지울 때까지 기다린다(최대 500ms, 스펙 §10).
    // 비동기로 떼면 렌더 스레드가 NSView 가 사라진 레이어에 제출하다 막혀 다른 화면도 함께 멈췄다(부록 A S1 (f)).
    // 시간이 지나면 더 기다리지 않는다(macOS 는 레이어를 우리가 retain 하므로 surface 는 유효하다).
    m_pRenderThread->DetachViewAndWait(m_iViewId, kDetachWaitMs);
    m_iViewId = 0;
    m_pNativeHandle = nullptr;
    m_bReattachPending = true;   // 다음 WinIdChange 에서 새 핸들로 다시 붙인다(새 id)
}

void TerminalSurface::OnWinIdChange()
{
    // Qt 는 네이티브 창을 지울 때 SurfaceAboutToBeDestroyed 뒤에 id 0 으로, 다시 만들 때 새 id 로 WinIdChange 를 보낸다.
    if (m_bAttaching || !m_bShownOnce || m_bFallbackRequested || !m_pRenderThread)
        return;
    if (!m_bGpu)
    {
        // CPU 화면은 네이티브 창을 쓰지 않는다: 뗐으면 바로 다시 붙인다
        if (m_bReattachPending)
            Attach();
        return;
    }
    if (internalWinId() == 0)
        return;   // 네이티브 창이 지워지는 중이다(SurfaceAboutToBeDestroyed 에서 이미 뗐다). 새 창이 생기면 다시 온다
    if (m_iViewId != 0)
    {
        // 떼라는 알림 없이 네이티브 창이 바뀌었다: 옛 창은 이미 없으므로 기다리지 않고 뗀다
        // (GUI 가 렌더 스레드를 기다리는 곳은 SurfaceAboutToBeDestroyed 와 GPU 위젯 소멸뿐이다)
        m_pRenderThread->DetachView(m_iViewId);
        m_iViewId = 0;
        m_pNativeHandle = nullptr;
        m_bReattachPending = true;
    }
    // 새 네이티브 창: create() 가 끝난 뒤(이벤트 루프로 돌아간 뒤) 새 핸들로 다시 붙인다
    QMetaObject::invokeMethod(this, [this]()
    {
        if (m_bReattachPending && m_iViewId == 0 && internalWinId() != 0)
            Attach();
    }, Qt::QueuedConnection);
}

void TerminalSurface::OnScreenChanged()
{
    if (UpdateScale())
    {
        SendResize();
        UpdateGrid();
    }
    SendRefreshRate();
}

void TerminalSurface::OnFrameReady(int viewId, const QImage& imgPatch, const QRect& rectPatch)
{
    if (viewId == 0 || viewId != m_iViewId || m_bGpu || imgPatch.isNull())
        return;
    const qreal dRatio = devicePixelRatioF();
    const QSize sizeDevice = DevicePixelSize();
    if (m_imgFrame.size() != sizeDevice || m_imgFrame.devicePixelRatio() != dRatio)
    {
        // 크기·배율이 바뀌었다: 새 크기의 이미지에 옛 내용을 옮긴다(곧 전체 조각이 온다)
        QImage imgResized(sizeDevice, QImage::Format_ARGB32_Premultiplied);
        imgResized.fill(palette().color(backgroundRole()));
        if (!m_imgFrame.isNull())
        {
            const int iCopyWidth = std::min(sizeDevice.width(), m_imgFrame.width());
            for (int iY = 0; iY < std::min(sizeDevice.height(), m_imgFrame.height()); ++iY)
                std::memcpy(imgResized.scanLine(iY), m_imgFrame.constScanLine(iY), size_t(iCopyWidth) * sizeof(QRgb));
        }
        imgResized.setDevicePixelRatio(dRatio);
        m_imgFrame = imgResized;
    }

    // 장치 픽셀 그대로 덮어 쓴다(배율이 소수여도 다시 표본화하지 않는다)
    const QImage imgSource = imgPatch.format() == QImage::Format_ARGB32_Premultiplied ? imgPatch
                                                                                      : imgPatch.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QRect rectTarget = rectPatch.intersected(QRect(QPoint(0, 0), m_imgFrame.size()));
    if (rectTarget.isEmpty())
        return;
    const int iSourceX = rectTarget.x() - rectPatch.x();
    const int iSourceY = rectTarget.y() - rectPatch.y();
    for (int iY = 0; iY < rectTarget.height(); ++iY)
    {
        const uchar* pSource = imgSource.constScanLine(iSourceY + iY) + size_t(iSourceX) * sizeof(QRgb);
        uchar* pTarget = m_imgFrame.scanLine(rectTarget.y() + iY) + size_t(rectTarget.x()) * sizeof(QRgb);
        std::memcpy(pTarget, pSource, size_t(rectTarget.width()) * sizeof(QRgb));
    }
    update(QRectF(rectTarget.x() / dRatio, rectTarget.y() / dRatio, rectTarget.width() / dRatio, rectTarget.height() / dRatio).toAlignedRect());
}

void TerminalSurface::OnViewNeedsCpuFallback(int viewId)
{
    if (viewId == 0 || viewId != m_iViewId)
        return;
    RequestCpuFallback();
}

void TerminalSurface::OnViewSurfaceLost(int viewId)
{
    if (viewId == 0 || viewId != m_iViewId)
        return;
    // surface 를 잃었다: 옛 화면을 떼고 새 핸들로 다시 붙인다(새 id). 옛 surface 는 렌더 스레드가 지우고 옛 핸들을 놓는다.
    m_pRenderThread->DetachView(m_iViewId);
    m_iViewId = 0;
    m_pNativeHandle = nullptr;
    Attach();
}

bool TerminalSurface::UpdateScale()
{
    const qreal dRatio = devicePixelRatioF();
    if (dRatio == m_dDevicePixelRatio)
        return false;
    m_dDevicePixelRatio = dRatio;
    if (m_bGpu && m_pNativeHandle)
    {
        TerminalNativeHandle handle;
        handle.pHandle = m_pNativeHandle;
        UpdateNativeScale(handle, dRatio);
    }
    return true;
}

void TerminalSurface::SendResize()
{
    if (m_iViewId != 0)
        m_pRenderThread->ResizeView(m_iViewId, DevicePixelSize(), devicePixelRatioF());
}

void TerminalSurface::SendRefreshRate()
{
    // 주사율은 GUI 스레드에서 QScreen 으로 얻는다(렌더 스레드는 vkGetRefreshCycleDurationGOOGLE 를 부르지 않는다, 스펙 §5).
    // Windows 는 Qt 가 정수로 버린 값 대신 그 모니터의 정확한 값(QueryDisplayConfig)을 쓴다: 59.949Hz 화면을 59 로 그리지 않게
    if (m_iViewId == 0)
        return;
    const double dHz = ScreenRefreshRate(screen());
    if (!(dHz > 0.0))
        return;
    m_pRenderThread->SetRefreshRate(m_iViewId, std::clamp(dHz, kMinRefreshHz, kMaxRefreshHz));
}

void TerminalSurface::UpdateGrid()
{
    const QSize sizeGrid = GetGridSize();
    if (sizeGrid == m_sizeGrid)
        return;
    m_sizeGrid = sizeGrid;
    emit GridSizeChanged(sizeGrid);
}

void TerminalSurface::WatchWindows()
{
    // 이 위젯의 네이티브 창(GPU 자식 창, 또는 최상위 창)이 지워지기 직전과 최상위 창이 드러났는지를 본다.
    // 둘이 같은 창(최상위 CPU 위젯)일 수 있으므로 더는 어느 쪽으로도 보지 않는 창에서만 필터를 뗀다.
    QWindow* pSurfaceWindow = windowHandle();
    QWindow* pTopLevelWindow = window() ? window()->windowHandle() : nullptr;
    for (QWindow* pOld : {m_pSurfaceWindow.data(), m_pTopLevelWindow.data()})
    {
        if (pOld && pOld != pSurfaceWindow && pOld != pTopLevelWindow)
            pOld->removeEventFilter(this);
    }
    if (pTopLevelWindow != m_pTopLevelWindow)
        m_bTopLevelExposed = true;   // 다른 최상위 창으로 옮겼다: 드러나지 않았다고 알릴 때까지 드러난 것으로 본다
    m_pSurfaceWindow = pSurfaceWindow;
    m_pTopLevelWindow = pTopLevelWindow;
    for (QWindow* pWatched : {pSurfaceWindow, pTopLevelWindow})
    {
        if (pWatched)
            pWatched->installEventFilter(this);   // 이미 붙어 있으면 맨 앞으로 옮길 뿐 둘이 되지 않는다
    }
    // 화면(배율·주사율)이 바뀌는 것은 자기 네이티브 창, 없으면 최상위 창에서 받는다
    QWindow* pScreenWindow = pSurfaceWindow ? pSurfaceWindow : pTopLevelWindow;
    if (pScreenWindow != m_pScreenWindow)
    {
        disconnect(m_connScreen);
        m_pScreenWindow = pScreenWindow;
        if (pScreenWindow)
            m_connScreen = connect(pScreenWindow, &QWindow::screenChanged, this, &TerminalSurface::OnScreenChanged);
    }
}

void TerminalSurface::UpdateRenderVisibility()
{
    const bool bVisible = IsRenderVisible(m_bShown, m_bTopLevelExposed);
    if (m_iViewId == 0 || bVisible == m_bRenderVisible)
        return;   // 최상위 창은 다시 그릴 때마다 Expose 를 받는다: 바뀔 때만 보낸다
    m_bRenderVisible = bVisible;
    m_pRenderThread->SetVisible(m_iViewId, bVisible);
}

void TerminalSurface::RequestCpuFallback()
{
    if (m_bFallbackRequested)
        return;
    m_bFallbackRequested = true;
    // 지금 처리 중인 이벤트가 끝난 뒤 알린다(받는 쪽이 이 위젯을 바꿔 끼울 수 있게)
    QMetaObject::invokeMethod(this, [this]() { emit CpuFallbackRequested(); }, Qt::QueuedConnection);
}
