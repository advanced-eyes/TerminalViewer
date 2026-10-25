#include "ShowcaseWindow.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"
#include "TerminalViewer/Testing/FakeFrameSource.h"
#include "TerminalViewer/Thread/TerminalRenderThread.h"
#include "TerminalViewer/UI/TerminalSurface.h"
#include "ScopedEnv.h"
#include "TestFonts.h"

#include <QApplication>
#include <QGuiApplication>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <cmath>
#include <memory>

namespace
{
constexpr int kCols = 60;
constexpr int kRows = 20;
constexpr int kFontPx = 14;
constexpr int kTimeoutMs = 5000;
constexpr int kScrollback = ShowcaseScene::kScrollbackLines;
constexpr int kSettleBeforeGrabMs = 500;   // main 의 --grab 과 같다
constexpr int kMinGrabColors = 16;         // 잡은 그림에 이보다 많은 색(색 견본 · 글자)이 있어야 한다
constexpr int kGrabSampleStepPx = 3;       // 색을 셀 때 가로 · 세로로 이만큼 건너뛰며 본다
constexpr int kGrabSizeTolerancePx = 1;    // 잡은 그림과 창 크기 x 배율의 차이(반올림)

QFont MakeBaseFont()
{
    QFont font(QStringLiteral("Cascadia Mono"));
    font.setPixelSize(kFontPx);
    return font;
}

// 공급원 · 렌더 스레드 · 창. 끄는 순서는 main 과 같다: 창 -> Stop -> 공급원
struct Rig
{
    explicit Rig(ETerminalRendererMode mode, bool failingGpu = false, double minContrast = 1.0)
        : source(kCols, kRows, kScrollback)
        , thread(mode)
        , upReleased(std::make_unique<QSignalSpy>(&thread, &TerminalRenderThread::SourceReleased))
        , upWindow()
    {
        if (failingGpu)
        {
            thread.SetGpuStrategyFactory([](QString* pError) -> std::unique_ptr<ITerminalRenderStrategy>
                                         {
                                             *pError = QStringLiteral("test GPU");
                                             return nullptr;
                                         });
        }
        thread.SetFont(MakeBaseFont());
        TerminalTheme theme = thread.GetTheme();
        theme.dMinContrastRatio = minContrast;   // main 의 --min-contrast 처럼 창을 만들기 전에 정한다
        thread.SetTheme(theme);
        thread.Start();
        upWindow = std::make_unique<ShowcaseWindow>(&thread, &source, mode, QSize(kCols, kRows));
        upWindow->show();
    }

    ~Rig() { Shutdown(); }

    void Shutdown()
    {
        upWindow.reset();
        thread.Stop();
    }

    bool WaitFrame() const
    {
        return QTest::qWaitFor([this]() { return upWindow->GetFrameCount() > 0; }, kTimeoutMs);
    }

    QSize GetSourceGrid()
    {
        TerminalFrame frame;
        source.CaptureFrame(TerminalCaptureRequest(), frame);
        return QSize(frame.iCols, frame.iRows);
    }

    QSize GetSceneGrid() const { return QSize(upWindow->GetScene().GetCols(), upWindow->GetScene().GetRows()); }

    // 대비 절 첫 줄을 그린 그림(CPU 위젯을 잡아 그 줄만 자른다)
    QImage GrabContrastLine()
    {
        const int iRow = upWindow->GetScene().GetSectionLine(EShowcaseSection::CONTRAST) - source.GetViewportTop();
        if (iRow < 0 || iRow >= upWindow->GetScene().GetRows())
            return QImage();
        const CellMetrics metrics = CellMetrics::Compute(thread.GetFont(), 1.0);
        const int iPadding = thread.GetTheme().iPaddingLogicalPx;
        const QImage imgWindow = upWindow->GrabFrame().convertToFormat(QImage::Format_RGB32);
        return imgWindow.copy(iPadding, iPadding + iRow * metrics.iCellHeight, upWindow->GetScene().GetCols() * metrics.iCellWidth,
                              metrics.iCellHeight);
    }

    // 키를 누른 뒤 다시 그린 프레임을 기다린다
    bool PressAndWaitFrame(Qt::Key key)
    {
        const int iFrames = upWindow->GetFrameCount();
        QTest::keyClick(upWindow->GetSurface(), key);
        if (!QTest::qWaitFor([this, iFrames]() { return upWindow->GetFrameCount() > iFrames; }, kTimeoutMs))
            return false;
        QTest::qWait(kSettleBeforeGrabMs);
        return true;
    }

    FakeFrameSource source;
    TerminalRenderThread thread;
    std::unique_ptr<QSignalSpy> upReleased;
    std::unique_ptr<ShowcaseWindow> upWindow;
};
}   // namespace

class ShowcaseWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestFonts::Register(); }

    void fillsTheMeasuredGrid()
    {
        Rig rig(ETerminalRendererMode::CPU);
        QVERIFY(rig.WaitFrame());
        const QSize sizeGrid = rig.upWindow->GetSurface()->GetGridSize();
        QCOMPARE(sizeGrid, QSize(kCols, kRows));   // 창 크기를 열 x 행 에 맞췄다
        QCOMPARE(rig.GetSceneGrid(), sizeGrid);
        QCOMPARE(rig.GetSourceGrid(), sizeGrid);
        QVERIFY(!rig.upWindow->GetSurface()->IsGpu());
        QVERIFY(rig.upWindow->windowTitle().contains(QStringLiteral("CPU (--renderer cpu)")));
    }

    void fontKeysRelayoutTheSource()
    {
        Rig rig(ETerminalRendererMode::CPU);
        QVERIFY(rig.WaitFrame());
        TerminalSurface* pSurface = rig.upWindow->GetSurface();
        QTest::keyClick(pSurface, Qt::Key_Plus);   // 위젯이 받지 않은 키는 창으로 올라온다
        QCOMPARE(rig.thread.GetFont().pixelSize(), kFontPx + 1);
        const QSize sizeBigger = pSurface->GetGridSize();
        QVERIFY(sizeBigger.width() < kCols);
        QCOMPARE(rig.GetSceneGrid(), sizeBigger);
        QCOMPARE(rig.GetSourceGrid(), sizeBigger);
        QVERIFY(rig.upWindow->windowTitle().contains(QStringLiteral("%1 px").arg(kFontPx + 1)));

        // 12px 까지 줄인다(Cascadia Mono 는 13px 도 14px 와 칸 폭이 같다)
        QTest::keyClick(pSurface, Qt::Key_Minus);
        QTest::keyClick(pSurface, Qt::Key_Minus);
        QTest::keyClick(pSurface, Qt::Key_Minus);
        QCOMPARE(rig.thread.GetFont().pixelSize(), kFontPx - 2);
        const QSize sizeSmaller = pSurface->GetGridSize();
        QVERIFY(sizeSmaller.width() > kCols);
        QCOMPARE(rig.GetSceneGrid(), sizeSmaller);
        QCOMPARE(rig.GetSourceGrid(), sizeSmaller);
        // 다시 채운 공급원의 어떤 줄도 새 열 수를 넘지 않는다
        for (const ShowcaseLine& line : rig.upWindow->GetScene().GetLines())
            QVERIFY(int(line.vecCells.size()) <= sizeSmaller.width());
    }

    void resizingTheWindowRelayouts()
    {
        Rig rig(ETerminalRendererMode::CPU);
        QVERIFY(rig.WaitFrame());
        rig.upWindow->resize(rig.upWindow->width() / 2, rig.upWindow->height());
        QTRY_VERIFY(rig.GetSceneGrid().width() < kCols / 2 + 2);
        QCOMPARE(rig.GetSceneGrid(), rig.upWindow->GetSurface()->GetGridSize());
        QCOMPARE(rig.GetSourceGrid(), rig.GetSceneGrid());
    }

    void cursorKeyCyclesShape()
    {
        Rig rig(ETerminalRendererMode::CPU);
        QVERIFY(rig.WaitFrame());
        QCOMPARE(rig.upWindow->GetScene().GetCursor().eShape, ECursorShape::BLOCK);
        QTest::keyClick(rig.upWindow->GetSurface(), Qt::Key_C);
        QCOMPARE(rig.upWindow->GetScene().GetCursor().eShape, ECursorShape::BAR);
        QVERIFY(rig.upWindow->windowTitle().contains(QStringLiteral("cursor bar")));
    }

    // M: 최소 대비 끔 <-> 4.5. 테마를 바꿔 다시 그리고(대비 절의 글자색이 바뀐다), 창 제목에 지금 값을 보인다
    void minContrastKeyToggles()
    {
        Rig rig(ETerminalRendererMode::CPU);
        QVERIFY(rig.WaitFrame());
        QCOMPARE(rig.thread.GetTheme().dMinContrastRatio, 1.0);
        QVERIFY2(rig.upWindow->windowTitle().contains(QStringLiteral("contrast off")), qPrintable(rig.upWindow->windowTitle()));
        QTest::qWait(kSettleBeforeGrabMs);
        const QImage imgOff = rig.GrabContrastLine();
        QVERIFY(!imgOff.isNull());

        QVERIFY(rig.PressAndWaitFrame(Qt::Key_M));
        QCOMPARE(rig.thread.GetTheme().dMinContrastRatio, ShowcaseWindow::kToggledMinContrast);
        QCOMPARE(ShowcaseWindow::kToggledMinContrast, 4.5);
        QVERIFY2(rig.upWindow->windowTitle().contains(QStringLiteral("contrast 4.5")), qPrintable(rig.upWindow->windowTitle()));
        const QImage imgOn = rig.GrabContrastLine();
        QVERIFY(!imgOn.isNull());
        QVERIFY(imgOn != imgOff);

        QVERIFY(rig.PressAndWaitFrame(Qt::Key_M));
        QCOMPARE(rig.thread.GetTheme().dMinContrastRatio, 1.0);
        QVERIFY(rig.upWindow->windowTitle().contains(QStringLiteral("contrast off")));
        QCOMPARE(rig.GrabContrastLine(), imgOff);   // 되돌리면 같은 그림
    }

    // 처음 값(main 의 --min-contrast)을 제목에 보이고, M 은 그 값에서 끔으로, 다시 4.5 로 간다
    void minContrastStartsFromTheTheme()
    {
        Rig rig(ETerminalRendererMode::CPU, false, 7.0);
        QVERIFY(rig.WaitFrame());
        QVERIFY2(rig.upWindow->windowTitle().contains(QStringLiteral("contrast 7")), qPrintable(rig.upWindow->windowTitle()));
        QTest::keyClick(rig.upWindow->GetSurface(), Qt::Key_M);
        QCOMPARE(rig.thread.GetTheme().dMinContrastRatio, 1.0);
        QVERIFY(rig.upWindow->windowTitle().contains(QStringLiteral("contrast off")));
        QTest::keyClick(rig.upWindow->GetSurface(), Qt::Key_M);
        QCOMPARE(rig.thread.GetTheme().dMinContrastRatio, 4.5);
    }

    void wheelAndKeysScroll()
    {
        Rig rig(ETerminalRendererMode::CPU);
        QVERIFY(rig.WaitFrame());
        QCOMPARE(rig.source.GetViewportTop(), kScrollback);
        // 위로 한 칸(+120) = 지난 출력 쪽으로 3줄
        QWheelEvent wheel(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(rig.upWindow.get(), &wheel);
        QTRY_COMPARE_WITH_TIMEOUT(rig.source.GetViewportTop(), kScrollback - 3, kTimeoutMs);
        // 프롬프트 줄이 화면 밖으로 나갔다: 커서 · 조합을 숨긴다
        QTRY_VERIFY_WITH_TIMEOUT(!rig.upWindow->GetScene().GetCursor().bVisible, kTimeoutMs);
        QVERIFY(!rig.upWindow->GetScene().GetPreedit());

        QTest::keyClick(rig.upWindow->GetSurface(), Qt::Key_PageUp);
        QTRY_COMPARE_WITH_TIMEOUT(rig.source.GetViewportTop(), kScrollback - 3 - (kRows - 1), kTimeoutMs);
        QTest::keyClick(rig.upWindow->GetSurface(), Qt::Key_End);
        QTRY_COMPARE_WITH_TIMEOUT(rig.source.GetViewportTop(), kScrollback, kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(rig.upWindow->GetScene().GetCursor().bVisible, kTimeoutMs);
        QCOMPARE(rig.upWindow->GetScene().GetCursor().iRow, kRows - 1);
    }

    void liveLineKeepsChanging()
    {
        Rig rig(ETerminalRendererMode::CPU);
        QVERIFY(rig.WaitFrame());
        const ShowcaseScene& scene = rig.upWindow->GetScene();
        const auto funcLiveText = [&scene]()
        {
            std::u32string strText;
            for (const TerminalCell& cell : scene.GetLines()[size_t(scene.GetSectionLine(EShowcaseSection::LIVE))].vecCells)
                strText += cell.uCodepoint ? char32_t(cell.uCodepoint) : U' ';
            return strText;
        };
        const std::u32string strFirst = funcLiveText();
        QTRY_VERIFY_WITH_TIMEOUT(funcLiveText() != strFirst, kTimeoutMs);
        const int iFrames = rig.upWindow->GetFrameCount();
        QTRY_VERIFY_WITH_TIMEOUT(rig.upWindow->GetFrameCount() > iFrames, kTimeoutMs);   // 바뀔 때마다 Wake 로 다시 그린다
    }

    void quitKeysCloseTheWindow()
    {
        Rig rig(ETerminalRendererMode::CPU);
        QVERIFY(rig.WaitFrame());
        QTest::keyClick(rig.upWindow->GetSurface(), Qt::Key_Q);
        QVERIFY(!rig.upWindow->isVisible());
        rig.upWindow->show();
        QTest::keyClick(rig.upWindow->GetSurface(), Qt::Key_Escape);
        QVERIFY(!rig.upWindow->isVisible());
    }

    void gpuFailureSwapsToCpuSurface()
    {
        // GPU 를 만들 수 없는 GPU 공장: AUTO 는 GPU 위젯으로 시작하고, 네이티브 화면이 없는 offscreen 에서 CPU 로 바꿔 끼운다
        Rig rig(ETerminalRendererMode::AUTO, true);
        QTRY_VERIFY_WITH_TIMEOUT(rig.upWindow->GetSurface() && !rig.upWindow->GetSurface()->IsGpu(), kTimeoutMs);
        QVERIFY(rig.WaitFrame());
        QCOMPARE(rig.GetSceneGrid(), rig.upWindow->GetSurface()->GetGridSize());
        QVERIFY(rig.upWindow->windowTitle().contains(QStringLiteral("CPU")));
        QCOMPARE(rig.upReleased->count(), 0);   // 바꿔 끼우는 동안 공급원을 놓지 않는다
        rig.Shutdown();
        QCOMPARE(rig.upReleased->count(), 1);
    }

    void cpuReasonNamesTheDisableVariable()
    {
        // TERMINALVIEWER_VK_DISABLE=1 로 Vulkan 을 끄면 CPU 인 이유로 그 변수를 알린다("Vulkan library not found" 가 아니다)
        ScopedEnv disable("TERMINALVIEWER_VK_DISABLE", "1");
        Rig rig(ETerminalRendererMode::AUTO);
        QVERIFY(rig.WaitFrame());
        QVERIFY(!rig.upWindow->GetSurface()->IsGpu());
        const QString strTitle = rig.upWindow->windowTitle();
#ifdef TERMINALVIEWER_GPU
        QVERIFY2(strTitle.contains(QStringLiteral("CPU (TERMINALVIEWER_VK_DISABLE=1)")), qPrintable(strTitle));
#else
        QVERIFY2(strTitle.contains(QStringLiteral("CPU (built with TERMINALVIEWER_GPU=OFF)")), qPrintable(strTitle));   // 빌드가 먼저다
#endif
    }

    void gpuGrabCapturesTheScreen()
    {
        // 실제 windows QPA 에서만 돈다(ctest 는 offscreen 이라 건너뛴다. 직접:
        //   QT_QPA_PLATFORM=windows TerminalViewerTests ShowcaseWindowTest gpuGrabCapturesTheScreen). 창이 잠깐 뜬다.
        // GPU 화면은 렌더 스레드가 네이티브 자식 창에 바로 표시하므로 창 DC 에는 그 내용이 없다: 화면에서 잡아야 그린 것이 보인다
        if (QGuiApplication::platformName() != QStringLiteral("windows"))
            QSKIP("windows QPA only");
        Rig rig(ETerminalRendererMode::AUTO);
        rig.thread.SetStatsEnabled(true);   // GPU 화면은 FrameReady 가 없어 프레임을 FrameStats 로 센다
        if (!rig.upWindow->GetSurface() || !rig.upWindow->GetSurface()->IsGpu())
            QSKIP("no Vulkan device");
        rig.upWindow->raise();
        rig.upWindow->activateWindow();
        QVERIFY(rig.WaitFrame());
        QTest::qWait(kSettleBeforeGrabMs);
        const QImage imgGrab = rig.upWindow->GrabFrame().convertToFormat(QImage::Format_RGB32);
        QVERIFY(!imgGrab.isNull());
        const qreal dRatio = rig.upWindow->devicePixelRatioF();
        QVERIFY(std::abs(imgGrab.width() - rig.upWindow->width() * dRatio) <= kGrabSizeTolerancePx);
        QVERIFY(std::abs(imgGrab.height() - rig.upWindow->height() * dRatio) <= kGrabSizeTolerancePx);
        // 왼쪽 위 여백은 터미널 배경색이고, 글자 · 색 견본이 있어 여러 색이 보인다
        const QRgb background = qRgb(ShowcaseScene::kBackground.r, ShowcaseScene::kBackground.g, ShowcaseScene::kBackground.b);
        QCOMPARE(imgGrab.pixel(1, 1), background);
        QSet<QRgb> setColors;
        for (int iY = 0; iY < imgGrab.height(); iY += kGrabSampleStepPx)
        {
            for (int iX = 0; iX < imgGrab.width(); iX += kGrabSampleStepPx)
                setColors.insert(imgGrab.pixel(iX, iY));
        }
        QVERIFY2(setColors.size() > kMinGrabColors, qPrintable(QStringLiteral("%1 colors").arg(setColors.size())));
    }

    void shutdownReleasesTheSourceOnce()
    {
        Rig rig(ETerminalRendererMode::CPU);
        QVERIFY(rig.WaitFrame());
        QCOMPARE(rig.upReleased->count(), 0);
        rig.Shutdown();
        QCOMPARE(rig.upReleased->count(), 1);
        QCOMPARE(rig.upReleased->at(0).at(0).value<ITerminalFrameSource*>(), static_cast<ITerminalFrameSource*>(&rig.source));
    }
};

QObject* CreateShowcaseWindowTest() { return new ShowcaseWindowTest(); }

#include "ShowcaseWindowTest.moc"
