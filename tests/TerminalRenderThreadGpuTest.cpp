#include "TerminalViewer/Thread/TerminalRenderThread.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/FrameScheduler.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "FakeFrameSource.h"
#include "TestFonts.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// GPU 로만 가는 렌더 스레드 경로(붙이기 실패, 표시 준비 안 됨, surface 잃음, 장치 잃음, GPU 시작 실패)를
// 가짜 GPU Strategy 로 시험한다. 실제 surface 는 offscreen 테스트에서 만들 수 없다.
namespace
{
constexpr int kCols = 20;
constexpr int kRows = 5;
constexpr int kPaddingLogical = 4;
constexpr int kTimeoutMs = 1000;
constexpr int kSettleMs = 150;
constexpr int kHugePixelSize = 500;      // 박스 글리프 몇 개로 아틀라스가 넘친다(atlasOverflowSkipsFrameWithoutLooping 과 같은 방법)
constexpr uint32_t kFirstBox = 0x2500;
constexpr int kBoxCount = 160;
constexpr int kEndlessStall = 1'000'000;  // 이만큼의 Execute 를 bNotReady 로(테스트가 0 으로 풀 때까지)
constexpr int kStallRetries = 3;          // 막힌 뒤 다시 본 횟수가 이만큼 되면 "다시 보는 중" 으로 본다
constexpr qint64 kBlinkOffEndMs = 2 * FrameScheduler::kBlinkIntervalNs / 1'000'000;   // 깜빡임 주기 시작부터 커서가 다시 켜질 때까지
constexpr TerminalRgb kRed{255, 0, 0};
constexpr TerminalRgb kGreen{0, 255, 0};

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

TerminalCell BgCell(const TerminalRgb& bg)
{
    TerminalCell cell;
    cell.bBgDefault = false;
    cell.bg = bg;
    return cell;
}

// 테스트와 렌더 스레드(가짜 Strategy)가 함께 보는 조종판
struct FakeGpuControl
{
    struct ExecuteRecord
    {
        int iViewId = 0;
        uint32_t uListGeneration = 0;
        uint32_t uCacheGeneration = 0;
        uint64_t uListHash = 0;          // 쿼드·스크롤 오프셋의 해시(같은 목록을 다시 냈는지 본다)
        bool bHasWatchedRect = false;    // uWatchedColor 색의 RECT 가 목록에 있다
        bool bPresented = false;
    };

    std::atomic<uint32_t> uWatchedColor{0};         // PackColor 값. 0 이면 보지 않는다
    std::atomic<int> iCreated{0};
    std::atomic<int> iDestroyed{0};
    std::atomic<bool> bCreateFails{false};
    std::atomic<bool> bAttachFails{false};
    std::atomic<bool> bReady{true};
    std::atomic<int> iNotReadyFrames{0};           // 다음 Execute 몇 번을 bNotReady 로
    std::atomic<bool> bResetAtlasWhenNotReady{false};
    std::atomic<bool> bSurfaceLostNext{false};
    std::atomic<int> iSurfaceLostFrames{0};         // 다음 Execute 몇 번을 bSurfaceLost 로(다시 붙인 화면도 센다)
    std::atomic<bool> bDeviceLostNext{false};
    std::atomic<int> iReleases{0};

    mutable std::mutex mtxRecords;
    std::vector<ExecuteRecord> vecExecutes;

    int CountExecutes(int viewId) const
    {
        std::lock_guard<std::mutex> lock(mtxRecords);
        int iCount = 0;
        for (const ExecuteRecord& record : vecExecutes)
            iCount += record.iViewId == viewId ? 1 : 0;
        return iCount;
    }

    std::vector<ExecuteRecord> GetExecutes() const
    {
        std::lock_guard<std::mutex> lock(mtxRecords);
        return vecExecutes;
    }

    std::vector<ExecuteRecord> GetExecutes(int viewId) const
    {
        std::lock_guard<std::mutex> lock(mtxRecords);
        std::vector<ExecuteRecord> vecView;
        for (const ExecuteRecord& record : vecExecutes)
        {
            if (record.iViewId == viewId)
                vecView.push_back(record);
        }
        return vecView;
    }
};

uint64_t HashList(const DrawList& list)
{
    constexpr uint64_t kFnvOffset = 1469598103934665603ull;
    static constexpr uint64_t kFnvPrime = 1099511628211ull;   // static: 람다가 캡처하지 않고 쓴다(MSVC 2019 는 static 이 아니면 C3493)
    uint64_t uHash = kFnvOffset;
    const auto funcMix = [&uHash](const void* pData, size_t size)
    {
        const unsigned char* pBytes = static_cast<const unsigned char*>(pData);
        for (size_t i = 0; i < size; ++i)
            uHash = (uHash ^ pBytes[i]) * kFnvPrime;
    };
    funcMix(list.vecQuads.data(), list.vecQuads.size() * sizeof(DrawQuad));
    funcMix(&list.iScrollOffsetPx, sizeof(list.iScrollOffsetPx));
    return uHash;
}

bool HasRect(const DrawList& list, uint32_t color)
{
    for (const DrawQuad& quad : list.vecQuads)
    {
        if (quad.eKind == EQuadKind::RECT && quad.uColor == color)
            return true;
    }
    return false;
}

// 렌더 스레드에서만 쓰인다. 핸들 계약(ITerminalRenderStrategy::AttachView)을 진짜 GPU Strategy 처럼 지킨다.
class FakeGpuStrategy : public ITerminalRenderStrategy
{
public:
    explicit FakeGpuStrategy(std::shared_ptr<FakeGpuControl> spControl)
        : m_spControl(std::move(spControl))
    {
        ++m_spControl->iCreated;
    }

    ~FakeGpuStrategy() override
    {
        for (auto& pairView : m_hashHandles)
            Release(pairView.second);
        ++m_spControl->iDestroyed;
    }

    ETerminalRenderStrategy GetType() const override { return ETerminalRenderStrategy::GPU; }

    bool Execute(const TerminalRenderInput& in, TerminalRenderOutput& out) override
    {
        out = TerminalRenderOutput();
        if (!m_hashHandles.count(in.iViewId))
            return false;
        FakeGpuControl::ExecuteRecord record;
        record.iViewId = in.iViewId;
        record.uListGeneration = in.pDrawList->uAtlasGeneration;
        record.uCacheGeneration = in.pGlyphCache->GetGeneration();
        record.uListHash = HashList(*in.pDrawList);
        const uint32_t uWatched = m_spControl->uWatchedColor;
        record.bHasWatchedRect = uWatched != 0 && HasRect(*in.pDrawList, uWatched);
        const bool bExecuted = Decide(in, out);
        record.bPresented = out.bPresented;
        {
            std::lock_guard<std::mutex> lock(m_spControl->mtxRecords);
            m_spControl->vecExecutes.push_back(record);
        }
        return bExecuted;
    }

    bool AttachView(int viewId, const TerminalNativeHandle& handle, const QSize&) override
    {
        if (m_spControl->bAttachFails)
        {
            Release(handle);
            return false;
        }
        m_hashHandles[viewId] = handle;
        return true;
    }

    void DetachView(int viewId) override
    {
        const auto it = m_hashHandles.find(viewId);
        if (it == m_hashHandles.end())
            return;
        const TerminalNativeHandle handle = it->second;
        m_hashHandles.erase(it);
        Release(handle);
    }

    void ResizeView(int, const QSize&) override {}

    bool IsReady(int viewId) const override { return m_hashHandles.count(viewId) && m_spControl->bReady; }

private:
    bool Decide(const TerminalRenderInput& in, TerminalRenderOutput& out)
    {
        if (m_spControl->bDeviceLostNext.exchange(false))
        {
            out.bDeviceLost = true;
            return false;
        }
        if (m_spControl->bSurfaceLostNext.exchange(false))
        {
            out.bSurfaceLost = true;
            return false;
        }
        if (m_spControl->iSurfaceLostFrames > 0)
        {
            --m_spControl->iSurfaceLostFrames;
            out.bSurfaceLost = true;
            return false;
        }
        if (m_spControl->iNotReadyFrames > 0)
        {
            --m_spControl->iNotReadyFrames;
            if (m_spControl->bResetAtlasWhenNotReady.exchange(false))
                OverflowAtlas(*in.pGlyphCache);   // 같은 캐시를 쓰는 다른 화면이 아틀라스를 비운 것과 같다
            out.bNotReady = true;
            return false;
        }
        out.bPresented = true;
        return true;
    }

    void Release(const TerminalNativeHandle& handle)
    {
        if (handle.funcRelease)
            handle.funcRelease(handle.pHandle);
    }

    static void OverflowAtlas(GlyphCache& cache)
    {
        const uint32_t uStart = cache.GetGeneration();
        for (int iBox = 0; iBox < kBoxCount && cache.GetGeneration() == uStart; ++iBox)
            cache.GetGlyph(std::u32string(1, char32_t(kFirstBox + uint32_t(iBox))), false, false, 1);
    }

    std::shared_ptr<FakeGpuControl> m_spControl;
    std::unordered_map<int, TerminalNativeHandle> m_hashHandles;
};

TerminalNativeHandle GpuHandle(std::atomic<int>& releases)
{
    static int s_iFakeNative = 0;
    TerminalNativeHandle handle;
    handle.pHandle = &s_iFakeNative;
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

QRegularExpression GpuUnavailableWarning()
{
    return QRegularExpression(QStringLiteral("^Terminal renderer: GPU unavailable, drawing with CPU: "));
}
}

// 모든 테스트에서 공급원·조종판·카운터는 렌더 스레드보다 먼저 선언한다(스레드가 먼저 멈춘 뒤 지워진다).
class TerminalRenderThreadGpuTest : public QObject
{
    Q_OBJECT

private:
    static void UseFakeGpu(TerminalRenderThread& thread, const std::shared_ptr<FakeGpuControl>& spControl)
    {
        thread.SetGpuStrategyFactory([spControl](QString* pError) -> std::unique_ptr<ITerminalRenderStrategy>
        {
            if (spControl->bCreateFails)
            {
                *pError = QStringLiteral("가짜 GPU 를 만들지 못했다");
                return nullptr;
            }
            return std::make_unique<FakeGpuStrategy>(spControl);
        });
    }

    // 표시를 막아 둔 채 내용을 바꾸고 깨운다. 렌더 스레드가 그 목록으로 kStallRetries 번 넘게 다시 보면 참
    static bool StallAfterChange(TerminalRenderThread& thread, FakeFrameSource& source, FakeGpuControl& control, int viewId, int line)
    {
        const int iExecutesBefore = control.CountExecutes(viewId);
        control.iNotReadyFrames = kEndlessStall;
        source.SetCell(line, 0, BgCell(kRed));
        thread.Wake(viewId);
        return QTest::qWaitFor([&] { return control.CountExecutes(viewId) >= iExecutesBefore + kStallRetries; }, kTimeoutMs);
    }

    // 그 화면의 fromIndex 번째 Execute 부터 표시한 것
    static int CountPresented(const FakeGpuControl& control, int viewId, int fromIndex)
    {
        const std::vector<FakeGpuControl::ExecuteRecord> vecExecutes = control.GetExecutes(viewId);
        return int(std::count_if(vecExecutes.begin() + std::min<size_t>(size_t(fromIndex), vecExecutes.size()), vecExecutes.end(),
                                 [](const FakeGpuControl::ExecuteRecord& record) { return record.bPresented; }));
    }

    static FakeGpuControl::ExecuteRecord FirstPresented(const FakeGpuControl& control, int viewId, int fromIndex)
    {
        const std::vector<FakeGpuControl::ExecuteRecord> vecExecutes = control.GetExecutes(viewId);
        for (size_t iIndex = size_t(fromIndex); iIndex < vecExecutes.size(); ++iIndex)
        {
            if (vecExecutes[iIndex].bPresented)
                return vecExecutes[iIndex];
        }
        return FakeGpuControl::ExecuteRecord();
    }

private slots:
    void initTestCase() { TestFonts::Register(); }

    void autoDrawsNativeViewsWithGpuAndOthersWithCpu()
    {
        FakeFrameSource gpuSource(kCols, kRows, 0);
        FakeFrameSource gpuSource2(kCols, kRows, 0);
        FakeFrameSource cpuSource(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::GPU);   // AUTO: 시도하기 전에도 GPU(새 화면은 GPU 위젯)
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        QSignalSpy spyStrategy(&thread, &TerminalRenderThread::StrategyChanged);
        QSignalSpy spyFallback(&thread, &TerminalRenderThread::ViewNeedsCpuFallback);
        thread.SetFont(BaseFont());
        thread.Start();
        QTest::qWait(kSettleMs);
        QCOMPARE(spControl->iCreated.load(), 0);   // 스레드를 시작해도 GPU 는 만들지 않는다

        const int iGpu = thread.AttachView(&gpuSource, GpuHandle(iReleases), ViewSize(), 1.0);
        const int iGpu2 = thread.AttachView(&gpuSource2, GpuHandle(iReleases), ViewSize(), 1.0);
        const int iCpu = thread.AttachView(&cpuSource, {}, ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iGpu) >= 1 && spControl->CountExecutes(iGpu2) >= 1; }, kTimeoutMs));
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iCpu) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);
        QCOMPARE(spControl->iCreated.load(), 1);           // GPU 는 한 번만 만든다
        QCOMPARE(CountId(spyFrames, iGpu), 0);             // GPU 화면은 조각을 내지 않는다
        QCOMPARE(CountId(spyFrames, iGpu2), 0);
        QCOMPARE(spControl->CountExecutes(iCpu), 0);       // 핸들 없는 화면은 CPU 로
        QCOMPARE(spyStrategy.count(), 0);
        QCOMPARE(spyFallback.count(), 0);
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::GPU);
        QVERIFY(thread.GetGpuError().isEmpty());

        QVERIFY(thread.DetachViewAndWait(iGpu, 500));
        QCOMPARE(iReleases.load(), 1);                     // GPU Strategy 가 한 번 놓는다(렌더 스레드는 놓지 않는다)
        thread.Stop();
        QCOMPARE(iReleases.load(), 2);
        QCOMPARE(spControl->iDestroyed.load(), 1);
    }

    void notReadyViewRetriesSameListWithoutCapture()
    {
        FakeFrameSource source(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        thread.SetFont(BaseFont());
        thread.Start();

        spControl->bReady = false;   // 펜스가 안 끝남: Execute 도 캡처도 하지 않고 다시 본다
        const int iView = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        QTest::qWait(kSettleMs);
        QCOMPARE(spControl->CountExecutes(iView), 0);
        QCOMPARE(source.GetCaptureCount(), 0);
        spControl->bReady = true;
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iView) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        // 이미지를 못 받음(bNotReady) 3번: 같은 목록으로 다시 Execute 하고 다시 캡처하지 않는다
        const int iCapturesBefore = source.GetCaptureCount();
        const int iExecutesBefore = spControl->CountExecutes(iView);
        spControl->iNotReadyFrames = 3;
        source.SetCell(1, 1, BgCell(kRed));
        thread.Wake(iView);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iView) >= iExecutesBefore + 4; }, kTimeoutMs));
        QTest::qWait(kSettleMs);
        QCOMPARE(spControl->CountExecutes(iView), iExecutesBefore + 4);
        QCOMPARE(source.GetCaptureCount(), iCapturesBefore + 1);
    }

    void atlasResetWhilePendingRebuildsList()
    {
        // 다시 그리기를 기다리는 동안 같은 캐시의 아틀라스가 비워지면 옛 목록(옛 글리프 자리)을 쓰지 않고 다시 만든다.
        QFont huge = BaseFont();
        huge.setPixelSize(kHugePixelSize);
        FakeFrameSource source(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        thread.SetFont(huge);
        spControl->iNotReadyFrames = 1;
        spControl->bResetAtlasWhenNotReady = true;
        thread.Start();
        const int iView = thread.AttachView(&source, GpuHandle(iReleases), QSize(200, 100), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iView) >= 2; }, 10000));
        QTest::qWait(kSettleMs);

        const std::vector<FakeGpuControl::ExecuteRecord> vecExecutes = spControl->GetExecutes();
        QCOMPARE(int(vecExecutes.size()), 2);
        QCOMPARE(vecExecutes[0].uListGeneration, vecExecutes[0].uCacheGeneration);
        QVERIFY(vecExecutes[1].uCacheGeneration > vecExecutes[0].uCacheGeneration);   // 그사이 아틀라스가 비워졌다
        QCOMPARE(vecExecutes[1].uListGeneration, vecExecutes[1].uCacheGeneration);    // 새 세대로 다시 만든 목록
        QCOMPARE(source.GetCaptureCount(), 2);                                          // 다시 만들려고 다시 캡처했다
        QVERIFY(source.GetLastRequest().bFull);
    }

    void surfaceLostIsReportedOnceAndViewStops()
    {
        FakeFrameSource source(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        QSignalSpy spyLost(&thread, &TerminalRenderThread::ViewSurfaceLost);
        thread.SetFont(BaseFont());
        thread.Start();
        const int iView = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iView) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        spControl->bSurfaceLostNext = true;
        source.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iView);
        QVERIFY(spyLost.wait(kTimeoutMs));
        QCOMPARE(spyLost.at(0).at(0).toInt(), iView);
        const int iExecutes = spControl->CountExecutes(iView);
        for (int i = 0; i < 5; ++i)
        {
            source.SetCell(0, i, BgCell(kRed));
            thread.Wake(iView);
        }
        QTest::qWait(kSettleMs);
        QCOMPARE(spyLost.count(), 1);                            // 한 번만 알린다
        QCOMPARE(spControl->CountExecutes(iView), iExecutes);    // 새 핸들로 다시 붙일 때까지 그리지 않는다
        QCOMPARE(iReleases.load(), 0);                           // 떼기 전에는 놓지 않는다
        QVERIFY(thread.DetachViewAndWait(iView, 500));
        QCOMPARE(iReleases.load(), 1);
    }

    void attachFailureFallsBackOnlyThatView()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FakeFrameSource other(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        QSignalSpy spyFallback(&thread, &TerminalRenderThread::ViewNeedsCpuFallback);
        QSignalSpy spyStrategy(&thread, &TerminalRenderThread::StrategyChanged);
        thread.SetFont(BaseFont());
        thread.Start();

        spControl->bAttachFails = true;   // surface 를 만들지 못함
        const int iFailed = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(spyFallback.wait(kTimeoutMs));
        QCOMPARE(spyFallback.at(0).at(0).toInt(), iFailed);
        QCOMPARE(iReleases.load(), 1);    // 실패해도 핸들은 한 번 놓는다
        QCOMPARE(source.GetCaptureCount(), 0);

        spControl->bAttachFails = false;
        const int iOk = thread.AttachView(&other, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iOk) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyFallback.count(), 1);
        QCOMPARE(spyStrategy.count(), 0);  // 그 화면만 CPU 로. GPU 는 그대로
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::GPU);
        thread.DetachView(iFailed);       // GUI 가 바꿔 낀 뒤 뗀다: 두 번 놓지 않는다
        QVERIFY(thread.DetachViewAndWait(iOk, 500));
        QCOMPARE(iReleases.load(), 2);
    }

    void deviceLostMovesEveryGpuViewToCpu()
    {
        FakeFrameSource sourceA(kCols, kRows, 0);
        FakeFrameSource sourceB(kCols, kRows, 0);
        FakeFrameSource cpuSource(kCols, kRows, 0);
        FakeFrameSource lateSource(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        QSignalSpy spyFallback(&thread, &TerminalRenderThread::ViewNeedsCpuFallback);
        QSignalSpy spyStrategy(&thread, &TerminalRenderThread::StrategyChanged);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        thread.SetFont(BaseFont());
        thread.Start();
        const int iA = thread.AttachView(&sourceA, GpuHandle(iReleases), ViewSize(), 1.0);
        const int iB = thread.AttachView(&sourceB, GpuHandle(iReleases), ViewSize(), 1.0);
        const int iCpu = thread.AttachView(&cpuSource, {}, ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iA) >= 1 && spControl->CountExecutes(iB) >= 1; }, kTimeoutMs));
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iCpu) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        spControl->bDeviceLostNext = true;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("GPU device lost")));
        sourceA.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iA);
        QVERIFY(spyStrategy.wait(kTimeoutMs));
        QCOMPARE(spyStrategy.count(), 1);
        QCOMPARE(spyStrategy.at(0).at(0).value<ETerminalRenderStrategy>(), ETerminalRenderStrategy::CPU);
        QCOMPARE(CountId(spyFallback, iA), 1);   // 모든 GPU 화면
        QCOMPARE(CountId(spyFallback, iB), 1);
        QCOMPARE(CountId(spyFallback, iCpu), 0);
        QCOMPARE(iReleases.load(), 2);           // GPU 자원을 버리며 핸들을 한 번씩 놓는다
        QCOMPARE(spControl->iDestroyed.load(), 1);
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::CPU);
        QVERIFY(!thread.GetGpuError().isEmpty());

        // CPU 화면은 계속 그린다
        spyFrames.clear();
        cpuSource.SetCell(1, 1, BgCell(kRed));
        thread.Wake(iCpu);
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iCpu) >= 1; }, kTimeoutMs));

        // 버린 화면의 명령은 무시되고, 새 GPU 화면은 GPU 를 다시 만들지 않고 바로 CPU 로 바꿔 달라고 한다
        const int iExecutes = int(spControl->GetExecutes().size());
        thread.Wake(iB);
        const int iLate = thread.AttachView(&lateSource, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFallback, iLate) == 1; }, kTimeoutMs));
        QCOMPARE(spControl->iCreated.load(), 1);
        QCOMPARE(iReleases.load(), 3);
        QCOMPARE(int(spControl->GetExecutes().size()), iExecutes);
        QCOMPARE(spyStrategy.count(), 1);
        QCOMPARE(lateSource.GetCaptureCount(), 0);
        thread.DetachView(iA);
        thread.DetachView(iB);
        QVERIFY(thread.DetachViewAndWait(iLate, 500));
        QCOMPARE(iReleases.load(), 3);
    }

    void gpuStartFailureFallsBackToCpu()
    {
        FakeFrameSource source(kCols, kRows, 0);
        FakeFrameSource second(kCols, kRows, 0);
        FakeFrameSource cpuSource(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        spControl->bCreateFails = true;
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        QSignalSpy spyFallback(&thread, &TerminalRenderThread::ViewNeedsCpuFallback);
        QSignalSpy spyStrategy(&thread, &TerminalRenderThread::StrategyChanged);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        QTest::ignoreMessage(QtWarningMsg, GpuUnavailableWarning());
        thread.SetFont(BaseFont());
        thread.Start();
        const int iView = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(spyFallback.wait(kTimeoutMs));
        QCOMPARE(spyFallback.at(0).at(0).toInt(), iView);
        QCOMPARE(spyStrategy.count(), 1);
        QCOMPARE(spyStrategy.at(0).at(0).value<ETerminalRenderStrategy>(), ETerminalRenderStrategy::CPU);
        QCOMPARE(iReleases.load(), 1);   // 렌더 스레드가 놓는다
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::CPU);
        QCOMPARE(thread.GetGpuError(), QStringLiteral("가짜 GPU 를 만들지 못했다"));

        const int iSecond = thread.AttachView(&second, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFallback, iSecond) == 1; }, kTimeoutMs));
        QCOMPARE(spControl->iCreated.load(), 0);
        QCOMPARE(spyStrategy.count(), 1);   // 다시 시도하지 않는다
        QCOMPARE(iReleases.load(), 2);
        const int iCpu = thread.AttachView(&cpuSource, {}, ViewSize(), 1.0);   // 바꿔 낀 CPU 위젯
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iCpu) >= 1; }, kTimeoutMs));
    }

    void cpuModeNeverStartsGpu()
    {
        FakeFrameSource source(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::CPU);
        UseFakeGpu(thread, spControl);
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::CPU);
        QSignalSpy spyFrames(&thread, &TerminalRenderThread::FrameReady);
        QSignalSpy spyStrategy(&thread, &TerminalRenderThread::StrategyChanged);
        thread.SetFont(BaseFont());
        thread.Start();
        const int iView = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return CountId(spyFrames, iView) >= 1; }, kTimeoutMs));   // 핸들이 있어도 CPU 로
        QCOMPARE(spControl->iCreated.load(), 0);
        QCOMPARE(spyStrategy.count(), 0);
        QVERIFY(thread.DetachViewAndWait(iView, 500));
        QCOMPARE(iReleases.load(), 1);
    }

    void notReadyStallRetriesWithoutBusyLoopOrFallback()
    {
        // 표시가 오래 막혀도(펜스·이미지가 안 끝남) 잠깐의 지연일 수 있다: CPU 로 바꾸지 않고 재시도 간격(1ms)으로 다시 본다
        FakeFrameSource source(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        QSignalSpy spyFallback(&thread, &TerminalRenderThread::ViewNeedsCpuFallback);
        QSignalSpy spyStrategy(&thread, &TerminalRenderThread::StrategyChanged);
        thread.SetFont(BaseFont());
        thread.Start();
        const int iView = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iView) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        constexpr int kStallMs = 300;
        constexpr int kStallFrames = 1'000'000;
        spControl->iNotReadyFrames = kStallFrames;
        const int iBefore = spControl->CountExecutes(iView);
        source.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iView);
        QTest::qWait(kStallMs);
        const int iDuring = spControl->CountExecutes(iView) - iBefore;
        QVERIFY2(iDuring >= 10 && iDuring <= 2 * kStallMs, qPrintable(QString::number(iDuring)));   // 다시 보지만 바쁘게 돌지 않는다
        QCOMPARE(spyFallback.count(), 0);
        QCOMPARE(spyStrategy.count(), 0);

        spControl->iNotReadyFrames = 0;   // 막힘이 풀렸다
        QVERIFY(QTest::qWaitFor([&] { return spControl->iNotReadyFrames == 0 && spControl->CountExecutes(iView) > iBefore + iDuring; },
                                kTimeoutMs));
        QTest::qWait(kSettleMs);
        const int iSettled = spControl->CountExecutes(iView);
        QTest::qWait(kSettleMs);
        QCOMPARE(spControl->CountExecutes(iView), iSettled);   // 표시되었으니 더 다시 보지 않는다
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::GPU);
        QVERIFY(thread.DetachViewAndWait(iView, 500));
    }

    void wakeDuringNotReadyRetryDrawsNewestState()
    {
        // 표시가 막혀 같은 목록으로 다시 보는 동안 새 Wake 가 오면 옛 목록을 버리고 새로 캡처한다. 막힘이 풀리면
        // 깨어난 시점의 최신 상태를 그린다(스펙 §5). 옛 목록을 내고 깨움을 지우면 새 상태가 다음 일까지 화면에 안 나온다.
        FakeFrameSource source(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        thread.SetFont(BaseFont());
        thread.Start();
        const int iView = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iView) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        // 1) 첫 변경: 표시가 막혀(bNotReady) 같은 목록으로 다시 본다
        spControl->uWatchedColor = PackColor(kGreen);
        const int iExecutesBefore = spControl->CountExecutes(iView);
        QVERIFY(StallAfterChange(thread, source, *spControl, iView, 0));
        const int iCapturesStalled = source.GetCaptureCount();
        const uint64_t uStalledHash = spControl->GetExecutes(iView).back().uListHash;

        // 2) 다시 보는 동안 새 변경이 온다: 새로 캡처해 목록을 다시 만든다
        source.SetCell(1, 1, BgCell(kGreen));
        thread.Wake(iView);
        QVERIFY(QTest::qWaitFor([&] { return source.GetCaptureCount() > iCapturesStalled; }, kTimeoutMs));

        // 3) 막힘이 풀리면 새 상태를 한 번 표시한다
        spControl->iNotReadyFrames = 0;
        QVERIFY(QTest::qWaitFor([&] { return CountPresented(*spControl, iView, iExecutesBefore) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);
        const FakeGpuControl::ExecuteRecord presented = FirstPresented(*spControl, iView, iExecutesBefore);
        QVERIFY(presented.bHasWatchedRect);               // 두 번째 변경이 들어 있다
        QVERIFY(presented.uListHash != uStalledHash);
        QCOMPARE(CountPresented(*spControl, iView, iExecutesBefore), 1);
        QVERIFY(thread.DetachViewAndWait(iView, 500));
    }

    void focusChangeDuringNotReadyRetryRebuildsList()
    {
        // 다시 보는 동안 포커스가 바뀌면(커서 모양이 바뀐다) 옛 목록 대신 지금 포커스로 다시 만든다
        FakeFrameSource source(kCols, kRows, 0);
        TerminalCursor cursor;
        cursor.bVisible = true;
        cursor.bBlinking = false;
        cursor.eShape = ECursorShape::BLOCK;
        cursor.iCol = 2;
        cursor.iRow = 1;
        source.SetCursor(cursor);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        thread.SetFont(BaseFont());
        thread.Start();
        const int iView = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        thread.SetFocus(iView, true);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iView) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        const int iExecutesBefore = spControl->CountExecutes(iView);
        QVERIFY(StallAfterChange(thread, source, *spControl, iView, 0));
        const int iCapturesStalled = source.GetCaptureCount();
        const uint64_t uFocusedHash = spControl->GetExecutes(iView).back().uListHash;

        thread.SetFocus(iView, false);
        QVERIFY(QTest::qWaitFor([&] { return source.GetCaptureCount() > iCapturesStalled; }, kTimeoutMs));
        QVERIFY(QTest::qWaitFor([&] { return spControl->GetExecutes(iView).back().uListHash != uFocusedHash; }, kTimeoutMs));
        const uint64_t uUnfocusedHash = spControl->GetExecutes(iView).back().uListHash;

        spControl->iNotReadyFrames = 0;
        QVERIFY(QTest::qWaitFor([&] { return CountPresented(*spControl, iView, iExecutesBefore) >= 1; }, kTimeoutMs));
        QCOMPARE(FirstPresented(*spControl, iView, iExecutesBefore).uListHash, uUnfocusedHash);
        QVERIFY(thread.DetachViewAndWait(iView, 500));
    }

    void blinkChangeDuringNotReadyRetryRebuildsList()
    {
        // 다시 보는 동안 깜빡임 상태가 바뀌면(530ms) 옛 목록 대신 지금 깜빡임 상태로 다시 만든다.
        // 막힘이 꺼진 구간(530~1060ms)에 풀리면 꺼진 커서를 그린다(옛 목록을 내면 다음 전환까지 켜진 커서가 남는다).
        FakeFrameSource source(kCols, kRows, 0);
        TerminalCursor cursor;
        cursor.bVisible = true;
        cursor.bBlinking = true;
        cursor.eShape = ECursorShape::BLOCK;
        cursor.iCol = 2;
        cursor.iRow = 1;
        source.SetCursor(cursor);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        thread.SetFont(BaseFont());
        thread.Start();
        const int iView = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        thread.SetFocus(iView, true);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iView) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        const int iExecutesBefore = spControl->CountExecutes(iView);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(StallAfterChange(thread, source, *spControl, iView, 0));   // 출력: 커서가 켜진 채 깜빡임 주기를 다시 시작한다
        const int iCapturesStalled = source.GetCaptureCount();
        const uint64_t uBlinkOnHash = spControl->GetExecutes(iView).back().uListHash;

        QVERIFY(QTest::qWaitFor([&] { return source.GetCaptureCount() > iCapturesStalled; }, kTimeoutMs));   // ~530ms: 꺼짐
        QVERIFY(QTest::qWaitFor([&] { return spControl->GetExecutes(iView).back().uListHash != uBlinkOnHash; }, kTimeoutMs));
        const uint64_t uBlinkOffHash = spControl->GetExecutes(iView).back().uListHash;
        spControl->iNotReadyFrames = 0;
        QVERIFY(QTest::qWaitFor([&] { return CountPresented(*spControl, iView, iExecutesBefore) >= 1; }, kTimeoutMs));
        const qint64 llPresentedMs = timer.elapsed();
        QVERIFY2(llPresentedMs < kBlinkOffEndMs, qPrintable(QString::number(llPresentedMs)));   // 아직 꺼진 구간이다
        QCOMPARE(FirstPresented(*spControl, iView, iExecutesBefore).uListHash, uBlinkOffHash);
        QVERIFY(thread.DetachViewAndWait(iView, 500));
    }

    void repeatedSurfaceLossFallsBackToCpu()
    {
        // 위젯처럼: surface 를 잃으면 떼고 같은 공급원으로 새 핸들을 붙인다. 표시 없이 3번 잇달아 잃으면(Windows 는 HWND 를
        // 다시 쓰므로 NATIVE_WINDOW_IN_USE 가 이어질 수 있다) 다시 붙이라고 하지 않고 그 화면만 CPU 로 바꿔 달라고 한다.
        FakeFrameSource source(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        QSignalSpy spyLost(&thread, &TerminalRenderThread::ViewSurfaceLost);
        QSignalSpy spyFallback(&thread, &TerminalRenderThread::ViewNeedsCpuFallback);
        QSignalSpy spyStrategy(&thread, &TerminalRenderThread::StrategyChanged);
        std::vector<int> vecViews;
        connect(&thread, &TerminalRenderThread::ViewSurfaceLost, &thread, [&](int viewId)
        {
            thread.DetachView(viewId);
            vecViews.push_back(thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0));
        });
        thread.SetFont(BaseFont());
        thread.Start();
        vecViews.push_back(thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0));
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(vecViews.front()) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        spControl->iSurfaceLostFrames = 1000;   // 계속 잃는다
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("lost its surface")));
        source.SetCell(0, 0, BgCell(kRed));
        thread.Wake(vecViews.front());
        QVERIFY(spyFallback.wait(kTimeoutMs));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyLost.count(), 2);              // 두 번은 다시 붙여 본다
        QCOMPARE(spyFallback.count(), 1);
        QCOMPARE(int(vecViews.size()), 3);
        QCOMPARE(spyFallback.at(0).at(0).toInt(), vecViews.back());   // 세 번째 화면은 CPU 로
        QCOMPARE(spyStrategy.count(), 0);          // GPU 는 그대로(다른 화면은 GPU)
        QCOMPARE(thread.GetActiveStrategy(), ETerminalRenderStrategy::GPU);
        thread.DetachView(vecViews.back());
    }

    void surfaceLossCountResetsAfterPresentAndRelease()
    {
        // 셈은 잇달아 잃은 횟수다: 한 번이라도 표시하면 다시 센다. 공급원을 놓으면(SourceReleased) 셈도 잊는다.
        FakeFrameSource source(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        QSignalSpy spyLost(&thread, &TerminalRenderThread::ViewSurfaceLost);
        QSignalSpy spyFallback(&thread, &TerminalRenderThread::ViewNeedsCpuFallback);
        QSignalSpy spyReleased(&thread, &TerminalRenderThread::SourceReleased);
        std::vector<int> vecViews;
        int iReattachesLeft = 1000;
        connect(&thread, &TerminalRenderThread::ViewSurfaceLost, &thread, [&](int viewId)
        {
            thread.DetachView(viewId);
            if (iReattachesLeft-- > 0)
                vecViews.push_back(thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0));
        });
        thread.SetFont(BaseFont());
        thread.Start();
        vecViews.push_back(thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0));
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(vecViews.back()) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        // 2번 잃고 세 번째 화면이 표시한다 → 다시 2번 잃어도 CPU 로 바꾸지 않는다
        for (int iBatch = 0; iBatch < 2; ++iBatch)
        {
            const int iLostBefore = spyLost.count();
            spControl->iSurfaceLostFrames = 2;
            source.SetCell(0, iBatch, BgCell(kRed));
            thread.Wake(vecViews.back());
            QVERIFY(QTest::qWaitFor([&] { return spyLost.count() == iLostBefore + 2 && spControl->CountExecutes(vecViews.back()) >= 1; },
                                    kTimeoutMs));
            QTest::qWait(kSettleMs);
        }
        QCOMPARE(spyFallback.count(), 0);

        // 2번 잃은 뒤 다시 붙이지 않고 떼면 공급원이 놓인다 → 같은 공급원으로 다시 붙여 한 번 잃어도 셋째가 아니다
        iReattachesLeft = 1;
        spControl->iSurfaceLostFrames = 2;
        source.SetCell(1, 0, BgCell(kRed));
        thread.Wake(vecViews.back());
        QVERIFY(spyReleased.wait(kTimeoutMs));
        const int iLostBefore = spyLost.count();
        spControl->iSurfaceLostFrames = 1;
        iReattachesLeft = 0;
        const int iLate = thread.AttachView(&source, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return spyLost.count() == iLostBefore + 1 || spyFallback.count() > 0; }, kTimeoutMs));
        QTest::qWait(kSettleMs);
        QCOMPARE(spyFallback.count(), 0);
        QCOMPARE(spyLost.last().at(0).toInt(), iLate);
    }

    void autoReportsCpuWithoutNativeSurfaces()
    {
        // 네이티브 화면(NSView/HWND)을 만들 수 없는 QPA(offscreen, Linux)에서는 처음부터 CPU 로 알린다:
        // 패널이 GPU 위젯을 만들었다가 바로 바꿔 끼우지 않게 한다. GPU Strategy 를 넣어 준 테스트·도구는 GPU 다.
        if (QGuiApplication::platformName() != QStringLiteral("offscreen"))
            QSKIP("offscreen QPA 에서만 본다");
        TerminalRenderThread automatic(ETerminalRendererMode::AUTO);
        QCOMPARE(automatic.GetActiveStrategy(), ETerminalRenderStrategy::CPU);
        TerminalRenderThread injected(ETerminalRendererMode::AUTO);
        UseFakeGpu(injected, std::make_shared<FakeGpuControl>());
        QCOMPARE(injected.GetActiveStrategy(), ETerminalRenderStrategy::GPU);
        TerminalRenderThread forcedCpu(ETerminalRendererMode::CPU);
        UseFakeGpu(forcedCpu, std::make_shared<FakeGpuControl>());
        QCOMPARE(forcedCpu.GetActiveStrategy(), ETerminalRenderStrategy::CPU);
    }

    void fallbackSignalsSeeCpuState()
    {
        // 장치를 잃어 GPU 화면을 CPU 로 바꿔 달라고 할 때, 받는 쪽이 보는 상태는 이미 CPU 다(새 위젯을 CPU 로 만든다)
        FakeFrameSource sourceA(kCols, kRows, 0);
        FakeFrameSource sourceB(kCols, kRows, 0);
        auto spControl = std::make_shared<FakeGpuControl>();
        std::atomic<int> iReleases{0};
        TerminalRenderThread thread(ETerminalRendererMode::AUTO);
        UseFakeGpu(thread, spControl);
        std::vector<ETerminalRenderStrategy> vecSeenStrategy;
        std::vector<QString> vecSeenError;
        connect(&thread, &TerminalRenderThread::ViewNeedsCpuFallback, &thread, [&](int)
        {
            vecSeenStrategy.push_back(thread.GetActiveStrategy());
            vecSeenError.push_back(thread.GetGpuError());
        });
        QSignalSpy spyStrategy(&thread, &TerminalRenderThread::StrategyChanged);
        thread.SetFont(BaseFont());
        thread.Start();
        const int iA = thread.AttachView(&sourceA, GpuHandle(iReleases), ViewSize(), 1.0);
        const int iB = thread.AttachView(&sourceB, GpuHandle(iReleases), ViewSize(), 1.0);
        QVERIFY(QTest::qWaitFor([&] { return spControl->CountExecutes(iA) >= 1 && spControl->CountExecutes(iB) >= 1; }, kTimeoutMs));
        QTest::qWait(kSettleMs);

        spControl->bDeviceLostNext = true;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("GPU device lost")));
        sourceA.SetCell(0, 0, BgCell(kRed));
        thread.Wake(iA);
        QVERIFY(spyStrategy.wait(kTimeoutMs));
        QCOMPARE(int(vecSeenStrategy.size()), 2);
        for (size_t iSeen = 0; iSeen < vecSeenStrategy.size(); ++iSeen)
        {
            QCOMPARE(vecSeenStrategy[iSeen], ETerminalRenderStrategy::CPU);
            QVERIFY(!vecSeenError[iSeen].isEmpty());
        }
        thread.DetachView(iA);
        thread.DetachView(iB);
    }
};

QObject* CreateTerminalRenderThreadGpuTest() { return new TerminalRenderThreadGpuTest(); }

#include "TerminalRenderThreadGpuTest.moc"
