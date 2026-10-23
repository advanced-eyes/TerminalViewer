#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"

#include "TestFonts.h"

#include <QFontMetricsF>
#include <QTest>
#include <algorithm>
#include <cmath>

class CellMetricsTest : public QObject
{
    Q_OBJECT

private:
    static QFont BaseFont()
    {
        QFont font(QStringLiteral("Cascadia Mono"));
        font.setPixelSize(14);
        return font;
    }

    static void VerifyAgainstFormula(qreal ratio)
    {
        const QFont font = BaseFont();
        const QFontMetricsF fm(font);
        const CellMetrics metrics = CellMetrics::Compute(font, ratio);
        const int iWidth = int(std::ceil(fm.horizontalAdvance(QLatin1Char('M')) * ratio));
        const int iHeight = int(std::ceil((fm.ascent() + fm.descent()) * ratio));
        const int iBaseline = int(std::ceil(fm.ascent() * ratio));
        const int iThick = std::max(1, int(std::lround(fm.lineWidth() * ratio)));
        const int iUnderlineY = std::min(iBaseline + std::max(1, int(std::lround(fm.underlinePos() * ratio))), iHeight - iThick);
        int iStrikeY = iBaseline - int(std::lround(fm.strikeOutPos() * ratio)) - iThick / 2;
        iStrikeY = std::clamp(iStrikeY, 0, iHeight - iThick);
        QCOMPARE(metrics.iCellWidth, iWidth);
        QCOMPARE(metrics.iCellHeight, iHeight);
        QCOMPARE(metrics.iBaseline, iBaseline);
        QCOMPARE(metrics.iUnderlineThickness, iThick);
        QCOMPARE(metrics.iUnderlineY, iUnderlineY);
        QCOMPARE(metrics.iStrikeThickness, iThick);
        QCOMPARE(metrics.iStrikeY, iStrikeY);
        QCOMPARE(metrics.dDevicePixelRatio, ratio);
        QVERIFY(metrics.iCellWidth > 0 && metrics.iCellHeight > 0);
        QVERIFY(metrics.iUnderlineY + metrics.iUnderlineThickness <= metrics.iCellHeight);
        QVERIFY(metrics.iStrikeY >= 0 && metrics.iStrikeY + metrics.iStrikeThickness <= metrics.iCellHeight);
    }

private slots:
    void initTestCase() { TestFonts::Register(); }

    void matchesFormulaAtScale1() { VerifyAgainstFormula(1.0); }
    void matchesFormulaAtScale2() { VerifyAgainstFormula(2.0); }

    void lineThicknessFollowsScale()
    {
        const QFont font = BaseFont();
        QCOMPARE(CellMetrics::Compute(font, 1.0).iLineThickness, 1);
        QCOMPARE(CellMetrics::Compute(font, 1.5).iLineThickness, 2);
        QCOMPARE(CellMetrics::Compute(font, 2.0).iLineThickness, 2);
    }

    void computeGrid()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        QCOMPARE(metrics.ComputeGrid(QSize(metrics.iCellWidth * 80 + 8, metrics.iCellHeight * 24 + 8), 4), QSize(80, 24));
        QCOMPARE(metrics.ComputeGrid(QSize(0, 0), 4), QSize(1, 1));
        QCOMPARE(metrics.ComputeGrid(QSize(metrics.iCellWidth / 2, metrics.iCellHeight / 2), 0), QSize(1, 1));
    }
};

QObject* CreateCellMetricsTest() { return new CellMetricsTest(); }

#include "CellMetricsTest.moc"
