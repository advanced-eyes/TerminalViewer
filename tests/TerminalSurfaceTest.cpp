#include "TerminalViewer/UI/TerminalSurface.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalPlatform.h"
#include "TerminalViewer/Thread/TerminalRenderThread.h"
#include "TerminalViewer/UI/TerminalSurfaceNative.h"
#include "TerminalViewer/Testing/FakeFrameSource.h"
#include "ScopedEnv.h"
#include "TestFonts.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QWindow>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace
{
constexpr int kCols = 20;
constexpr int kRows = 5;
constexpr int kPaddingLogical = 4;
constexpr int kFrameTimeoutMs = 1000;
constexpr int kSettleMs = 150;
constexpr int kBlinkWindowMs = 1200;
constexpr int kRecreateRounds = 10;
constexpr TerminalRgb kBackground{18, 52, 86};
constexpr TerminalRgb kForeground{230, 230, 230};
constexpr TerminalRgb kRed{255, 0, 0};

QFont BaseFont()
{
    QFont font(QStringLiteral("Cascadia Mono"));
    font.setPixelSize(14);
    return font;
}

QSize ViewSize()
{
    const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
    return QSize(kCols * metrics.iCellWidth + 2 * kPaddingLogical, kRows * metrics.iCellHeight + 2 * kPaddingLogical);
}

QPoint CellCenter(int col, int row)
{
    const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
    return QPoint(kPaddingLogical + col * metrics.iCellWidth + metrics.iCellWidth / 2,
                  kPaddingLogical + row * metrics.iCellHeight + metrics.iCellHeight / 2);
}

TerminalCell BgCell(const TerminalRgb& bg)
{
    TerminalCell cell;
    cell.bBgDefault = false;
    cell.bg = bg;
    return cell;
}

TerminalNativeHandle CountingHandle(std::atomic<int>& releases)
{
    static int s_iFakeNative = 0;
    TerminalNativeHandle handle;
    handle.pHandle = &s_iFakeNative;   // 아무 주소(GPU 핸들처럼 보인다)
    handle.funcRelease = [&releases](void*) { ++releases; };
    return handle;
}

int CountId(const QSignalSpy& spy, int viewId)
{
    int iCount = 0;
    for (const QList<QVariant>& args : spy)
        iCount += args.at(0).toInt() == viewId ? 1 : 0;
    return iCount;
}

// 다른 스레드에서 1ms 마다 내용을 바꾸고 지금 화면 id 를 깨운다(IO 스레드 흉내)
class OutputThread
{
public:
    OutputThread(TerminalRenderThread& renderThread, FakeFrameSource& source)
        : m_renderThread(renderThread)
        , m_source(source)
        , m_thWorker([this] { Run(); })
    {
    }

    ~OutputThread() { Join(); }

    void SetViewId(int viewId) { m_iViewId = viewId; }

    void Join()
    {
        m_bRunning = false;
        if (m_thWorker.joinable())
            m_thWorker.join();
    }

private:
    void Run()
    {
        uint8_t uShade = 0;
        while (m_bRunning)
        {
            ++uShade;
            m_source.SetCell(m_source.GetViewportTop() + uShade % kRows, uShade % kCols, BgCell(TerminalRgb{uShade, 0, 0}));
            m_renderThread.Wake(m_iViewId);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    TerminalRenderThread& m_renderThread;
    FakeFrameSource& m_source;
    std::atomic<int> m_iViewId{0};
    std::atomic<bool> m_bRunning{true};
    std::thread m_thWorker;
};

// 무장하면 다음 캡처 하나가 오래 걸리는 공급원. 주인이 "놓았다"고 표시한 뒤의 캡처를 센다(놓은 공급원 사용 = 해제 후 사용).
class SlowFrameSource : public ITerminalFrameSource
{
public:
    explicit SlowFrameSource(FakeFrameSource& inner)
        : m_inner(inner)
    {
    }

    void ArmSlowCapture(int milliseconds) { m_iSlowMs = milliseconds; }
    bool WaitInCapture(int timeoutMs) const
    {
        return QTest::qWaitFor([this] { return m_bInCapture.load(); }, timeoutMs);
    }
    void MarkFreed()
    {
        m_bInCaptureWhenFreed = m_bInCapture.load();
        m_bFreed = true;
    }
    bool IsFreed() const { return m_bFreed; }
    bool WasInCaptureWhenFreed() const { return m_bInCaptureWhenFreed; }
    int GetUsesAfterFree() const { return m_iUsesAfterFree; }

    void CaptureFrame(const TerminalCaptureRequest& request, TerminalFrame& frame) override
    {
        if (m_bFreed)
            ++m_iUsesAfterFree;
        m_bInCapture = true;
        const int iSlowMs = m_iSlowMs.exchange(0);
        if (iSlowMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(iSlowMs));
        m_inner.CaptureFrame(request, frame);
        m_bInCapture = false;
    }

    int ScrollViewport(int rows) override
    {
        if (m_bFreed)
            ++m_iUsesAfterFree;
        return m_inner.ScrollViewport(rows);
    }

private:
    FakeFrameSource& m_inner;
    std::atomic<int> m_iSlowMs{0};
    std::atomic<bool> m_bInCapture{false};
    std::atomic<bool> m_bFreed{false};
    std::atomic<bool> m_bInCaptureWhenFreed{false};
    std::atomic<int> m_iUsesAfterFree{0};
};
}

// 공급원·렌더 스레드를 위젯보다 먼저 선언한다: 위젯이 먼저 떼고, 스레드가 멈춘 뒤 공급원이 지워진다.
class TerminalSurfaceTest : public QObject
{
    Q_OBJECT

private:
    static void StartCpu(TerminalRenderThread& thread)
    {
        thread.SetFont(BaseFont());
        thread.Start();
    }

    static bool ShowAndWaitFrame(TerminalSurface& surface, QSignalSpy& spyFrames)
    {
        surface.resize(ViewSize());
        surface.show();
        if (!QTest::qWaitForWindowExposed(&surface) || surface.GetViewId() <= 0)
            return false;
        return QTest::qWaitFor([&] { return CountId(spyFrames, surface.GetViewId()) >= 1; }, kFrameTimeoutMs);
    }

private slots:
    void initTestCase() { TestFonts::Register(); }

    void shownSurfaceShowsSourceBackground()
    {
        FakeFrameSource source(kCols, kRows, 0);
        source.SetColors(kBackground, kForeground);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread);
        TerminalSurface surface(&thread, &source, false);
        QVERIFY(!surface.IsGpu());
        QCOMPARE(surface.GetViewId(), 0);   // 보이기 전에는 붙지 않는다
        surface.resize(ViewSize());
        surface.show();
        QVERIFY(QTest::qWaitForWindowExposed(&surface));
        QVERIFY(surface.GetViewId() > 0);
        const QRgb expected = qRgb(kBackground.r, kBackground.g, kBackground.b);
        QVERIFY(QTest::qWaitFor([&] { return surface.grab().toImage().pixel(CellCenter(3, 2)) == expected; }, kFrameTimeoutMs));
    }

    void resizeEmitsComputedGrid()
    {
        FakeFrameSource source(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread);
        TerminalSurface surface(&thread, &source, false);
        QSignalSpy spyGrid(&surface, &TerminalSurface::GridSizeChanged);
        surface.resize(ViewSize());
        surface.show();
        QVERIFY(QTest::qWaitForWindowExposed(&surface));
        spyGrid.clear();

        const QSize newSize(333, 211);
        surface.resize(newSize);
        QVERIFY(QTest::qWaitFor([&] { return !spyGrid.isEmpty(); }, kFrameTimeoutMs));
        const qreal dRatio = surface.devicePixelRatioF();
        const QSize deviceSize(qRound(newSize.width() * dRatio), qRound(newSize.height() * dRatio));
        const QSize expected = CellMetrics::Compute(BaseFont(), dRatio).ComputeGrid(deviceSize, qRound(kPaddingLogical * dRatio));
        QCOMPARE(spyGrid.last().at(0).toSize(), expected);
        QCOMPARE(surface.GetGridSize(), expected);

        // 글꼴이 바뀌어도 다시 잰다
        spyGrid.clear();
        QFont bigger = BaseFont();
        bigger.setPixelSize(20);
        thread.SetFont(bigger);
        QVERIFY(QTest::qWaitFor([&] { return !spyGrid.isEmpty(); }, kFrameTimeoutMs));
        QCOMPARE(spyGrid.last().at(0).toSize(), CellMetrics::Compute(bigger, dRatio).ComputeGrid(deviceSize, qRound(kPaddingLogical * dRatio)));
    }

    void hiddenSurfaceGetsNoPatches()
    {
        FakeFrameSource source(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        StartCpu(thread);
        TerminalSurface surface(&thread, &source, false);
        QVERIFY(ShowAndWaitFrame(surface, spyFrames));
        const int iView = surface.GetViewId();
        QTest::qWait(kSettleMs);

        surface.hide();
        spyFrames.clear();
        const int iCaptures = source.GetCaptureCount();
        source.SetCell(1, 1, BgCell(kRed));
        thread.Wake(iView);
        QTest::qWait(200);
        QCOMPARE(CountId(spyFrames, iView), 0);
        QCOMPARE(source.GetCaptureCount(), iCaptures);

        surface.show();   // 다시 보이면 다시 그린다
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iView) >= 1; }, kFrameTimeoutMs));
    }

    void renderVisibilityNeedsShownWidgetAndExposedTopLevel()
    {
        QVERIFY(IsRenderVisible(true, true));
        QVERIFY(!IsRenderVisible(true, false));    // 최소화·가려짐·다른 Space
        QVERIFY(!IsRenderVisible(false, true));    // 숨은 탭·숨김
        QVERIFY(!IsRenderVisible(false, false));

        // Expose 는 창이 지금 드러났는지를 따른다
        QVERIFY(!UpdateTopLevelExposed(true, QEvent::Expose, false, false));
        QVERIFY(UpdateTopLevelExposed(false, QEvent::Expose, true, false));
        QVERIFY(UpdateTopLevelExposed(true, QEvent::Expose, true, false));
        // 다시 보인 창은 노출 알림이 없어도 드러난 것으로 본다
        QVERIFY(UpdateTopLevelExposed(false, QEvent::Show, false, false));
        // 최소화하면 드러나지 않고, 최소화에서 돌아오면 노출 알림이 없어도(cocoa) 드러난 것으로 본다
        QVERIFY(!UpdateTopLevelExposed(true, QEvent::WindowStateChange, true, true));
        QVERIFY(UpdateTopLevelExposed(false, QEvent::WindowStateChange, false, false));
        // 그 밖의 이벤트는 바꾸지 않는다
        QVERIFY(!UpdateTopLevelExposed(false, QEvent::Resize, true, false));
        QVERIFY(UpdateTopLevelExposed(true, QEvent::Hide, false, true));
    }

    void unexposedWindowGetsNoPatches()
    {
        // 최상위 창이 드러나 있지 않으면(최소화·가려짐·다른 Space: 빈 영역의 Expose) 위젯이 보여도 그리지 않는다.
        // 다시 드러나면(또는 창이 다시 보이면) 다시 그린다.
        FakeFrameSource source(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        StartCpu(thread);
        TerminalSurface surface(&thread, &source, false);
        QVERIFY(ShowAndWaitFrame(surface, spyFrames));
        const int iView = surface.GetViewId();
        QWindow* pWindow = surface.windowHandle();
        QVERIFY(pWindow);
        QTest::qWait(kSettleMs);

        QExposeEvent unexposed{QRegion()};
        QCoreApplication::sendEvent(pWindow, &unexposed);
        QVERIFY(surface.isVisible());
        spyFrames.clear();
        const int iCaptures = source.GetCaptureCount();
        source.SetCell(1, 1, BgCell(kRed));
        thread.Wake(iView);
        QTest::qWait(kSettleMs);
        QCOMPARE(CountId(spyFrames, iView), 0);
        QCOMPARE(source.GetCaptureCount(), iCaptures);

        QExposeEvent exposed{QRegion(QRect(QPoint(0, 0), surface.size()))};
        QCoreApplication::sendEvent(pWindow, &exposed);
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iView) >= 1; }, kFrameTimeoutMs));

        // 최소화하면 그리지 않고(cocoa 는 빈 영역의 Expose 도 보낸다), 최소화에서 돌아오면 노출 알림 없이도(cocoa) 다시 그린다
        surface.setWindowState(Qt::WindowMinimized);
        QTRY_VERIFY(pWindow->windowState() == Qt::WindowMinimized);
        QCoreApplication::sendEvent(pWindow, &unexposed);
        QTest::qWait(kSettleMs);
        spyFrames.clear();
        source.SetCell(3, 3, BgCell(kRed));
        thread.Wake(iView);
        QTest::qWait(kSettleMs);
        QCOMPARE(CountId(spyFrames, iView), 0);
        surface.setWindowState(Qt::WindowNoState);
        QTRY_VERIFY(pWindow->windowState() == Qt::WindowNoState);
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iView) >= 1; }, kFrameTimeoutMs));

        // 창을 숨겼다 다시 보이면 노출 알림을 기다리지 않고 다시 그린다
        QCoreApplication::sendEvent(pWindow, &unexposed);
        surface.hide();
        surface.show();
        QVERIFY(QTest::qWaitForWindowExposed(&surface));
        spyFrames.clear();
        source.SetCell(2, 2, BgCell(kRed));
        thread.Wake(iView);
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iView) >= 1; }, kFrameTimeoutMs));
    }

    void focusChangesReachRenderThread()
    {
        FakeFrameSource source(kCols, kRows, 0);
        TerminalCursor cursor;
        cursor.bVisible = true;
        cursor.bBlinking = true;
        cursor.eShape = ECursorShape::BLOCK;
        source.SetCursor(cursor);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        StartCpu(thread);
        TerminalSurface surface(&thread, &source, false);
        QVERIFY(ShowAndWaitFrame(surface, spyFrames));
        const int iView = surface.GetViewId();
        QApplication::setActiveWindow(&surface);
        QVERIFY(QTest::qWaitForWindowActive(&surface));
        surface.clearFocus();
        QTRY_VERIFY(!surface.hasFocus());
        QTest::qWait(kSettleMs);
        spyFrames.clear();
        QTest::qWait(kBlinkWindowMs);
        QCOMPARE(CountId(spyFrames, iView), 0);   // 포커스가 없으면 깜빡이지 않는다

        surface.setFocus();
        QTRY_VERIFY(surface.hasFocus());
        QTest::qWait(kSettleMs);
        spyFrames.clear();
        QTest::qWait(kBlinkWindowMs);
        QVERIFY2(CountId(spyFrames, iView) >= 2, qPrintable(QString::number(CountId(spyFrames, iView))));

        surface.clearFocus();
        QTRY_VERIFY(!surface.hasFocus());
        QTest::qWait(kSettleMs);
        spyFrames.clear();
        QTest::qWait(kBlinkWindowMs);
        QCOMPARE(CountId(spyFrames, iView), 0);
    }

    void deletingSurfaceDetachesView()
    {
        FakeFrameSource source(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        StartCpu(thread);
        auto upSurface = std::make_unique<TerminalSurface>(&thread, &source, false);
        QVERIFY(ShowAndWaitFrame(*upSurface, spyFrames));
        const int iView = upSurface->GetViewId();

        upSurface.reset();
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyDetached, iView) >= 1; }, kFrameTimeoutMs));
        spyFrames.clear();
        const int iCaptures = source.GetCaptureCount();
        source.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iView);   // 뗀 id 를 깨워도 아무 일 없다
        thread.SetVisible(iView, true);
        QTest::qWait(kSettleMs);
        QCOMPARE(CountId(spyFrames, iView), 0);
        QCOMPARE(source.GetCaptureCount(), iCaptures);
    }

    void cpuFallbackSignalOnlyForOwnView()
    {
        FakeFrameSource source(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        StartCpu(thread);
        TerminalSurface surface(&thread, &source, false);
        QSignalSpy spyFallback(&surface, &TerminalSurface::CpuFallbackRequested);
        QVERIFY(ShowAndWaitFrame(surface, spyFrames));
        const int iView = surface.GetViewId();

        emit thread.ViewNeedsCpuFallback(iView + 1000);
        QTest::qWait(50);
        QCOMPARE(spyFallback.count(), 0);
        emit thread.ViewNeedsCpuFallback(iView);
        QVERIFY(spyFallback.wait(kFrameTimeoutMs));
        QCOMPARE(spyFallback.count(), 1);
    }

    void gpuSurfaceWithoutNativeLayerRequestsCpu()
    {
        // offscreen QPA 는 NSView/HWND 가 없다: GPU 위젯은 붙지 않고 CPU 위젯으로 바꿔 달라고 한다
        FakeFrameSource source(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        StartCpu(thread);
        TerminalSurface surface(&thread, &source, true);
        QVERIFY(surface.IsGpu());
        QVERIFY(surface.testAttribute(Qt::WA_NativeWindow));
        QVERIFY(surface.testAttribute(Qt::WA_PaintOnScreen));
        QVERIFY(surface.testAttribute(Qt::WA_NoSystemBackground));
        QVERIFY(!surface.testAttribute(Qt::WA_DontCreateNativeAncestors));
        QSignalSpy spyFallback(&surface, &TerminalSurface::CpuFallbackRequested);
        surface.resize(ViewSize());
        surface.show();
        QVERIFY(spyFallback.wait(kFrameTimeoutMs));
        QCOMPARE(spyFallback.count(), 1);
        QCOMPARE(surface.GetViewId(), 0);
    }

    void nativeSurfaceRecreationReattaches()
    {
        FakeFrameSource source(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        StartCpu(thread);
        TerminalSurface surface(&thread, &source, false);
        QVERIFY(ShowAndWaitFrame(surface, spyFrames));
        OutputThread output(thread, source);
        output.SetViewId(surface.GetViewId());
        QTest::qWait(50);

        for (int iRound = 0; iRound < kRecreateRounds; ++iRound)
        {
            QWindow* pWindow = surface.windowHandle();
            QVERIFY(pWindow);
            const int iOld = surface.GetViewId();
            QPlatformSurfaceEvent aboutToBeDestroyed(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
            QCoreApplication::sendEvent(pWindow, &aboutToBeDestroyed);
            // 기다려 뗐다(CPU 화면은 Qt 가 함께 보내는 WinIdChange(0) 에서 바로 다시 붙을 수 있다)
            QVERIFY(surface.GetViewId() != iOld);
            QVERIFY(QTest::qWaitFor([&] { return CountId(spyDetached, iOld) >= 1; }, kFrameTimeoutMs));
            QEvent winIdChange(QEvent::WinIdChange);
            QCoreApplication::sendEvent(&surface, &winIdChange);
            const int iNew = surface.GetViewId();
            QVERIFY2(iNew > 0 && iNew != iOld, qPrintable(QStringLiteral("round %1: %2 -> %3").arg(iRound).arg(iOld).arg(iNew)));
            output.SetViewId(iNew);
            QVERIFY2(QTest::qWaitFor([&] { return CountId(spyFrames, iNew) >= 2; }, kFrameTimeoutMs),
                     qPrintable(QStringLiteral("round %1").arg(iRound)));   // 새 id 로 프레임이 이어진다
        }
        output.Join();
    }

    void vulkanMissingFallsBackToCpu()
    {
        // 성공 기준 8: Vulkan 이 없으면 GPU 화면을 CPU 로 바꿔 달라고 하고, 핸들을 한 번 놓고, 앱은 계속 돈다
        ScopedEnv disable("OCEAN_VK_DISABLE", "1");
        FakeFrameSource source(kCols, kRows, 0);
        FakeFrameSource cpuSource(kCols, kRows, 0);
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        // offscreen QPA 는 네이티브 화면을 만들 수 없어 처음부터 CPU 로 알린다. 그래도 GPU 핸들이 오면 GPU 를 시도한다.
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::CPU);
        QSignalSpy spyStrategy(&thread, &TerminalRenderThread::StrategyChanged);
        QSignalSpy spyFallback(&thread, &TerminalRenderThread::ViewNeedsCpuFallback);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^Terminal renderer: GPU unavailable, drawing with CPU: ")));
        StartCpu(thread);
        const int iView = thread.AttachView(&source, CountingHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(spyFallback.wait(kFrameTimeoutMs));
        QCOMPARE(spyFallback.at(0).at(0).toInt(), iView);
        QVERIFY(QTest::qWaitFor([&] { return spyStrategy.count() >= 1; }, kFrameTimeoutMs));
        QCOMPARE(spyStrategy.count(), 1);
        QCOMPARE(spyStrategy.at(0).at(0).value<ETerminalRenderStrategy>(), ETerminalRenderStrategy::CPU);
        QCOMPARE(iReleases.load(), 1);
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::CPU);
        QVERIFY(!thread.GetGpuError().isEmpty());

        const int iCpu = thread.AttachView(&cpuSource, {}, ViewSize(), 1.0);   // 바꿔 낀 CPU 위젯
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iCpu) >= 1; }, kFrameTimeoutMs));
        thread.DetachView(iView);
        thread.Stop();
        QCOMPARE(iReleases.load(), 1);
    }

    void deletedSurfaceSourceIsFreedOnlyOnRelease()
    {
        // 공급원 주인은 위젯을 지운 뒤 SourceReleased 가 올 때 공급원을 놓는다. 렌더 스레드가 500ms 넘게 캡처 안에
        // 있어도 놓은 뒤에는 공급원을 쓰지 않는다(GPU·CPU 같은 규칙).
        FakeFrameSource inner(kCols, kRows, 0);
        SlowFrameSource source(inner);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        connect(&thread, &TerminalRenderThread::SourceReleased, this, [&source](ITerminalFrameSource* pSource)
        {
            if (pSource == &source)
                source.MarkFreed();
        });
        StartCpu(thread);
        auto upSurface = std::make_unique<TerminalSurface>(&thread, &source, false);
        QVERIFY(ShowAndWaitFrame(*upSurface, spyFrames));
        const int iView = upSurface->GetViewId();

        source.ArmSlowCapture(700);
        inner.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iView);
        QVERIFY(source.WaitInCapture(kFrameTimeoutMs));
        upSurface.reset();
        QVERIFY(!source.IsFreed());   // 렌더 스레드가 아직 캡처 안에 있다
        QVERIFY(QTest::qWaitFor([&] { return source.IsFreed(); }, 3000));
        QVERIFY(!source.WasInCaptureWhenFreed());
        for (int iWake = 0; iWake < 5; ++iWake)
        {
            thread.Wake(iView);
            thread.ScrollPixels(iView, 10);
        }
        QTest::qWait(kSettleMs);
        QCOMPARE(source.GetUsesAfterFree(), 0);
    }

    void neverAttachedSurfaceReleasesSourceOnDeletion()
    {
        // 한 번도 붙지 않은 위젯(보이지 않았거나, 네이티브 핸들을 못 만들어 바로 CPU 로 바꿔 달라고 한 GPU 위젯)도
        // 지울 때 SourceReleased 가 온다. 살아 있는 동안에는 오지 않는다.
        FakeFrameSource hiddenSource(kCols, kRows, 0);
        FakeFrameSource gpuSource(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyReleased(&thread, &TerminalRenderThread::SourceReleased);
        StartCpu(thread);
        {
            TerminalSurface hidden(&thread, &hiddenSource, false);
        }
        QCOMPARE(spyReleased.count(), 1);
        QCOMPARE(spyReleased.at(0).at(0).value<ITerminalFrameSource*>(), static_cast<ITerminalFrameSource*>(&hiddenSource));

        auto upGpu = std::make_unique<TerminalSurface>(&thread, &gpuSource, true);
        QSignalSpy spyFallback(upGpu.get(), &TerminalSurface::CpuFallbackRequested);
        upGpu->resize(ViewSize());
        upGpu->show();
        QVERIFY(spyFallback.wait(kFrameTimeoutMs));
        QCOMPARE(upGpu->GetViewId(), 0);
        QTest::qWait(kSettleMs);
        QCOMPARE(spyReleased.count(), 1);   // 살아 있는 동안에는 오지 않는다
        upGpu.reset();
        QCOMPARE(spyReleased.count(), 2);
        QCOMPARE(spyReleased.at(1).at(0).value<ITerminalFrameSource*>(), static_cast<ITerminalFrameSource*>(&gpuSource));
    }

    void surfaceDetachedThenDeletedReleasesOnlyAfterDeletion()
    {
        // 네이티브 창이 지워져 떨어진 위젯은 다시 붙을 수 있다: 공급원의 화면 수가 0 이 되어도 위젯을 지울 때까지 알리지 않는다
        FakeFrameSource source(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        QSignalSpy spyReleased(&thread, &TerminalRenderThread::SourceReleased);
        StartCpu(thread);
        auto upSurface = std::make_unique<TerminalSurface>(&thread, &source, false);
        QVERIFY(ShowAndWaitFrame(*upSurface, spyFrames));
        QWindow* pWindow = upSurface->windowHandle();
        QVERIFY(pWindow);
        // 첫 가짜 파괴 뒤에는 Qt 가 보내는 WinIdChange(0) 으로 CPU 화면이 바로 다시 붙는다. 두 번째 뒤에는 떨어진 채로 남는다.
        for (int iRound = 0; iRound < 2; ++iRound)
        {
            QPlatformSurfaceEvent aboutToBeDestroyed(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
            QCoreApplication::sendEvent(pWindow, &aboutToBeDestroyed);
        }
        QCOMPARE(upSurface->GetViewId(), 0);
        QVERIFY(QTest::qWaitFor([&] { return spyDetached.count() >= 2; }, kFrameTimeoutMs));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyReleased.count(), 0);   // 위젯이 살아 있다
        upSurface.reset();
        QVERIFY(QTest::qWaitFor([&] { return spyReleased.count() >= 1; }, kFrameTimeoutMs));
        QCOMPARE(spyReleased.count(), 1);
        QCOMPARE(spyReleased.at(0).at(0).value<ITerminalFrameSource*>(), static_cast<ITerminalFrameSource*>(&source));
    }

    void surfaceLostReattachesWithNewId()
    {
        FakeFrameSource source(kCols, kRows, 0);
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        StartCpu(thread);
        TerminalSurface surface(&thread, &source, false);
        QVERIFY(ShowAndWaitFrame(surface, spyFrames));
        const int iOld = surface.GetViewId();

        emit thread.ViewSurfaceLost(iOld + 1000);   // 남의 id 는 무시한다
        QCOMPARE(surface.GetViewId(), iOld);
        emit thread.ViewSurfaceLost(iOld);
        const int iNew = surface.GetViewId();
        QVERIFY(iNew > 0 && iNew != iOld);
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyDetached, iOld) >= 1; }, kFrameTimeoutMs));
        source.SetCell(1, 1, BgCell(kRed));
        thread.Wake(iNew);
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iNew) >= 1; }, kFrameTimeoutMs));
    }

    void nativeSurfaceSupportIsAlgorithmLayerQuery()
    {
        // 렌더 스레드(Thread 층)도 보는 질문이라 UI 가 아닌 Algorithm 층(TerminalPlatform.h)에 둔다.
        // offscreen QPA 는 네이티브 화면이 없다: 질문은 거짓, 핸들은 비어 있다(winId() 를 만들지 않는다)
        if (QGuiApplication::platformName() != QStringLiteral("offscreen"))
            QSKIP("offscreen QPA 에서만 본다");
        QVERIFY(!IsNativeSurfaceSupported());
        QWidget widget;
        const TerminalNativeHandle handle = CreateNativeHandle(&widget, 1.0);
        QVERIFY(handle.pHandle == nullptr);
        QVERIFY(!handle.funcRelease);
        QCOMPARE(widget.internalWinId(), WId(0));
    }
};

QObject* CreateTerminalSurfaceTest() { return new TerminalSurfaceTest(); }

#include "TerminalSurfaceTest.moc"
