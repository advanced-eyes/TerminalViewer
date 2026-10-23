#include "TerminalViewer/Algorithm/TerminalRenderer/Common/BoxGlyphPainter.h"

#include "TestFonts.h"

#include <QTest>
#include <cmath>

class BoxGlyphPainterTest : public QObject
{
    Q_OBJECT

private:
    static QFont BaseFont()
    {
        QFont font(QStringLiteral("Cascadia Mono"));
        font.setPixelSize(14);
        return font;
    }

    static int Px(const QImage& image, int column, int row) { return image.constScanLine(row)[column]; }

    static int RowCoverage(const QImage& image, int row)
    {
        int iCount = 0;
        for (int iX = 0; iX < image.width(); ++iX) iCount += Px(image, iX, row) == 255 ? 1 : 0;
        return iCount;
    }

    static bool RowHasInk(const QImage& image, int row)
    {
        for (int iX = 0; iX < image.width(); ++iX)
        {
            if (Px(image, iX, row) > 0) return true;
        }
        return false;
    }

    static int ColCoverage(const QImage& image, int column)
    {
        int iCount = 0;
        for (int iY = 0; iY < image.height(); ++iY) iCount += Px(image, column, iY) == 255 ? 1 : 0;
        return iCount;
    }

    // 가운데 열에서 255 인 행 수 (가로선 두께)
    static int HorizontalThickness(const QImage& image)
    {
        int iCount = 0;
        for (int iY = 0; iY < image.height(); ++iY) iCount += RowCoverage(image, iY) == image.width() ? 1 : 0;
        return iCount;
    }

    static int Painted(const QImage& image)
    {
        int iCount = 0;
        for (int iY = 0; iY < image.height(); ++iY)
        {
            for (int iX = 0; iX < image.width(); ++iX) iCount += Px(image, iX, iY) > 0 ? 1 : 0;
        }
        return iCount;
    }

    static double Mean(const QImage& image)
    {
        double dSum = 0;
        for (int iY = 0; iY < image.height(); ++iY)
        {
            for (int iX = 0; iX < image.width(); ++iX) dSum += Px(image, iX, iY);
        }
        return dSum / 255.0 / (image.width() * image.height());
    }

    // 중심 가로선이 지나는 행(첫 번째)
    static int CenterRow(const QImage& image) { return image.height() / 2; }

    // x 방향 연속 구간 수 (255 인 칸)
    static int RunsInRow(const QImage& image, int row)
    {
        int iRuns = 0;
        bool bIn = false;
        for (int iX = 0; iX < image.width(); ++iX)
        {
            const bool bOn = Px(image, iX, row) == 255;
            if (bOn && !bIn) ++iRuns;
            bIn = bOn;
        }
        return iRuns;
    }

    static int LongestRun(const QImage& image, int row)
    {
        int iBest = 0;
        int iCurrent = 0;
        for (int iX = 0; iX < image.width(); ++iX)
        {
            iCurrent = Px(image, iX, row) == 255 ? iCurrent + 1 : 0;
            iBest = std::max(iBest, iCurrent);
        }
        return iBest;
    }

    void VerifyAtScale(qreal ratio)
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), ratio);
        const int iW = metrics.iCellWidth;
        const int iH = metrics.iCellHeight;
        const int iT = metrics.iLineThickness;

        // Handles 경계, 크기
        QVERIFY(!BoxGlyphPainter::Handles(0x24FF));
        QVERIFY(BoxGlyphPainter::Handles(0x2500));
        QVERIFY(BoxGlyphPainter::Handles(0x259F));
        QVERIFY(!BoxGlyphPainter::Handles(0x25A0));
        for (char32_t c = 0x2500; c <= 0x259F; ++c)
        {
            const QImage image = BoxGlyphPainter::Paint(c, metrics);
            QCOMPARE(image.format(), QImage::Format_Alpha8);
            QCOMPARE(image.size(), QSize(iW, iH));
            QVERIFY2(Painted(image) > 0, qPrintable(QStringLiteral("empty U+%1").arg(uint(c), 4, 16)));
        }

        // ─ : 중심 행 전체, 맨 위·아래 행 0, 두께 iT, 위치 iH/2 - iT/2
        const QImage light = BoxGlyphPainter::Paint(0x2500, metrics);
        QCOMPARE(HorizontalThickness(light), iT);
        QCOMPARE(RowCoverage(light, iH / 2 - iT / 2), iW);
        QCOMPARE(RowCoverage(light, 0), 0);
        QCOMPARE(RowCoverage(light, iH - 1), 0);
        // │
        const QImage vertical = BoxGlyphPainter::Paint(0x2502, metrics);
        QCOMPARE(ColCoverage(vertical, iW / 2 - iT / 2), iH);
        QCOMPARE(ColCoverage(vertical, 0), 0);
        // ━ = 2t
        QCOMPARE(HorizontalThickness(BoxGlyphPainter::Paint(0x2501, metrics)), 2 * iT);

        // ┼
        const QImage cross = BoxGlyphPainter::Paint(0x253C, metrics);
        QCOMPARE(RowCoverage(cross, iH / 2 - iT / 2), iW);
        QCOMPARE(ColCoverage(cross, iW / 2 - iT / 2), iH);
        QCOMPARE(Px(cross, 0, 0), 0);
        // ═ : 사이에 빈 행이 있는 두 선
        const QImage dbl = BoxGlyphPainter::Paint(0x2550, metrics);
        QCOMPARE(HorizontalThickness(dbl), 2 * iT);
        QCOMPARE(RowCoverage(dbl, iH / 2 - (3 * iT) / 2 + iT), 0);
        QCOMPARE(RowCoverage(dbl, iH / 2 - (3 * iT) / 2), iW);
        QCOMPARE(RowCoverage(dbl, iH / 2 - (3 * iT) / 2 + 2 * iT), iW);

        // 이음새: ─ 오른쪽 끝 열과 ┼ 왼쪽 끝 열이 같은 행에서 255
        for (int iY = 0; iY < iH; ++iY)
        {
            QCOMPARE(Px(light, iW - 1, iY), Px(cross, 0, iY));
            QCOMPARE(Px(light, 0, iY), Px(cross, iW - 1, iY));
        }
        // 가는/굵은 혼합 이음새: ┝ 오른쪽 끝은 ━ 와 같은 행, ┥ 왼쪽 끝은 ━ 와 같은 행
        const QImage heavy = BoxGlyphPainter::Paint(0x2501, metrics);
        const QImage mixedRight = BoxGlyphPainter::Paint(0x251D, metrics);
        const QImage mixedLeft = BoxGlyphPainter::Paint(0x2525, metrics);
        for (int iY = 0; iY < iH; ++iY)
        {
            QCOMPARE(Px(mixedRight, iW - 1, iY), Px(heavy, 0, iY));
            QCOMPARE(Px(mixedLeft, 0, iY), Px(heavy, 0, iY));
        }
        // 이중선 이음새: ╠ 오른쪽 끝은 ═ 와 같은 행
        const QImage doubleEdge = BoxGlyphPainter::Paint(0x2560, metrics);
        for (int iY = 0; iY < iH; ++iY) QCOMPARE(Px(doubleEdge, iW - 1, iY), Px(dbl, 0, iY));
        // 이중선 ╬ 의 모서리와 틈: 가운데 칸은 비고, 모서리는 칠해진다
        const QImage dcross = BoxGlyphPainter::Paint(0x256C, metrics);
        QCOMPARE(Px(dcross, iW / 2, iH / 2 - (3 * iT) / 2 + iT), 0);
        QCOMPARE(Px(dcross, iW / 2 - (3 * iT) / 2, iH / 2 - (3 * iT) / 2), 255);
        // ╔ 의 안쪽 모서리 아래·오른쪽은 틈
        const QImage dcorner = BoxGlyphPainter::Paint(0x2554, metrics);
        QCOMPARE(Px(dcorner, iW - 1, iH / 2 - (3 * iT) / 2 + iT), 0);
        QCOMPARE(Px(dcorner, iW - 1, iH / 2 - (3 * iT) / 2), 255);
        QCOMPARE(Px(dcorner, iW / 2 - (3 * iT) / 2 + iT, iH - 1), 0);
        // ╪ : 세로 가는 선이 이중 가로선의 틈을 가로지른다
        const QImage cross2 = BoxGlyphPainter::Paint(0x256A, metrics);
        QCOMPARE(Px(cross2, iW / 2, iH / 2 - (3 * iT) / 2 + iT), 255);
        QCOMPARE(Px(cross2, 0, iH / 2 - (3 * iT) / 2 + iT), 0);

        // ┄ : 조각 3개
        const QImage dashed = BoxGlyphPainter::Paint(0x2504, metrics);
        QCOMPARE(RunsInRow(dashed, CenterRow(dashed)), 3);
        QVERIFY(RowCoverage(dashed, CenterRow(dashed)) < iW);
        QCOMPARE(RunsInRow(BoxGlyphPainter::Paint(0x2508, metrics), CenterRow(dashed)), 4);
        QCOMPARE(RunsInRow(BoxGlyphPainter::Paint(0x254C, metrics), CenterRow(dashed)), 2);

        // ╭
        const QImage arc = BoxGlyphPainter::Paint(0x256D, metrics);
        QCOMPARE(Px(arc, iW - 1, iH / 2), 255);
        QCOMPARE(Px(arc, iW / 2, iH - 1), 255);
        QCOMPARE(Px(arc, 0, 0), 0);
        bool bPartial = false;
        for (int iY = 0; iY < iH; ++iY)
        {
            for (int iX = 0; iX < iW; ++iX)
            {
                const int iValue = Px(arc, iX, iY);
                bPartial = bPartial || (iValue >= 1 && iValue <= 254);
            }
        }
        QVERIFY(bPartial);

        // ╱
        const QImage slash = BoxGlyphPainter::Paint(0x2571, metrics);
        QVERIFY(Px(slash, iW - 1, 0) > 0 || Px(slash, iW - 2, 1) > 0);
        QVERIFY(Px(slash, 0, iH - 1) > 0 || Px(slash, 1, iH - 2) > 0);
        QCOMPARE(Px(slash, 0, 0), 0);

        // 블록
        const QImage full = BoxGlyphPainter::Paint(0x2588, metrics);
        QCOMPARE(Painted(full), iW * iH);
        QCOMPARE(Mean(full), 1.0);
        const QImage upper = BoxGlyphPainter::Paint(0x2580, metrics);
        const int iHalf = int(std::lround(iH / 2.0));
        QCOMPARE(RowCoverage(upper, iHalf - 1), iW);
        QCOMPARE(RowCoverage(upper, iHalf), 0);
        QCOMPARE(RowCoverage(upper, 0), iW);
        const QImage lowerEighth = BoxGlyphPainter::Paint(0x2581, metrics);
        const int iEighth = int(std::lround(iH / 8.0));
        for (int iY = 0; iY < iH; ++iY) QCOMPARE(RowCoverage(lowerEighth, iY), iY >= iH - iEighth ? iW : 0);
        QVERIFY(std::abs(Mean(BoxGlyphPainter::Paint(0x2591, metrics)) - 0.25) <= 0.02);
        QVERIFY(std::abs(Mean(BoxGlyphPainter::Paint(0x2592, metrics)) - 0.50) <= 0.02);
        QVERIFY(std::abs(Mean(BoxGlyphPainter::Paint(0x2593, metrics)) - 0.75) <= 0.02);
        // ▘ : 왼쪽 위 사분면만
        const QImage quadrant = BoxGlyphPainter::Paint(0x2598, metrics);
        const int iHalfW = int(std::lround(iW / 2.0));
        QCOMPARE(Px(quadrant, 0, 0), 255);
        QCOMPARE(Px(quadrant, iHalfW - 1, iHalf - 1), 255);
        QCOMPARE(Px(quadrant, iHalfW, 0), 0);
        QCOMPARE(Px(quadrant, 0, iHalf), 0);
        QCOMPARE(Px(quadrant, iW - 1, iH - 1), 0);
        // ▌ + ▐ = 전체
        const QImage left = BoxGlyphPainter::Paint(0x258C, metrics);
        const QImage right = BoxGlyphPainter::Paint(0x2590, metrics);
        for (int iX = 0; iX < iW; ++iX) QCOMPARE(Px(left, iX, 0) + Px(right, iX, 0), 255);

        // 밑줄 무늬
        const int iTop = BoxGlyphPainter::GetUnderlineSpriteTop(metrics);
        QCOMPARE(iTop, std::max(0, metrics.iUnderlineY - metrics.iUnderlineThickness));
        const int iSpriteH = std::min(3 * metrics.iUnderlineThickness, iH - iTop);
        for (EUnderlineSprite sprite : {EUnderlineSprite::CURLY, EUnderlineSprite::DOTTED, EUnderlineSprite::DASHED})
        {
            const QImage image = BoxGlyphPainter::PaintUnderline(sprite, metrics);
            QCOMPARE(image.format(), QImage::Format_Alpha8);
            QCOMPARE(image.width(), iW);
            QCOMPARE(image.height(), iSpriteH);
            QVERIFY(Painted(image) > 0);
        }
        const QImage curly = BoxGlyphPainter::PaintUnderline(EUnderlineSprite::CURLY, metrics);
        int iRowsWithInk = 0;
        for (int iY = 0; iY < curly.height(); ++iY) iRowsWithInk += RowHasInk(curly, iY) ? 1 : 0;
        QVERIFY(iRowsWithInk >= 2);
        const QImage dotted = BoxGlyphPainter::PaintUnderline(EUnderlineSprite::DOTTED, metrics);
        const QImage dashes = BoxGlyphPainter::PaintUnderline(EUnderlineSprite::DASHED, metrics);
        const int iLine = metrics.iUnderlineY - iTop;
        QVERIFY(RunsInRow(dotted, iLine) >= 2);
        QVERIFY(RowCoverage(dotted, iLine) < iW);
        QVERIFY(RunsInRow(dashes, iLine) >= 1);
        QVERIFY(LongestRun(dashes, iLine) > LongestRun(dotted, iLine));
    }

private slots:
    void initTestCase() { TestFonts::Register(); }

    void paintsAtScale1() { VerifyAtScale(1.0); }
    void paintsAtScale2() { VerifyAtScale(2.0); }

    void lineThicknessAtScale2()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 2.0);
        QCOMPARE(HorizontalThickness(BoxGlyphPainter::Paint(0x2500, metrics)), 2);
    }

    void outOfRangeIsNull()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        QVERIFY(BoxGlyphPainter::Paint(0x25A0, metrics).isNull());
    }
};

QObject* CreateBoxGlyphPainterTest() { return new BoxGlyphPainterTest(); }

#include "BoxGlyphPainterTest.moc"
