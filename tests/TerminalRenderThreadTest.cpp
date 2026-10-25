#include "TerminalViewer/Thread/TerminalRenderThread.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "TerminalViewer/Testing/FakeFrameSource.h"
#include "TestFonts.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
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

// 렌더 스레드의 측정값(TerminalFrameStats)과 같은 시계
qint64 SteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 다른 스레드에서 1ms 마다 정확히(타이머 해상도와 상관없이 바쁜 대기로) step(k) 를 부른다: 1kHz 로 쏟는 IO 스레드 흉내.
// (Spammer 의 sleep_for(1ms) 는 Windows 기본 타이머 해상도에서 약 15.6ms 라 1kHz 가 되지 않는다)
class PacedFlood
{
public:
    explicit PacedFlood(std::function<void(int)> funcStep)
        : m_funcStep(std::move(funcStep))
        , m_thWorker([this] { Run(); })
    {
    }

    ~PacedFlood() { Join(); }

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
        constexpr qint64 kPeriodNs = 1'000'000;
        qint64 llNext = SteadyNowNs();
        int iStep = 0;
        while (m_bRunning)
        {
            m_funcStep(++iStep);
            ++m_iIterations;
            llNext += kPeriodNs;
            while (m_bRunning && SteadyNowNs() < llNext)
            {
                std::this_thread::yield();
            }
        }
    }

    std::function<void(int)> m_funcStep;
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

    // 실제 GUI 스레드처럼 이벤트 루프를 돌며 기다린다(이벤트가 오면 바로 처리한다). QTest::qWait 는 이벤트를 한 번 처리한 뒤
    // 10ms 씩 자므로 그동안 조각을 받지 않는다: CPU 조각은 GUI 가 받아야 다음을 그리므로(받지 않은 조각은 하나) 프레임 수를 세는
    // 구간에서는 이것으로 기다린다.
    static void RunEventLoop(int ms)
    {
        QEventLoop loop;
        QTimer::singleShot(ms, Qt::PreciseTimer, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // 공급원 kStopSources 개를 만들어 sources 에 넣고 붙인 뒤 첫 프레임까지 기다린다(Stop 이 여러 공급원을 알리는 시험)
    static bool AttachStopSources(TerminalRenderThread& thread, FrameLog& log, std::vector<std::unique_ptr<FakeFrameSource>>& sources)
    {
        constexpr int kStopSources = 3;
        for (int iSource = 0; iSource < kStopSources; ++iSource)
        {
            sources.push_back(std::make_unique<FakeFrameSource>(kCols, kRows, 0));
            const int iView = thread.AttachView(sources.back().get(), {}, ViewSize(1.0), 1.0);
            if (!WaitFrames(log, iView, 1))
            {
                return false;
            }
        }
        return true;
    }

    // 알린 공급원(released)이 붙인 공급원(sources)마다 정확히 한 번인가
    static bool EachReleasedOnce(const std::vector<ITerminalFrameSource*>& released,
                                 const std::vector<std::unique_ptr<FakeFrameSource>>& sources)
    {
        if (released.size() != sources.size())
        {
            return false;
        }
        for (const auto& upSource : sources)
        {
            if (std::count(released.begin(), released.end(), static_cast<ITerminalFrameSource*>(upSource.get())) != 1)
            {
                return false;
            }
        }
        return true;
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
            RunEventLoop(kWindowMs);
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

    // 최소 대비만 바꿔도 SetTheme 가 행 캐시를 버리고 전체를 다시 그린다. 되돌리면 처음 그림과 같다
    void minContrastChangeRedraws()
    {
        FakeFrameSource source(kCols, kRows, 0);
        TerminalCell style;
        style.fg = TerminalRgb{0, 0, 140};   // 검정 배경 위 어두운 파랑(대비 1.4)
        source.SetText(0, 0, U"MMMM", style);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        const CellMetrics metrics = Metrics(1.0);
        const QRect rectText(kPaddingLogical, kPaddingLogical, 4 * metrics.iCellWidth, metrics.iCellHeight);
        const QImage imgOff = log.Composite(iView, ViewSize(1.0)).copy(rectText);
        const auto funcBrightest = [](const QImage& image)
        {
            int iMax = 0;
            for (int iY = 0; iY < image.height(); ++iY)
                for (int iX = 0; iX < image.width(); ++iX)
                    iMax = std::max(iMax, qGray(image.pixel(iX, iY)));
            return iMax;
        };

        TerminalTheme theme = thread.GetTheme();
        theme.dMinContrastRatio = 4.5;
        int iFrames = log.Count(iView);
        thread.SetTheme(theme);
        QVERIFY(WaitFrames(log, iView, iFrames + 1));
        QVERIFY(source.GetLastRequest().bFull);
        QTest::qWait(kSettleMs);
        const QImage imgOn = log.Composite(iView, ViewSize(1.0)).copy(rectText);
        QVERIFY(imgOn != imgOff);
        QVERIFY(funcBrightest(imgOn) > funcBrightest(imgOff));   // 밝게 고쳤다

        theme.dMinContrastRatio = 1.0;
        iFrames = log.Count(iView);
        thread.SetTheme(theme);
        QVERIFY(WaitFrames(log, iView, iFrames + 1));
        QTest::qWait(kSettleMs);
        QCOMPARE(log.Composite(iView, ViewSize(1.0)).copy(rectText), imgOff);
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

    void wakeFloodIsCappedAtRefreshRate()
    {
        // 다른 스레드가 1kHz 로 내용을 바꾸고 Wake 해도 그 화면은 주사율(60Hz)만큼만 그린다(주기당 한 번)
        constexpr int kWindowMs = 1000;
        constexpr int kMaxFps = 72;    // 60Hz + 20%
        constexpr int kMinFps = 30;    // 60Hz 의 절반
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        thread.SetRefreshRate(iView, 60.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);

        PacedFlood flood([&](int step)
        {
            source.SetCell(source.GetViewportTop() + step % kRows, step % kCols, BgCell(TerminalRgb{uint8_t(step), 0, 0}));
            thread.Wake(iView);
        });
        QTest::qWait(kSettleMs);
        log.vecPatches.clear();
        const int iStepsBefore = flood.GetIterations();
        QElapsedTimer timer;
        timer.start();
        RunEventLoop(kWindowMs);
        const int iFrames = log.Count(iView);
        const qint64 llElapsedMs = timer.elapsed();
        const int iWakes = flood.GetIterations() - iStepsBefore;
        flood.Join();
        const QString strInfo = QStringLiteral("frames %1, wakes %2 in %3 ms").arg(iFrames).arg(iWakes).arg(llElapsedMs);
        QVERIFY2(iWakes * 2 > kWindowMs, qPrintable(strInfo));   // 전제: 실제로 1kHz 가까이 쏟았다
        QVERIFY2(iFrames <= llElapsedMs * kMaxFps / 1000, qPrintable(strInfo));
        QVERIFY2(iFrames >= llElapsedMs * kMinFps / 1000, qPrintable(strInfo));
    }

    void wakeFloodKeepsRefreshPhase()
    {
        // 쏟는 동안의 프레임은 마감(이전 마감 + 간격)에 그린다: 렌더 스레드가 마감보다 조금씩 늦게 깨어나도(1ms 타이머) 늦음이 쌓여
        // 주사율보다 덜 그리지 않는다. 늦음이 간격에 견줘 크게 보이게 240Hz(4.17ms)로 잰다(늦음이 쌓이면 0.5ms 에 약 215fps).
        constexpr double kHz = 240.0;
        constexpr int kWindowMs = 1000;
        constexpr double kMinRatio = 0.95;
        constexpr double kMaxRatio = 1.02;
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        thread.SetRefreshRate(iView, kHz);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);

        PacedFlood flood([&](int step)
        {
            source.SetCell(source.GetViewportTop() + step % kRows, step % kCols, BgCell(TerminalRgb{uint8_t(step), 0, 0}));
            thread.Wake(iView);
        });
        RunEventLoop(kSettleMs);
        log.vecPatches.clear();
        QElapsedTimer timer;
        timer.start();
        RunEventLoop(kWindowMs);
        const int iFrames = log.Count(iView);
        const qint64 llElapsedMs = timer.elapsed();
        flood.Join();
        const double dExpected = kHz * double(llElapsedMs) / 1000.0;
        const QString strInfo = QStringLiteral("frames %1 in %2 ms, expected %3").arg(iFrames).arg(llElapsedMs).arg(dExpected);
        QVERIFY2(iFrames >= dExpected * kMinRatio, qPrintable(strInfo));
        QVERIFY2(iFrames <= dExpected * kMaxRatio + 1, qPrintable(strInfo));
    }

    void blockedGuiQueuesAtMostOnePatch()
    {
        // GUI 이벤트 루프가 막힌 동안 렌더 스레드는 그 화면의 조각을 하나만 내고 기다린다(GUI 가 받지 않은 조각은 최대 하나).
        // 막힌 동안 실행한 프레임 = GUI 에 쌓인 FrameReady. 풀리면 다시 그린다.
        constexpr int kBlockMs = 200;
        constexpr int kMaxQueued = 2;
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        QSignalSpy spyStats(&thread, &TerminalRenderThread::FrameStats);
        thread.SetStatsEnabled(true);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        thread.SetRefreshRate(iView, 60.0);
        QVERIFY(WaitFrames(log, iView, 1));

        PacedFlood flood([&](int step)
        {
            source.SetCell(source.GetViewportTop() + step % kRows, step % kCols, BgCell(TerminalRgb{uint8_t(step), 0, 0}));
            thread.Wake(iView);
        });
        QTest::qWait(kSettleMs);
        const qint64 llBlockStart = SteadyNowNs();
        std::this_thread::sleep_for(std::chrono::milliseconds(kBlockMs));   // GUI 스레드(이 테스트)가 이벤트를 처리하지 않는다
        const qint64 llBlockEnd = SteadyNowNs();
        const int iPatchesBefore = log.Count(iView);
        QTest::qWait(kSettleMs * 2);
        const int iPatchesAfter = log.Count(iView);
        flood.Join();

        int iDuringBlock = 0;
        for (const QList<QVariant>& args : spyStats)
        {
            const TerminalFrameStats stats = args.at(0).value<TerminalFrameStats>();
            if (stats.iViewId == iView && stats.llExecuteEndNs > llBlockStart && stats.llExecuteEndNs <= llBlockEnd)
            {
                ++iDuringBlock;
            }
        }
        const QString strInfo = QStringLiteral("frames during block %1, patches after unblocking %2")
                                    .arg(iDuringBlock)
                                    .arg(iPatchesAfter - iPatchesBefore);
        QVERIFY2(iDuringBlock <= kMaxQueued, qPrintable(strInfo));
        QVERIFY2(iPatchesAfter - iPatchesBefore >= 3, qPrintable(strInfo));   // 풀리면 계속 그린다
    }

    void latestStateShownAfterBlockedGui()
    {
        // GUI 가 막힌 동안 여러 줄이 바뀌고 그대로 출력이 멈춰도, 풀리면 조각을 덮어 쓴 화면이 마지막 상태와 정확히 같다
        // (실행을 건너뛴 뒤의 조각이 그사이 바뀐 줄을 모두 덮는다). 기준: 같은 내용의 공급원을 새 화면으로 처음부터 그린 그림.
        FakeFrameSource source(kCols, kRows, 0);
        FakeFrameSource reference(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        thread.SetRefreshRate(iView, 60.0);
        QVERIFY(WaitFrames(log, iView, 1));

        PacedFlood flood([&](int step)
        {
            const int iRow = step % kRows;
            const int iCol = (step / kRows) % kCols;
            const TerminalCell cell = BgCell(TerminalRgb{uint8_t(step * 7), uint8_t(step * 13), uint8_t(step)});
            source.SetCell(source.GetViewportTop() + iRow, iCol, cell);
            reference.SetCell(reference.GetViewportTop() + iRow, iCol, cell);
            thread.Wake(iView);
        });
        QTest::qWait(kSettleMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));   // GUI 막힘: 그동안 모든 줄이 여러 번 바뀐다
        flood.Join();                                                  // 막힌 채로 출력이 멈춘다
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        QVERIFY(flood.GetIterations() > 2 * kRows * kCols);
        QTest::qWait(kSettleMs * 3);

        const int iReference = thread.AttachView(&reference, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iReference, 1));
        const Patch* pReference = log.Last(iReference);
        QVERIFY(pReference);
        QCOMPARE(pReference->rect, QRect(QPoint(0, 0), ViewSize(1.0)));
        QCOMPARE(log.Composite(iView, ViewSize(1.0)), pReference->img);
    }

    void firstWakeAfterIdleIsNotDelayed()
    {
        // 한가한 뒤(마지막으로 그린 뒤 한 간격 이상)의 Wake 는 프레임 간격을 기다리지 않고 바로 그린다(키 입력 지연)
        constexpr int kProbes = 10;
        constexpr int kProbeGapMs = 100;
        constexpr double kMaxMedianMs = 4.0;    // 60Hz 간격(16.7ms)보다 훨씬 작다. 이 PC 에서 0.5ms 안팎
        constexpr double kSlowMs = 8.0;
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        QSignalSpy spyStats(&thread, &TerminalRenderThread::FrameStats);
        thread.SetStatsEnabled(true);
        const int iView = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        thread.SetRefreshRate(iView, 60.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);
        spyStats.clear();

        for (int iProbe = 0; iProbe < kProbes; ++iProbe)
        {
            source.SetCell(1, iProbe, BgCell(TerminalRgb{uint8_t(20 * iProbe), 0, 0}));
            thread.Wake(iView);
            QTest::qWait(kProbeGapMs);
        }
        std::vector<double> vecLatencyMs;
        for (const QList<QVariant>& args : spyStats)
        {
            const TerminalFrameStats stats = args.at(0).value<TerminalFrameStats>();
            if (stats.iViewId == iView && stats.llWakeNs > 0)
            {
                vecLatencyMs.push_back(double(stats.llExecuteEndNs - stats.llWakeNs) / 1e6);
            }
        }
        QCOMPARE(int(vecLatencyMs.size()), kProbes);
        std::sort(vecLatencyMs.begin(), vecLatencyMs.end());
        const double dMedianMs = vecLatencyMs[kProbes / 2];
        const int iSlow = int(std::count_if(vecLatencyMs.begin(), vecLatencyMs.end(), [](double ms) { return ms > kSlowMs; }));
        const QString strInfo = QStringLiteral("median %1 ms, max %2 ms, over %3 ms: %4")
                                    .arg(dMedianMs).arg(vecLatencyMs.back()).arg(kSlowMs).arg(iSlow);
        QVERIFY2(dMedianMs < kMaxMedianMs, qPrintable(strInfo));
        QVERIFY2(iSlow <= 1, qPrintable(strInfo));
    }

    void pendingPatchDoesNotBlockDetachOrStop()
    {
        // GUI 가 받지 않은 조각이 남은 채로 떼거나 멈춰도 막히지 않는다. 뗀 화면의 늦은 조각은 버리고(확인만 간다),
        // 같은 공급원을 새 id 로 붙인 화면은 바로 그린다.
        FakeFrameSource source(kCols, kRows, 0);
        FakeFrameSource other(kCols, kRows, 0);
        std::atomic<int> iReleases{0};
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        StartCpu(thread, log);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        const int iView = thread.AttachView(&source, CountingHandle(iReleases), ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iView, 1));
        QTest::qWait(kSettleMs);

        // 이벤트를 처리하지 않고(조각을 받지 않고) 바꾸고 떼고 다시 붙인다
        log.vecPatches.clear();
        source.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iView);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        source.SetCell(0, 1, BgCell(kGreen));
        thread.Wake(iView);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        thread.DetachView(iView);
        const int iAgain = thread.AttachView(&source, CountingHandle(iReleases), ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iAgain, 1));
        QVERIFY(QTest::qWaitFor([&] { return spyDetached.count() >= 1; }, kFrameTimeoutMs));
        QCOMPARE(iReleases.load(), 1);
        QCOMPARE(log.Count(iView), 0);   // 뗀 뒤에 온 조각은 버린다
        QVERIFY(QTest::qWaitFor([&] { return log.Composite(iAgain, ViewSize(1.0)).pixel(CellCenter(1, 0)) == ToQRgb(kGreen); },
                                kFrameTimeoutMs));

        // 다른 화면도 조각을 받지 않은 채로 Stop
        const int iOther = thread.AttachView(&other, CountingHandle(iReleases), ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iOther, 1));
        other.SetCell(1, 1, BgCell(kBlue));
        thread.Wake(iOther);
        source.SetCell(1, 1, BgCell(kBlue));
        thread.Wake(iAgain);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto start = std::chrono::steady_clock::now();
        thread.Stop();
        QVERIFY(ElapsedMs(start) < 1000);
        QCOMPARE(iReleases.load(), 3);
    }

    void deletingThreadInFrameReadySlotIsSafe()
    {
        // FrameReady 를 받는 슬롯(직접 연결)이 그 자리에서 렌더 스레드를 지워도 죽지 않는다: 워커의 조각을 다시 내는 중계는
        // emit 뒤에 지워진 객체(받았다는 확인을 넣을 채널)를 쓰지 않는다. 지우면 Stop 이 핸들과 공급원을 정상으로 돌려준다.
        constexpr int kRounds = 10;
        for (int iRound = 0; iRound < kRounds; ++iRound)
        {
            FakeFrameSource source(kCols, kRows, 0);
            std::atomic<int> iReleases{0};
            int iPatches = 0;
            std::vector<ITerminalFrameSource*> vecReleased;
            TerminalRenderThread* pThread = new TerminalRenderThread(ETerminalRendererMode::CPU);
            const QPointer<TerminalRenderThread> pGuard(pThread);
            connect(pThread, &TerminalRenderThread::SourceReleased, this,
                    [&](ITerminalFrameSource* pSource) { vecReleased.push_back(pSource); });
            connect(pThread, &TerminalRenderThread::FrameReady, pThread, [&](int, const QImage&, const QRect&)
            {
                ++iPatches;
                delete pThread;
            });
            pThread->SetFont(BaseFont());
            pThread->Start();
            pThread->AttachView(&source, CountingHandle(iReleases), ViewSize(1.0), 1.0);
            QVERIFY(QTest::qWaitFor([&] { return pGuard.isNull(); }, kFrameTimeoutMs));
            QTest::qWait(20);   // 남은 이벤트를 처리해도 죽지 않는다
            QCOMPARE(iPatches, 1);
            QCOMPARE(iReleases.load(), 1);   // 지울 때 Stop 이 핸들을 놓았다
            QCOMPARE(int(vecReleased.size()), 1);
            QCOMPARE(vecReleased.front(), static_cast<ITerminalFrameSource*>(&source));
        }
    }

    void deletingThreadInViewDetachedSlotIsSafe()
    {
        // ViewDetached 를 받는 슬롯(직접 연결)이 그 자리에서 렌더 스레드를 지워도 죽지 않는다. 그 공급원의 SourceReleased 는
        // 정확히 한 번 온다: 그 화면은 슬롯이 끝난 뒤에 세므로, 지울 때 Stop 이 아직 쓰는 공급원으로 보고 알린다.
        constexpr int kRounds = 10;
        for (int iRound = 0; iRound < kRounds; ++iRound)
        {
            FakeFrameSource source(kCols, kRows, 0);
            std::vector<int> vecDetached;
            int iReleasedAtDetach = -1;
            std::vector<ITerminalFrameSource*> vecReleased;
            TerminalRenderThread* pThread = new TerminalRenderThread(ETerminalRendererMode::CPU);
            const QPointer<TerminalRenderThread> pGuard(pThread);
            FrameLog log;
            connect(pThread, &TerminalRenderThread::FrameReady, this, [&log](int viewId, const QImage& imgPatch, const QRect& rectPatch)
            {
                log.vecPatches.push_back({viewId, imgPatch, rectPatch});
            });
            connect(pThread, &TerminalRenderThread::SourceReleased, this,
                    [&](ITerminalFrameSource* pSource) { vecReleased.push_back(pSource); });
            connect(pThread, &TerminalRenderThread::ViewDetached, pThread, [&](int viewId)
            {
                vecDetached.push_back(viewId);
                iReleasedAtDetach = int(vecReleased.size());
                delete pThread;
            });
            pThread->SetFont(BaseFont());
            pThread->Start();
            const int iView = pThread->AttachView(&source, {}, ViewSize(1.0), 1.0);
            QVERIFY(WaitFrames(log, iView, 1));
            pThread->DetachView(iView);
            QVERIFY(QTest::qWaitFor([&] { return pGuard.isNull(); }, kFrameTimeoutMs));
            QTest::qWait(20);   // 남은 이벤트를 처리해도 죽지 않는다
            QCOMPARE(vecDetached, std::vector<int>{iView});
            QCOMPARE(iReleasedAtDetach, 0);   // ViewDetached 가 SourceReleased 보다 먼저 온다
            QCOMPARE(int(vecReleased.size()), 1);
            QCOMPARE(vecReleased.front(), static_cast<ITerminalFrameSource*>(&source));
        }
    }

    void viewDetachedSlotSeesViewStillCounted()
    {
        // ViewDetached 를 받는 슬롯 안에서는 뗀 화면을 아직 공급원을 쓰는 것으로 센다(슬롯이 끝난 뒤 센다). 그래서 슬롯이 같은
        // 공급원을 다시 붙이거나 잡으면 그 공급원은 쓰이는 채로 남아 SourceReleased 가 오지 않고, 마지막 쓰는 쪽이 사라질 때 한 번 온다.
        // 슬롯이 잡은 것을 놓으면 슬롯 안에서가 아니라 ViewDetached 를 다 낸 뒤에 한 번 온다.
        FakeFrameSource source(kCols, kRows, 0);
        FrameLog log;
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        QSignalSpy spyReleased(&thread, &TerminalRenderThread::SourceReleased);
        QSignalSpy spyDetached(&thread, &TerminalRenderThread::ViewDetached);
        StartCpu(thread, log);

        // 1) 슬롯이 같은 공급원을 다시 붙인다
        const int iFirst = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iFirst, 1));
        int iAgain = 0;
        QMetaObject::Connection connReattach = connect(&thread, &TerminalRenderThread::ViewDetached, &thread, [&](int viewId)
        {
            if (viewId == iFirst)
            {
                iAgain = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
            }
        });
        thread.DetachView(iFirst);
        QVERIFY(QTest::qWaitFor([&] { return iAgain != 0; }, kFrameTimeoutMs));
        disconnect(connReattach);
        QVERIFY(WaitFrames(log, iAgain, 1));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyReleased.count(), 0);   // 다시 붙은 화면이 쓴다
        thread.DetachView(iAgain);
        QVERIFY(spyReleased.wait(kFrameTimeoutMs));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyReleased.count(), 1);

        // 2) 슬롯이 공급원을 잡는다
        spyReleased.clear();
        const int iSecond = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iSecond, 1));
        bool bRetained = false;
        QMetaObject::Connection connRetain = connect(&thread, &TerminalRenderThread::ViewDetached, &thread, [&](int viewId)
        {
            if (viewId == iSecond)
            {
                thread.RetainSource(&source);
                bRetained = true;
            }
        });
        thread.DetachView(iSecond);
        QVERIFY(QTest::qWaitFor([&] { return bRetained; }, kFrameTimeoutMs));
        disconnect(connRetain);
        QTest::qWait(kSettleMs);
        QCOMPARE(spyReleased.count(), 0);   // 잡혀 있다
        thread.ReleaseSource(&source);
        QCOMPARE(spyReleased.count(), 1);   // 놓는 자리에서 한 번

        // 3) 슬롯이 잡은 것을 놓는다(붙은 화면 1 + 잡음 1)
        spyReleased.clear();
        thread.RetainSource(&source);
        const int iThird = thread.AttachView(&source, {}, ViewSize(1.0), 1.0);
        QVERIFY(WaitFrames(log, iThird, 1));
        int iReleasedInSlot = -1;
        QMetaObject::Connection connRelease = connect(&thread, &TerminalRenderThread::ViewDetached, &thread, [&](int viewId)
        {
            if (viewId == iThird)
            {
                thread.ReleaseSource(&source);
                iReleasedInSlot = spyReleased.count();
            }
        });
        thread.DetachView(iThird);
        QVERIFY(spyReleased.wait(kFrameTimeoutMs));
        disconnect(connRelease);
        QTest::qWait(kSettleMs);
        QCOMPARE(iReleasedInSlot, 0);       // 슬롯 안에서는 뗀 화면이 아직 센다
        QCOMPARE(spyReleased.count(), 1);   // ViewDetached 를 다 낸 뒤 한 번
        QVERIFY(spyDetached.count() >= 4);
    }

    void deletingThreadInSourceReleasedSlotDuringStopReleasesAll()
    {
        // 명시적 Stop 이 공급원 여러 개를 알리는 중에 첫 SourceReleased 슬롯(직접 연결)이 렌더 스레드를 지워도 죽지 않고,
        // 남은 공급원도 정확히 한 번씩 알린다(지우면서 부른 Stop 이 이어서 알린다)
        constexpr int kRounds = 5;
        for (int iRound = 0; iRound < kRounds; ++iRound)
        {
            std::vector<std::unique_ptr<FakeFrameSource>> vecSources;
            std::vector<ITerminalFrameSource*> vecReleased;
            FrameLog log;
            TerminalRenderThread* pThread = new TerminalRenderThread(ETerminalRendererMode::CPU);
            const QPointer<TerminalRenderThread> pGuard(pThread);
            Connect(*pThread, log);
            connect(pThread, &TerminalRenderThread::SourceReleased, this, [&](ITerminalFrameSource* pSource)
            {
                vecReleased.push_back(pSource);
                if (vecReleased.size() == 1)
                {
                    delete pThread;
                }
            });
            pThread->SetFont(BaseFont());
            pThread->Start();
            QVERIFY(AttachStopSources(*pThread, log, vecSources));
            pThread->Stop();
            QVERIFY(pGuard.isNull());
            QTest::qWait(20);   // 남은 이벤트를 처리해도 죽지 않는다
            QVERIFY2(EachReleasedOnce(vecReleased, vecSources), qPrintable(QString::number(vecReleased.size())));
        }
    }

    void sourceReleasedSlotDuringStopKeepsCounts()
    {
        // Stop 이 알리는 중의 SourceReleased 슬롯이 할 수 있는 일:
        // 1) Stop 을 다시 부르면 남은 공급원을 그 자리에서 알린다(공급원마다 한 번)
        // 2) 아직 알리지 않은 다른 공급원을 잡거나 다시 붙이면 그 공급원은 쓰이는 채로 남아 이번 Stop 은 알리지 않는다.
        //    놓거나(ReleaseSource) 다시 멈출 때(소멸자의 Stop) 한 번 알린다(쓰이는데 알렸다가 또 알리지 않는다)
        constexpr int kStopAgain = 0;     // 1)
        constexpr int kRetainOther = 1;   // 2) 잡는다
        constexpr int kAttachOther = 2;   // 2) 다시 붙인다
        constexpr int kCases = 3;
        for (int iCase = 0; iCase < kCases; ++iCase)
        {
            std::vector<std::unique_ptr<FakeFrameSource>> vecSources;
            std::vector<ITerminalFrameSource*> vecReleased;
            FrameLog log;
            ITerminalFrameSource* pKept = nullptr;   // 슬롯이 잡거나 다시 붙인 공급원
            auto upThread = std::make_unique<TerminalRenderThread>(ETerminalRendererMode::CPU);
            TerminalRenderThread& thread = *upThread;
            Connect(thread, log);
            connect(&thread, &TerminalRenderThread::SourceReleased, this, [&](ITerminalFrameSource* pSource)
            {
                vecReleased.push_back(pSource);
                if (vecReleased.size() != 1)
                {
                    return;
                }
                if (iCase == kStopAgain)
                {
                    thread.Stop();
                    return;
                }
                for (const auto& upSource : vecSources)
                {
                    if (upSource.get() != pSource)
                    {
                        pKept = upSource.get();   // 아직 알리지 않은 공급원
                        break;
                    }
                }
                if (iCase == kRetainOther)
                {
                    thread.RetainSource(pKept);
                }
                else if (iCase == kAttachOther)
                {
                    thread.AttachView(pKept, {}, ViewSize(1.0), 1.0);   // 멈춘 스레드: 다음 Start 까지 붙은 것으로 센다
                }
            });
            thread.SetFont(BaseFont());
            thread.Start();
            QVERIFY(AttachStopSources(thread, log, vecSources));
            thread.Stop();
            if (iCase == kStopAgain)
            {
                QVERIFY2(EachReleasedOnce(vecReleased, vecSources), qPrintable(QString::number(vecReleased.size())));
                continue;
            }
            QVERIFY(pKept);
            QCOMPARE(int(std::count(vecReleased.begin(), vecReleased.end(), pKept)), 0);   // 쓰이는 동안 알리지 않았다
            QCOMPARE(int(vecReleased.size()), int(vecSources.size()) - 1);
            if (iCase == kRetainOther)
            {
                thread.ReleaseSource(pKept);   // 놓는 자리에서 한 번
            }
            else
            {
                upThread.reset();               // 붙여 둔 채 지운다: 소멸자의 Stop 이 한 번
            }
            QVERIFY2(EachReleasedOnce(vecReleased, vecSources),
                     qPrintable(QStringLiteral("case %1: %2").arg(iCase).arg(vecReleased.size())));
        }
    }
};

QObject* CreateTerminalRenderThreadTest() { return new TerminalRenderThreadTest(); }

#include "TerminalRenderThreadTest.moc"
