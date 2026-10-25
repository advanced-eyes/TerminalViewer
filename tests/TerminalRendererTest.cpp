#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"

#include "ScopedEnv.h"
#include "TestFonts.h"

#include <QRegularExpression>
#include <QTest>

#include <cstring>
#include <vector>

class TerminalRendererTest : public QObject
{
    Q_OBJECT

private:
    static QFont BaseFont()
    {
        QFont font(QStringLiteral("Cascadia Mono"));
        font.setPixelSize(14);
        return font;
    }

    static DrawList MakeList()
    {
        DrawList list;
        list.clearColor = TerminalRgb{0, 0, 255};
        list.iCols = 20;
        list.iRows = 5;
        list.iCellWidth = 10;
        list.iCellHeight = 20;
        return list;
    }

    static uint32_t GetPixel(const QImage& image, int x, int y)
    {
        return reinterpret_cast<const uint32_t*>(image.constScanLine(y))[x];
    }

private slots:
    void initTestCase() { TestFonts::Register(); }

    void firstFrameIsFullThenDirtyBand()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalRenderer renderer;
        QVERIFY(renderer.AttachView(1, {}, QSize(200, 100)));
        QVERIFY(renderer.IsReady(1));

        DrawList list = MakeList();
        TerminalRenderInput in{1, &list, &cache};
        TerminalRenderOutput out;
        QVERIFY(renderer.Execute(in, out));
        QVERIFY(out.bPresented);
        QCOMPARE(out.rectPatch, QRect(0, 0, 200, 100));
        QCOMPARE(out.imgPatch.size(), QSize(200, 100));

        list.bFullRedraw = false;
        list.vecDirtyRows = {2};
        QVERIFY(renderer.Execute(in, out));
        QVERIFY(out.bPresented);
        QCOMPARE(out.rectPatch, QRect(0, 40, 200, 20));
        QCOMPARE(out.imgPatch.size(), QSize(200, 20));
    }

    void resizeForcesFullPatch()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalRenderer renderer;
        QVERIFY(renderer.AttachView(1, {}, QSize(200, 100)));
        DrawList list = MakeList();
        TerminalRenderInput in{1, &list, &cache};
        TerminalRenderOutput out;
        QVERIFY(renderer.Execute(in, out));

        renderer.ResizeView(1, QSize(100, 60));
        list.iCols = 10;
        list.iRows = 3;
        list.bFullRedraw = false;
        list.vecDirtyRows = {0};
        QVERIFY(renderer.Execute(in, out));
        QCOMPARE(out.rectPatch, QRect(0, 0, 100, 60));
    }

    void remainderAreaUsesViewSize()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalRenderer renderer;
        QVERIFY(renderer.AttachView(1, {}, QSize(205, 107)));
        DrawList list = MakeList();
        list.iCols = 10;
        list.iRows = 5;
        list.iCellWidth = 20;
        list.iCellHeight = 20;
        TerminalRenderInput in{1, &list, &cache};
        TerminalRenderOutput out;
        QVERIFY(renderer.Execute(in, out));
        QCOMPARE(out.rectPatch, QRect(0, 0, 205, 107));
        QCOMPARE(out.imgPatch.size(), QSize(205, 107));
        QCOMPARE(reinterpret_cast<const uint32_t*>(out.imgPatch.constScanLine(106))[204], 0xFF0000FFu);

        list.bFullRedraw = false;
        list.vecDirtyRows = {1};
        QVERIFY(renderer.Execute(in, out));
        QCOMPARE(out.rectPatch, QRect(0, 20, 205, 20));
    }

    // 받는 쪽이 조각을 쥐고 있어도 다음 프레임이 그 픽셀을 바꾸지 않는다(조각이 화면 이미지와 픽셀을 함께 쓰더라도).
    // 쥐고 있던 동안에도 다음 프레임의 조각 · 그 뒤의 부분 조각은 바른 내용이다.
    void heldPatchKeepsItsPixels()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalRenderer renderer;
        QVERIFY(renderer.AttachView(1, {}, QSize(200, 100)));
        DrawList list = MakeList();
        TerminalRenderInput in{1, &list, &cache};
        TerminalRenderOutput first;
        QVERIFY(renderer.Execute(in, first));
        QCOMPARE(first.rectPatch, QRect(0, 0, 200, 100));
        const QImage imgHeld = first.imgPatch;
        const QImage imgHeldCopy = first.imgPatch.copy();
        QCOMPARE(GetPixel(imgHeld, 5, 45), 0xFF0000FFu);

        list.clearColor = TerminalRgb{255, 0, 0};
        TerminalRenderOutput second;
        QVERIFY(renderer.Execute(in, second));
        QCOMPARE(second.rectPatch, QRect(0, 0, 200, 100));
        QCOMPARE(GetPixel(second.imgPatch, 5, 45), 0xFFFF0000u);
        QVERIFY(imgHeld == imgHeldCopy);
        QCOMPARE(GetPixel(imgHeld, 5, 45), 0xFF0000FFu);

        const QImage imgSecondHeld = second.imgPatch;
        list.clearColor = TerminalRgb{0, 255, 0};
        list.bFullRedraw = false;
        list.vecDirtyRows = {2};
        TerminalRenderOutput third;
        QVERIFY(renderer.Execute(in, third));
        QCOMPARE(third.rectPatch, QRect(0, 40, 200, 20));
        QCOMPARE(GetPixel(third.imgPatch, 5, 5), 0xFF00FF00u);
        QCOMPARE(GetPixel(imgSecondHeld, 5, 45), 0xFFFF0000u);

        // 쥐지 않은 다음 전체 프레임(빨강) 뒤의 부분 조각도 바르다(마지막에 행 1 · 3 만 초록, 나머지는 그 전체 프레임의 빨강이 남아 있다)
        list.clearColor = TerminalRgb{255, 0, 0};
        list.bFullRedraw = true;
        TerminalRenderOutput fourth;
        QVERIFY(renderer.Execute(in, fourth));
        fourth = TerminalRenderOutput();
        list.clearColor = TerminalRgb{0, 255, 0};
        list.bFullRedraw = false;
        list.vecDirtyRows = {1};
        TerminalRenderOutput fifth;
        QVERIFY(renderer.Execute(in, fifth));
        QCOMPARE(fifth.rectPatch, QRect(0, 20, 200, 20));
        QCOMPARE(GetPixel(fifth.imgPatch, 5, 5), 0xFF00FF00u);
        list.vecDirtyRows = {1, 3};
        TerminalRenderOutput sixth;
        QVERIFY(renderer.Execute(in, sixth));
        QCOMPARE(sixth.rectPatch, QRect(0, 0, 200, 100));        // 감싼 사각형(행 1..3)이 절반 이상: 화면 전체
        QCOMPARE(GetPixel(sixth.imgPatch, 5, 5), 0xFFFF0000u);    // 행 0: 앞 전체 프레임 그대로
        QCOMPARE(GetPixel(sixth.imgPatch, 5, 25), 0xFF00FF00u);   // 행 1
        QCOMPARE(GetPixel(sixth.imgPatch, 5, 45), 0xFFFF0000u);   // 행 2: 앞 전체 프레임 그대로
        QCOMPARE(GetPixel(sixth.imgPatch, 5, 65), 0xFF00FF00u);   // 행 3
    }

    // 바뀐 영역을 감싼 사각형이 화면의 절반 이상이면 조각은 화면 전체(복사하지 않는다), 절반보다 작으면 그 사각형이다.
    // 어느 쪽이든 조각을 덮어 쓴 화면은 화면 이미지와 같다.
    void patchIsWholeFrameWhenBoundingRectCoversHalf()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalRenderer renderer;
        QVERIFY(renderer.AttachView(1, {}, QSize(200, 100)));
        DrawList list = MakeList();
        TerminalRenderInput in{1, &list, &cache};
        TerminalRenderOutput out;
        QVERIFY(renderer.Execute(in, out));
        QImage imgShown = out.imgPatch.copy();

        struct Step
        {
            std::vector<int> vecRows;
            QRect rectExpected;
        };
        const std::vector<Step> vecSteps = {
            {{0}, QRect(0, 0, 200, 20)},
            {{0, 1}, QRect(0, 0, 200, 40)},                // 40%
            {{1, 3}, QRect(0, 0, 200, 100)},               // 감싼 사각형 행 1..3 = 60%
            {{0, 4}, QRect(0, 0, 200, 100)},               // 맨 위와 맨 아래
            {{2}, QRect(0, 40, 200, 20)},
            {{0, 1, 2, 3, 4}, QRect(0, 0, 200, 100)},
        };
        std::vector<uint32_t> vecRowColors(5, 0xFF0000FFu);   // 첫 전체 프레임: 파랑
        uint8_t uShade = 10;
        for (const Step& step : vecSteps)
        {
            list.bFullRedraw = false;
            list.vecDirtyRows = step.vecRows;
            list.clearColor = TerminalRgb{uShade, uint8_t(255 - uShade), 0};
            uShade = uint8_t(uShade + 40);
            for (int iRow : step.vecRows)
                vecRowColors[size_t(iRow)] = 0xFF000000u | (uint32_t(list.clearColor.r) << 16) | (uint32_t(list.clearColor.g) << 8);
            QVERIFY(renderer.Execute(in, out));
            QCOMPARE(out.rectPatch, step.rectExpected);
            QCOMPARE(out.imgPatch.size(), step.rectExpected.size());
            for (int iY = 0; iY < out.rectPatch.height(); ++iY)
                std::memcpy(imgShown.scanLine(out.rectPatch.y() + iY) + out.rectPatch.x() * 4, out.imgPatch.constScanLine(iY),
                            size_t(out.rectPatch.width()) * 4);
            for (int iRow = 0; iRow < 5; ++iRow)
            {
                for (int iY : {iRow * 20, iRow * 20 + 19})
                {
                    for (int iX : {0, 199})
                        QCOMPARE(GetPixel(imgShown, iX, iY), vecRowColors[size_t(iRow)]);
                }
            }
        }
    }

    void detachedViewFails()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalRenderer renderer;
        QVERIFY(renderer.AttachView(1, {}, QSize(200, 100)));
        renderer.DetachView(1);
        DrawList list = MakeList();
        TerminalRenderInput in{1, &list, &cache};
        TerminalRenderOutput out;
        QVERIFY(!renderer.Execute(in, out));
        TerminalRenderInput unknown{7, &list, &cache};
        QVERIFY(!renderer.Execute(unknown, out));
    }

    // GPU 를 만들 수 없으면(Vulkan 없음을 TERMINALVIEWER_VK_DISABLE=1 로 흉내, 또는 GPU 를 끈 빌드) CPU 로 남는다.
    // Vulkan 이 있을 때 GPU 로 바뀌는 것은 VulkanRendererTest 가 본다.
    void gpuStaysCpuWithoutVulkan()
    {
        ScopedEnv disable("TERMINALVIEWER_VK_DISABLE", "1");
        TerminalRenderer renderer;
        QCOMPARE(renderer.GetStrategy(), ETerminalRenderStrategy::CPU);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^Terminal renderer: GPU unavailable, drawing with CPU: ")));
        renderer.SetStrategy(ETerminalRenderStrategy::GPU);
        QCOMPARE(renderer.GetStrategy(), ETerminalRenderStrategy::CPU);
#ifdef TERMINALVIEWER_GPU
        QVERIFY(!renderer.GetGpuError().isEmpty());
#else
        // GPU 를 끈 빌드의 이유 문구는 라이브러리 옵션 이름을 쓴다.
        QCOMPARE(renderer.GetGpuError(), QStringLiteral("GPU 렌더러 없이 만든 빌드다(TERMINALVIEWER_GPU=OFF)"));
#endif
    }
};

QObject* CreateTerminalRendererTest() { return new TerminalRendererTest(); }

#include "TerminalRendererTest.moc"
