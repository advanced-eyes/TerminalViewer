#include "TerminalViewer/Thread/TerminalRenderThread.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "FakeFrameSource.h"
#include "TestFonts.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
constexpr int kCols = 20;
constexpr int kRows = 5;
constexpr int kPaddingLogical = 4;          // TerminalTheme 기본 여백
constexpr int kFrameTimeoutMs = 1000;
constexpr int kSettleMs = 100;
constexpr int kIdleWindowMs = 300;
constexpr int kScrollSettleMs = 600;   // 스크롤 애니메이션이 끝나기를 기다리는 시간
constexpr TerminalRgb kRed{255, 0, 0};
constexpr TerminalRgb kGreen{0, 255, 0};
constexpr TerminalRgb kBlue{0, 0, 255};

QFont BaseFont()
{
    QFont font(QStringLiteral("Cascadia Mono"));
    font.setPixelSize(14);
    return font;
}

CellMetrics Metrics(qreal devicePixelRatio)
{
    return CellMetrics::Compute(BaseFont(), devicePixelRatio);
}

QSize ViewSize(qreal devicePixelRatio)
{
    const CellMetrics metrics = Metrics(devicePixelRatio);
    const int iPadding = qRound(kPaddingLogical * devicePixelRatio);
    return QSize(kCols * metrics.iCellWidth + 2 * iPadding, kRows * metrics.iCellHeight + 2 * iPadding);
}

QPoint CellCenter(int col, int row)
{
    const CellMetrics metrics = Metrics(1.0);
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

QRgb ToQRgb(const TerminalRgb& rgb)
{
    return qRgb(rgb.r, rgb.g, rgb.b);
}

TerminalNativeHandle CountingHandle(std::atomic<int>& releases)
{
    static int s_iFakeNative = 0;
    TerminalNativeHandle handle;
    handle.pHandle = &s_iFakeNative;   // GPU 핸들처럼 보이지만 CPU 모드라 CPU 로 그린다
    handle.funcRelease = [&releases](void*) { ++releases; };
    return handle;
}

struct Patch
{
    int iViewId = 0;
    QImage img;
    QRect rect;
};

// FrameReady 를 GUI 스레드에서 모은다.
struct FrameLog
{
    std::vector<Patch> vecPatches;

    int Count(int viewId) const
    {
        int iCount = 0;
        for (const Patch& patch : vecPatches)
        {
            iCount += patch.iViewId == viewId ? 1 : 0;
        }
        return iCount;
    }

    const Patch* Last(int viewId) const
    {
        for (auto it = vecPatches.rbegin(); it != vecPatches.rend(); ++it)
        {
            if (it->iViewId == viewId)
            {
                return &*it;
            }
        }
        return nullptr;
    }

    const Patch* LastContaining(int viewId, const QPoint& point) const
    {
        for (auto it = vecPatches.rbegin(); it != vecPatches.rend(); ++it)
        {
            if (it->iViewId == viewId && it->rect.contains(point))
            {
                return &*it;
            }
        }
        return nullptr;
    }

    // 받은 조각을 모두 차례로 덮어 쓴 화면(TerminalSurface 와 같은 방식)
    QImage Composite(int viewId, const QSize& size) const
    {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        for (const Patch& patch : vecPatches)
        {
            if (patch.iViewId == viewId)
            {
                painter.drawImage(patch.rect.topLeft(), patch.img);
            }
        }
        return image;
    }
};

// 캡처 안에서 테스트가 풀어 줄 때까지 멈추는 공급원(렌더 스레드가 "프레임 도중"인 상태를 만든다).
class BlockingFrameSource : public ITerminalFrameSource
{
public:
    explicit BlockingFrameSource(FakeFrameSource& inner)
        : m_inner(inner)
    {
    }

    ~BlockingFrameSource() override { Release(); }

    void Arm()
    {
        std::lock_guard<std::mutex> lock(m_mtxState);
        m_bArmed = true;
        m_bEntered = false;
        m_bReleased = false;
    }

    bool WaitEntered(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mtxState);
        return m_cvState.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return m_bEntered; });
    }

    void Release()
    {
        {
            std::lock_guard<std::mutex> lock(m_mtxState);
            m_bReleased = true;
        }
        m_cvState.notify_all();
    }

    void CaptureFrame(const TerminalCaptureRequest& request, TerminalFrame& frame) override
    {
        {
            std::unique_lock<std::mutex> lock(m_mtxState);
            if (m_bArmed)
            {
                m_bArmed = false;
                m_bEntered = true;
                m_cvState.notify_all();
                m_cvState.wait(lock, [this] { return m_bReleased; });
            }
        }
        m_inner.CaptureFrame(request, frame);
    }

    int ScrollViewport(int rows) override { return m_inner.ScrollViewport(rows); }

private:
    FakeFrameSource& m_inner;
    std::mutex m_mtxState;
    std::condition_variable m_cvState;
    bool m_bArmed = false;
    bool m_bEntered = false;
    bool m_bReleased = false;
};

// 캡처 요청을 차례로 기록하는 공급원(첫 프레임의 요청을 경쟁 없이 확인한다).
class RecordingFrameSource : public ITerminalFrameSource
{
public:
    explicit RecordingFrameSource(FakeFrameSource& inner)
        : m_inner(inner)
    {
    }

    std::vector<TerminalCaptureRequest> GetRequests() const
    {
        std::lock_guard<std::mutex> lock(m_mtxRequests);
        return m_vecRequests;
    }

    void CaptureFrame(const TerminalCaptureRequest& request, TerminalFrame& frame) override
    {
        {
            std::lock_guard<std::mutex> lock(m_mtxRequests);
            m_vecRequests.push_back(request);
        }
        m_inner.CaptureFrame(request, frame);
    }

    int ScrollViewport(int rows) override { return m_inner.ScrollViewport(rows); }

private:
    FakeFrameSource& m_inner;
    mutable std::mutex m_mtxRequests;
    std::vector<TerminalCaptureRequest> m_vecRequests;
};

// 다른 스레드에서 1ms 마다 내용을 바꾸고 Wake 한다(IO 스레드 흉내).
class Spammer
{
public:
    Spammer(TerminalRenderThread& thread, std::vector<std::pair<FakeFrameSource*, int>> vecTargets)
        : m_thread(thread)
        , m_vecTargets(std::move(vecTargets))
        , m_thWorker([this] { Run(); })
    {
    }

    ~Spammer() { Join(); }

    void Join()
    {
        m_bRunning = false;
        if (m_thWorker.joinable())
        {
            m_thWorker.join();
        }
    }

    int GetIterations() const { return m_iIterations; }

private:
    void Run()
    {
        uint8_t uShade = 0;
        while (m_bRunning)
        {
            ++uShade;
            for (const auto& target : m_vecTargets)
            {
                target.first->SetCell(target.first->GetViewportTop() + uShade % kRows, uShade % kCols, BgCell(TerminalRgb{uShade, 0, 0}));
                m_thread.Wake(target.second);
            }
            ++m_iIterations;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    TerminalRenderThread& m_thread;
    std::vector<std::pair<FakeFrameSource*, int>> m_vecTargets;
    std::atomic<bool> m_bRunning{true};
    std::atomic<int> m_iIterations{0};
    std::thread m_thWorker;
};
}

// 모든 테스트에서 공급원·기록·카운터는 렌더 스레드보다 먼저 선언한다: 스레드가 먼저 멈춘(Stop) 뒤에 공급원이 지워진다.
class TerminalRenderThreadTest : public QObject
{
    Q_OBJECT

private:
    void Connect(TerminalRenderThread& thread, FrameLog& log)
    {
        connect(&thread, &TerminalRenderThread::FrameReady, &thread,
                [&log](int viewId, const QImage& imgPatch, const QRect& rectPatch) { log.vecPatches.push_back({viewId, imgPatch, rectPatch}); });
    }

    void StartCpu(TerminalRenderThread& thread, FrameLog& log)
    {
        Connect(thread, log);
        thread.SetFont(BaseFont());
        thread.Start();
    }

    static bool WaitFrames(const FrameLog& log, int viewId, int count, int timeoutMs = kFrameTimeoutMs)
    {
        return QTest::qWaitFor([&] { return log.Count(viewId) >= count; }, timeoutMs);
    }

    static qint64 ElapsedMs(const std::chrono::steady_clock::time_point& start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    }

    static int CountStats(const QSignalSpy& spy, int viewId)
    {
        int iCount = 0;
        for (const QList<QVariant>& args : spy)
        {
            iCount += args.at(0).value<TerminalFrameStats>().iViewId == viewId ? 1 : 0;
        }
        return iCount;
    }

private slots:
    void initTestCase() { TestFonts::Register(); }

    void firstFrameIsFullSize()
    {
        FakeFrameSource inner(kCols, kRows, 0);
        RecordingFrameSource source(inner);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::CPU);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(iView > 0);
        thread.Wake(iView);
        QVERIFY(WaitFrames(log, iView, 1));
        const Patch& first = log.vecPatches.front();
        QCOMPARE(first.iViewId, iView);
        QCOMPARE(first.rect, QRect(QPoint(0, 0), ViewSize(1.0)));
        QCOMPARE(first.img.size(), ViewSize(1.0));
        // 첫 프레임은 붙인 뒤 첫 캡처다(Wake 가 따로 처리되면 바뀐 것 없는 두 번째 캡처가 뒤따를 수 있다)
        const std::vector<TerminalCaptureRequest> vecRequests = source.GetRequests();
        QVERIFY(!vecRequests.empty());
        QVERIFY(vecRequests.front().bFull);
        QCOMPARE(vecRequests.front().iOverscanAbove, 0);
        QCOMPARE(vecRequests.front().iOverscanBelow, 1);
    }

    void wakesAreCoalesced()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();
        const int iCapturesBefore = source.GetCaptureCount();

        for (int i = 0; i < 10; ++i)
        {
            source.SetCell(1, i, BgCell(TerminalRgb{uint8_t(20 * i), 0, 0}));
            thread.Wake(iView);
        }
        QTest::qWait(300);
        QVERIFY2(log.Count(iView) >= 1 && log.Count(iView) <= 3, qPrintable(QString::number(log.Count(iView))));
        const int iCaptures = source.GetCaptureCount() - iCapturesBefore;
        QVERIFY2(iCaptures <= 3, qPrintable(QString::number(iCaptures)));
    }

    void drawsLatestState()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));

        const QPoint point = CellCenter(3, 2);
        for (const TerminalRgb& color : {kRed, kGreen, kBlue})
        {
            source.SetCell(2, 3, BgCell(color));
            thread.Wake(iView);
        }
        QVERIFY(QTest::qWaitFor([&] { return log.Composite(iView, ViewSize(1.0)).pixel(point) == ToQRgb(kBlue); }, kFrameTimeoutMs));
        QTest::qWait(kSettleMs);
        const Patch* pLast = log.LastContaining(iView, point);
        QVERIFY(pLast);
        QCOMPARE(pLast->img.pixel(point - pLast->rect.topLeft()), ToQRgb(kBlue));
        QCOMPARE(log.Composite(iView, ViewSize(1.0)).pixel(point), ToQRgb(kBlue));
    }

    void hiddenViewIsNotRendered()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();
        const int iCapturesBefore = source.GetCaptureCount();

        thread.SetVisible(iView, false);
        source.SetCell(1, 1, BgCell(kRed));
        thread.Wake(iView);
        thread.ScrollPixels(iView, -40);
        QTest::qWait(200);
        QCOMPARE(log.Count(iView), 0);
        QCOMPARE(source.GetCaptureCount(), iCapturesBefore);   // 숨은 화면은 캡처도 하지 않는다

        thread.SetVisible(iView, true);
        QVERIFY(WaitFrames(log, iView, 1));
        QVERIFY(source.GetLastRequest().bFull);
        QCOMPARE(log.vecPatches.front().rect, QRect(QPoint(0, 0), ViewSize(1.0)));
    }

    void idleViewProducesNoFrames()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();
        const int iCapturesBefore = source.GetCaptureCount();

        QTest::qWait(kIdleWindowMs);
        QCOMPARE(log.Count(iView), 0);
        QCOMPARE(source.GetCaptureCount(), iCapturesBefore);   // 깨우지 않으면 캡처도 없다
        for (int i = 0; i < 3; ++i)
        {
            thread.Wake(iView);   // 바뀐 것 없이 깨움
        }
        QTest::qWait(kIdleWindowMs);
        QCOMPARE(log.Count(iView), 0);
        QVERIFY(source.GetCaptureCount() > iCapturesBefore);
    }

    void cursorBlinksOnlyWithFocus()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        TerminalCursor cursor;
        cursor.bVisible = true;
        cursor.bBlinking = true;
        cursor.eShape = ECursorShape::BLOCK;
        source.SetCursor(cursor);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        thread.SetFocus(iView, true);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();
        QTest::qWait(1200);
        QVERIFY2(log.Count(iView) >= 2 && log.Count(iView) <= 3, qPrintable(QString::number(log.Count(iView))));

        thread.SetFocus(iView, false);
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();
        QTest::qWait(1200);
        QCOMPARE(log.Count(iView), 0);
    }

    void pixelScrollMovesViewport()
    {
        FakeFrameSource source(kCols, kRows, 50);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        source.FillPattern(EFakePattern::MIXED, 7);
        const int iTop = source.GetViewportTop();
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();

        QElapsedTimer timer;
        timer.start();
        thread.ScrollPixels(iView, -Metrics(1.0).iCellHeight * 3.0);
        QVERIFY(QTest::qWaitFor([&] { return source.GetViewportTop() == iTop - 3; }, kFrameTimeoutMs));
        QTest::qWait(int(std::max<qint64>(0, kFrameTimeoutMs - timer.elapsed())));
        // 1초 동안 온 조각은 모두 스크롤 애니메이션의 프레임이다(한 번에 뛰지 않고 여러 프레임으로 움직인다).
        QVERIFY2(log.Count(iView) >= 3, qPrintable(QString::number(log.Count(iView))));
        QCOMPARE(source.GetViewportTop(), iTop - 3);   // 지나치지 않는다
        const int iFrames = log.Count(iView);
        QTest::qWait(200);
        QCOMPARE(log.Count(iView), iFrames);           // 애니메이션이 끝나면 프레임도 멈춘다
    }

    void refreshRatePacesScrollFrames()
    {
        // 같은 스크롤을 60Hz 화면과 120Hz 화면에서 차례로 하고, 처음 구간의 프레임 수를 비교한다.
        constexpr int kScrollLines = 20;
        constexpr int kWindowMs = 250;
        FakeFrameSource source60(kCols, kRows, 50);
        FakeFrameSource source120(kCols, kRows, 50);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        source60.FillPattern(EFakePattern::MIXED, 5);
        source120.FillPattern(EFakePattern::MIXED, 5);
        const int iView60 = thread.AttachView(&source60, {}, ViewSize(1.0), 1.0);
        const int iView120 = thread.AttachView(&source120, {}, ViewSize(1.0), 1.0);
        thread.SetRefreshRate(iView60, 60.0);
        thread.SetRefreshRate(iView120, 120.0);
        thread.SetRefreshRate(iView120, 0.0);     // 무시
        thread.SetRefreshRate(iView120, -30.0);   // 무시
        QVERIFY(WaitFrames(log, iView60, 1));
        QVERIFY(WaitFrames(log, iView120, 1));
        QTest::qWait(kSettleMs);

        const double dPixels = -Metrics(1.0).iCellHeight * double(kScrollLines);
        const auto funcScrollFrames = [&](int viewId)
        {
            log.vecPatches.clear();
            thread.ScrollPixels(viewId, dPixels);
            QTest::qWait(kWindowMs);
            return log.Count(viewId);
        };
        const int iFrames60 = funcScrollFrames(iView60);
        QTest::qWait(kScrollSettleMs);   // 60Hz 애니메이션이 끝날 때까지
        const int iFrames120 = funcScrollFrames(iView120);
        const QString strCounts = QStringLiteral("60Hz %1, 120Hz %2").arg(iFrames60).arg(iFrames120);
        QVERIFY2(iFrames120 * 2 >= iFrames60 * 3, qPrintable(strCounts));       // 1.5배 이상
        QVERIFY2(iFrames120 * 1000 > kWindowMs * 60, qPrintable(strCounts));   // 60fps 보다 많다
        QTest::qWait(kScrollSettleMs);
        QCOMPARE(source120.GetViewportTop(), source60.GetViewportTop());
    }

    void scrollPastBottomStopsAtEdge()
    {
        FakeFrameSource source(kCols, kRows, 50);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        source.FillPattern(EFakePattern::MIXED, 3);
        const int iTop = source.GetViewportTop();   // 맨 아래(최근)
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();

        thread.ScrollPixels(iView, Metrics(1.0).iCellHeight * 2.0);
        QTest::qWait(300);
        QCOMPARE(source.GetViewportTop(), iTop);
        log.vecPatches.clear();
        QTest::qWait(200);
        QCOMPARE(log.Count(iView), 0);
    }

    void scaleChangeRedrawsFullAtNewSize()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        source.FillPattern(EFakePattern::MIXED, 1);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();

        const QSize doubled = ViewSize(1.0) * 2;
        thread.ResizeView(iView, doubled, 2.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QVERIFY(source.GetLastRequest().bFull);
        QCOMPARE(log.vecPatches.front().rect, QRect(QPoint(0, 0), doubled));
        QCOMPARE(log.vecPatches.front().img.size(), doubled);
    }

    void zeroSizeStopsDrawingUntilRestored()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();

        thread.ResizeView(iView, QSize(0, 0), 1.0);
        QTest::qWait(50);
        const int iCapturesBefore = source.GetCaptureCount();
        source.SetCell(0, 0, BgCell(kGreen));
        thread.Wake(iView);
        QTest::qWait(200);
        QCOMPARE(log.Count(iView), 0);
        QCOMPARE(source.GetCaptureCount(), iCapturesBefore);

        thread.ResizeView(iView, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QVERIFY(source.GetLastRequest().bFull);
        QCOMPARE(log.vecPatches.front().rect, QRect(QPoint(0, 0), ViewSize(1.0)));
        QCOMPARE(log.vecPatches.front().img.pixel(CellCenter(0, 0)), ToQRgb(kGreen));
    }

    void themeChangeRequestsFullFrame()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();

        TerminalTheme theme;
        theme.selectionBg = TerminalRgb{1, 2, 3};
        theme.cursorColor = TerminalRgb{200, 100, 0};
        thread.SetTheme(theme);
        QVERIFY(WaitFrames(log, iView, 1));
        QVERIFY(source.GetLastRequest().bFull);
    }

    void fontChangeRequestsFullFrame()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        source.FillPattern(EFakePattern::MIXED, 2);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();

        QFont bigger = BaseFont();
        bigger.setPixelSize(20);
        thread.SetFont(bigger);
        QVERIFY(WaitFrames(log, iView, 1));
        QVERIFY(source.GetLastRequest().bFull);
        QCOMPARE(log.vecPatches.front().rect, QRect(QPoint(0, 0), ViewSize(1.0)));
    }

    void detachDuringFrameStopsFrames()
    {
        FakeFrameSource innerA(kCols, kRows, 0);
        BlockingFrameSource sourceA(innerA);
        FakeFrameSource sourceB(kCols, kRows, 0);
        std::atomic<int> iReleases{0};
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        QSignalSpy spyStats(&thread, &TerminalRenderThread::FrameStats);
        thread.SetStatsEnabled(true);
        const int iViewA = thread.AttachView(&sourceA, CountingHandle(iReleases), ViewSize(1.0), 1.0);
        const int iViewB = thread.AttachView(&sourceB, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iViewA, 1));
        QVERIFY(WaitFrames(log, iViewB, 1));
        QTest::qWait(kSettleMs);
        QVERIFY(CountStats(spyStats, iViewA) >= 1);

        sourceA.Arm();
        innerA.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iViewA);
        QVERIFY(sourceA.WaitEntered(kFrameTimeoutMs));   // 렌더 스레드가 A 의 캡처 안에서 멈춤
        log.vecPatches.clear();
        spyStats.clear();
        thread.DetachView(iViewA);
        sourceB.SetCell(0, 0, BgCell(kGreen));
        thread.Wake(iViewB);
        sourceA.Release();

        QVERIFY(WaitFrames(log, iViewB, 1));               // 렌더 스레드는 계속 돈다
        QVERIFY(QTest::qWaitFor([&] { return iReleases == 1; }, kFrameTimeoutMs));
        const int iCapturesAfterDetach = innerA.GetCaptureCount();
        innerA.SetCell(0, 1, BgCell(kBlue));
        thread.Wake(iViewA);
        thread.ScrollPixels(iViewA, 10);
        thread.SetVisible(iViewA, true);
        QTest::qWait(200);
        QCOMPARE(log.Count(iViewA), 0);                    // 떼어 낸 뒤에는 그 id 의 조각이 없다
        QCOMPARE(CountStats(spyStats, iViewA), 0);         // 측정값도 없다(그리던 프레임 것까지)
        QVERIFY(CountStats(spyStats, iViewB) >= 1);
        QCOMPARE(innerA.GetCaptureCount(), iCapturesAfterDetach);   // 공급원도 더 쓰지 않는다
        QCOMPARE(iReleases.load(), 1);
    }

    void asyncDetachEmitsViewDetached()
    {
        FakeFrameSource source(kCols, kRows, 0);
        std::atomic<int> iReleases{0};
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        int iReleasesAtSignal = -1;
        connect(&thread, &TerminalRenderThread::ViewDetached, &thread, [&](int) { iReleasesAtSignal = iReleases; });
        const int iView = thread.AttachView(&source, CountingHandle(iReleases), ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));

        Spammer spammer(thread, {{&source, iView}});
        QTest::qWait(50);
        thread.DetachView(iView);
        QVERIFY(spyDetached.wait(kFrameTimeoutMs));
        QCOMPARE(spyDetached.count(), 1);
        QCOMPARE(spyDetached.at(0).at(0).toInt(), iView);
        QCOMPARE(iReleasesAtSignal, 1);                    // 핸들을 놓은 뒤에 알린다
        const int iCaptures = source.GetCaptureCount();
        QTest::qWait(200);                                 // 그동안에도 다른 스레드는 계속 Wake 한다
        QCOMPARE(source.GetCaptureCount(), iCaptures);     // 알린 뒤에는 공급원을 쓰지 않는다
        QCOMPARE(spyDetached.count(), 1);
        spammer.Join();
    }

    void detachViewAndWaitReleasesHandle()
    {
        auto upSource = std::make_unique<FakeFrameSource>(kCols, kRows, 0);
        std::atomic<int> iReleases{0};
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(upSource.get(), CountingHandle(iReleases), ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));

        Spammer spammer(thread, {{upSource.get(), iView}});
        QTest::qWait(50);
        const auto start = std::chrono::steady_clock::now();
        QVERIFY(thread.DetachViewAndWait(iView, 500));
        QVERIFY(ElapsedMs(start) < 500);
        QCOMPARE(iReleases.load(), 1);
        spammer.Join();
        upSource.reset();   // 참을 돌려받았으면 렌더 스레드는 더 이상 공급원을 쓰지 않는다
        thread.Wake(iView);
        QTest::qWait(50);
        QCOMPARE(iReleases.load(), 1);
    }

    void detachViewAndWaitTimesOutWithoutDeadlock()
    {
        FakeFrameSource inner(kCols, kRows, 0);
        BlockingFrameSource source(inner);
        std::atomic<int> iReleases{0};
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, CountingHandle(iReleases), ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));

        source.Arm();
        inner.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iView);
        QVERIFY(source.WaitEntered(kFrameTimeoutMs));
        const auto start = std::chrono::steady_clock::now();
        QVERIFY(!thread.DetachViewAndWait(iView, 100));
        const qint64 llElapsed = ElapsedMs(start);
        QVERIFY2(llElapsed >= 90 && llElapsed < 400, qPrintable(QString::number(llElapsed)));
        QCOMPARE(iReleases.load(), 0);
        source.Release();
        QVERIFY(QTest::qWaitFor([&] { return iReleases == 1; }, kFrameTimeoutMs));
    }

    void stopReleasesHandlesAndRestartWorks()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FakeFrameSource other(kCols, kRows, 0);
        std::atomic<int> iReleases{0};
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        const int iView = thread.AttachView(&source, CountingHandle(iReleases), ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));

        thread.Stop();
        QCOMPARE(iReleases.load(), 1);
        QVERIFY(QTest::qWaitFor([&] { return spyDetached.count() >= 1; }, kFrameTimeoutMs));   // 멈출 때 뗀 화면도 알린다
        QCOMPARE(spyDetached.at(0).at(0).toInt(), iView);
        auto start = std::chrono::steady_clock::now();
        QVERIFY(!thread.DetachViewAndWait(iView, 500));   // 멈춘 스레드는 기다리지 않는다
        QVERIFY(ElapsedMs(start) < 100);
        thread.Stop();                                     // 두 번 불러도 된다

        thread.Start();
        log.vecPatches.clear();
        spyDetached.clear();
        const int iOther = thread.AttachView(&other, CountingHandle(iReleases), ViewSize(1.0), 1.0);
        QVERIFY(iOther != iView);
        QVERIFY(WaitFrames(log, iOther, 1));
        start = std::chrono::steady_clock::now();
        QVERIFY(thread.DetachViewAndWait(iOther, 500));
        QVERIFY(ElapsedMs(start) < 500);
        QCOMPARE(iReleases.load(), 2);
        QVERIFY(QTest::qWaitFor([&] { return spyDetached.count() >= 1; }, kFrameTimeoutMs));
        QCOMPARE(spyDetached.last().at(0).toInt(), iOther);
    }

    void pendingAttachIsReleasedWithoutStart()
    {
        std::atomic<int> iReleases{0};
        FakeFrameSource source(kCols, kRows, 0);
        {
            TerminalRenderThread thread(ETerminalRendererMode::AUTO);
            thread.AttachView(&source, CountingHandle(iReleases), ViewSize(1.0), 1.0);
        }
        QCOMPARE(iReleases.load(), 1);
        QCOMPARE(source.GetCaptureCount(), 0);
    }

    void stopReturnsWithinOneSecondUnderLoad()
    {
        std::vector<std::unique_ptr<FakeFrameSource>> vecSources;
        std::vector<std::pair<FakeFrameSource*, int>> vecTargets;
        std::atomic<int> iReleases{0};
        FrameLog log;
        for (int i = 0; i < 4; ++i)
        {
            vecSources.push_back(std::make_unique<FakeFrameSource>(kCols, kRows, 0));
            vecSources.back()->FillPattern(EFakePattern::FLOOD, uint32_t(i));
        }
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        for (const auto& upSource : vecSources)
        {
            const int iView = thread.AttachView(upSource.get(), CountingHandle(iReleases), ViewSize(1.0), 1.0);
            vecTargets.push_back({upSource.get(), iView});
        }
        for (const auto& target : vecTargets)
        {
            QVERIFY(WaitFrames(log, target.second, 1));
        }

        Spammer spammer(thread, vecTargets);
        QTest::qWait(200);
        QVERIFY(spammer.GetIterations() > 10);
        const auto start = std::chrono::steady_clock::now();
        thread.Stop();
        const qint64 llElapsed = ElapsedMs(start);
        QVERIFY2(llElapsed < 1000, qPrintable(QString::number(llElapsed)));
        QCOMPARE(iReleases.load(), 4);
        spammer.Join();
    }

    void statsOnlyWhenEnabled()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        QSignalSpy spyStats(&thread, &TerminalRenderThread::FrameStats);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        source.SetCell(1, 1, BgCell(kRed));
        thread.Wake(iView);
        QVERIFY(WaitFrames(log, iView, 2));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyStats.count(), 0);

        thread.SetStatsEnabled(true);
        source.SetCell(1, 2, BgCell(kGreen));
        thread.Wake(iView);
        QVERIFY(spyStats.wait(kFrameTimeoutMs));
        const TerminalFrameStats stats = spyStats.takeFirst().at(0).value<TerminalFrameStats>();
        QCOMPARE(stats.iViewId, iView);
        QVERIFY(stats.llWakeNs > 0);
        QVERIFY(stats.llWakeNs <= stats.llCaptureDoneNs);
        QVERIFY(stats.llCaptureDoneNs <= stats.llBuildDoneNs);
        QVERIFY(stats.llBuildDoneNs <= stats.llExecuteStartNs);
        QVERIFY(stats.llExecuteStartNs <= stats.llExecuteEndNs);
        QCOMPARE(stats.llPresentCallNs, stats.llExecuteEndNs);
        QCOMPARE(stats.iNotReadyRetries, 0);
        QCOMPARE(stats.iPresentMode, -1);   // CPU Strategy 는 표시 모드가 없다
        QVERIFY(stats.iQuadCount > 0);
        QCOMPARE(stats.iRebuiltRows, 1);
    }

    void atlasOverflowSkipsFrameWithoutLooping()
    {
        // 화면 하나가 아틀라스보다 많은 글리프를 쓰면 FULL 로 다시 해도 NEED_FULL_FRAME 이다.
        // 박스·블록 문자(U+2500–259F, 160자)는 셀 크기 비트맵을 도형으로 그려 빠르므로, 큰 글꼴로 아틀라스를 넘치게 한다.
        constexpr int kHugePixelSize = 500;
        constexpr int kBoxCols = 20;
        constexpr int kBoxRows = 8;
        constexpr uint32_t kFirstBox = 0x2500;
        QFont huge = BaseFont();
        huge.setPixelSize(kHugePixelSize);
        FakeFrameSource source(kBoxCols, kBoxRows, 0);
        for (int iRow = 0; iRow < kBoxRows; ++iRow)
        {
            for (int iCol = 0; iCol < kBoxCols; ++iCol)
            {
                TerminalCell cell;
                cell.uCodepoint = kFirstBox + uint32_t(iRow * kBoxCols + iCol);
                source.SetCell(iRow, iCol, cell);
            }
        }
        {
            // 전제: 이 글꼴로는 화면의 글리프가 아틀라스에 다 들어가지 않는다
            GlyphCache cache(huge, 1.0);
            for (int i = 0; i < kBoxCols * kBoxRows; ++i)
            {
                cache.GetGlyph(std::u32string(1, char32_t(kFirstBox + uint32_t(i))), false, false, 1);
            }
            QVERIFY(cache.GetGeneration() > 0);
        }

        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        Connect(thread, log);
        thread.SetFont(huge);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("needs more glyphs than the atlas")));   // 로그 한 줄
        thread.Start();
        const int iView = thread.AttachView(&source, {}, QSize(200, 100), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return source.GetCaptureCount() >= 2; }, 10000));
        QTest::qWait(300);
        QCOMPARE(source.GetCaptureCount(), 2);   // FULL 한 번 더 해 보고 그 프레임은 건너뛴다(반복하지 않음)
        QCOMPARE(log.Count(iView), 0);

        thread.Wake(iView);                      // 다음 차례에 다시 해 본다
        QVERIFY(QTest::qWaitFor([&] { return source.GetCaptureCount() >= 4; }, 10000));
        QTest::qWait(300);
        QCOMPARE(source.GetCaptureCount(), 4);
        QVERIFY(source.GetLastRequest().bFull);

        const auto start = std::chrono::steady_clock::now();
        thread.Stop();
        QVERIFY(ElapsedMs(start) < 1000);
    }

    void sourceReleasedAfterItsLastViewDetaches()
    {
        // 공급원 하나를 두 화면이 쓴다: 둘 다 떨어진 뒤에만 SourceReleased. Stop 은 남은 공급원을 돌아오기 전에 모두 알린다.
        FakeFrameSource shared(kCols, kRows, 0);
        FakeFrameSource other(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyReleased(&thread, &TerminalRenderThread::SourceReleased);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        StartCpu(thread, log);
        const int iA = thread.AttachView(&shared, {}, ViewSize(1.0), 1.0);
        const int iB = thread.AttachView(&shared, {}, ViewSize(1.0), 1.0);
        const int iOther = thread.AttachView(&other, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iA, 1));
        QVERIFY(WaitFrames(log, iB, 1));
        QVERIFY(WaitFrames(log, iOther, 1));

        thread.DetachView(iA);
        QVERIFY(spyDetached.wait(kFrameTimeoutMs));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyReleased.count(), 0);   // B 가 아직 쓴다
        thread.DetachView(iB);
        QVERIFY(spyReleased.wait(kFrameTimeoutMs));
        QCOMPARE(spyReleased.count(), 1);
        QCOMPARE(spyReleased.at(0).at(0).value<ITerminalFrameSource*>(), static_cast<ITerminalFrameSource*>(&shared));
        thread.DetachView(iB);   // 같은 id 를 또 떼도 다시 알리지 않는다
        QTest::qWait(kSettleMs);
        QCOMPARE(spyReleased.count(), 1);

        thread.Stop();
        QCOMPARE(spyReleased.count(), 2);
        QCOMPARE(spyReleased.at(1).at(0).value<ITerminalFrameSource*>(), static_cast<ITerminalFrameSource*>(&other));
    }

    void sourceReleasedOnlyAfterTimedOutDetachFinishes()
    {
        // GPU 위젯은 지울 때 DetachViewAndWait(500ms) 를 쓴다. 시간이 넘으면 렌더 스레드가 아직 공급원을 쓰므로
        // 공급원은 SourceReleased 가 온 뒤에만 놓는다.
        FakeFrameSource inner(kCols, kRows, 0);
        BlockingFrameSource source(inner);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyReleased(&thread, &TerminalRenderThread::SourceReleased);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));

        source.Arm();
        inner.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iView);
        QVERIFY(source.WaitEntered(kFrameTimeoutMs));
        QVERIFY(!thread.DetachViewAndWait(iView, 100));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyReleased.count(), 0);   // 아직 캡처 안에 있다
        const int iCaptures = inner.GetCaptureCount();
        source.Release();
        QVERIFY(spyReleased.wait(kFrameTimeoutMs));
        QCOMPARE(spyReleased.at(0).at(0).value<ITerminalFrameSource*>(), static_cast<ITerminalFrameSource*>(&source));
        thread.Wake(iView);
        QTest::qWait(kSettleMs);
        QCOMPARE(inner.GetCaptureCount(), iCaptures + 1);   // 막혀 있던 캡처 하나만 끝났다
    }

    void heldSourceIsReleasedOnlyWhenUnheld()
    {
        // RetainSource 로 잡힌 공급원(살아 있는 위젯의 공급원)은 붙은 화면이 없어도, Stop 뒤에도 알리지 않는다.
        // ReleaseSource 로 놓을 때 렌더 스레드가 더 쓰지 않으면 그 자리에서 알린다.
        FakeFrameSource source(kCols, kRows, 0);
        FakeFrameSource neverAttached(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyReleased(&thread, &TerminalRenderThread::SourceReleased);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        StartCpu(thread, log);
        thread.RetainSource(&source);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        thread.DetachView(iView);
        QVERIFY(spyDetached.wait(kFrameTimeoutMs));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyReleased.count(), 0);   // 잡혀 있다(위젯이 다시 붙을 수 있다)
        thread.Stop();
        QCOMPARE(spyReleased.count(), 0);   // Stop 도 잡힌 공급원은 알리지 않는다
        thread.ReleaseSource(&source);
        QCOMPARE(spyReleased.count(), 1);   // 놓는 자리에서 바로
        QCOMPARE(spyReleased.at(0).at(0).value<ITerminalFrameSource*>(), static_cast<ITerminalFrameSource*>(&source));

        thread.RetainSource(&neverAttached);   // 한 번도 붙지 않은 위젯
        thread.ReleaseSource(&neverAttached);
        QCOMPARE(spyReleased.count(), 2);
        QCOMPARE(spyReleased.at(1).at(0).value<ITerminalFrameSource*>(), static_cast<ITerminalFrameSource*>(&neverAttached));
    }
};

QObject* CreateTerminalRenderThreadTest() { return new TerminalRenderThreadTest(); }

#include "TerminalRenderThreadTest.moc"
