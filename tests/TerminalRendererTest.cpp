#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"

#include "ScopedEnv.h"
#include "TestFonts.h"

#include <QRegularExpression>
#include <QTest>

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
