#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphRasterizer.h"

#include "TestFonts.h"

#include <QPainter>
#include <QTest>

class GlyphRasterizerTest : public QObject
{
    Q_OBJECT

private:
    static QFont BaseFont()
    {
        QFont font(QStringLiteral("Cascadia Mono"));
        font.setPixelSize(14);
        return font;
    }

    static qint64 AlphaSum(const QImage& image)
    {
        const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
        qint64 llSum = 0;
        for (int iY = 0; iY < argb.height(); ++iY)
            for (int iX = 0; iX < argb.width(); ++iX)
                llSum += qAlpha(argb.pixel(iX, iY));
        return llSum;
    }

    static void VerifyInBox(const RasterGlyph& glyph, const CellMetrics& metrics, int span)
    {
        QVERIFY(glyph.offset.x() >= 0 && glyph.offset.y() >= 0);
        QVERIFY(glyph.offset.x() + glyph.image.width() <= metrics.iCellWidth * span);
        QVERIFY(glyph.offset.y() + glyph.image.height() <= metrics.iCellHeight);
    }

    // 크기 변환 없이 큰 캔버스에 그린 실제 잉크(장치 픽셀). 기준선은 iBaseline.
    static QRect UnscaledInk(const QString& text, bool bold, bool italic, const CellMetrics& metrics)
    {
        QFont font = BaseFont();
        font.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Apple SD Gothic Neo"), QStringLiteral("Apple Color Emoji")});
        font.setStyleStrategy(QFont::NoSubpixelAntialias);
        font.setBold(bold);
        font.setItalic(italic);
        const qreal dRatio = metrics.dDevicePixelRatio;
        constexpr int MARGIN = 100;
        constexpr QSize CANVAS_SIZE(600, 300);
        QImage canvas(CANVAS_SIZE * dRatio, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        canvas.setDevicePixelRatio(dRatio);
        {
            QPainter painter(&canvas);
            painter.setFont(font);
            painter.setPen(Qt::black);
            painter.drawText(QPointF(MARGIN, MARGIN + metrics.iBaseline / dRatio), text);
        }
        QRect ink;
        for (int iY = 0; iY < canvas.height(); ++iY)
            for (int iX = 0; iX < canvas.width(); ++iX)
                if (qAlpha(canvas.pixel(iX, iY)) > 0)
                    ink = ink.united(QRect(iX, iY, 1, 1));
        return ink.translated(QPoint(-MARGIN, -MARGIN) * dRatio);
    }

    static QRect GlyphRect(const RasterGlyph& glyph)
    {
        return QRect(glyph.offset, glyph.image.size());
    }

private slots:
    void initTestCase() { TestFonts::Register(); }

    void latinMono()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        const GlyphRasterizer rasterizer(BaseFont(), metrics);
        const RasterGlyph glyph = rasterizer.Rasterize(U"A", false, false, 1);
        QVERIFY(!glyph.image.isNull());
        QVERIFY(!glyph.bColor);
        QCOMPARE(glyph.image.format(), QImage::Format_Alpha8);
        QVERIFY(AlphaSum(glyph.image) > 0);
        VerifyInBox(glyph, metrics, 1);
    }

    void hangulTwoCells()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        const GlyphRasterizer rasterizer(BaseFont(), metrics);
        const RasterGlyph glyph = rasterizer.Rasterize(U"한", false, false, 2);
        QVERIFY(!glyph.image.isNull());
        QVERIFY(glyph.image.width() > metrics.iCellWidth);
        VerifyInBox(glyph, metrics, 2);
    }

    void emojiColor()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        const GlyphRasterizer rasterizer(BaseFont(), metrics);
        const RasterGlyph glyph = rasterizer.Rasterize(U"\U0001F600", false, false, 2);
        if (glyph.image.isNull() || !glyph.bColor)
            QSKIP("no color emoji font");
        QCOMPARE(glyph.image.format(), QImage::Format_RGBA8888_Premultiplied);
        VerifyInBox(glyph, metrics, 2);
    }

    void zwjFamilyIsOneGlyph()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        const GlyphRasterizer rasterizer(BaseFont(), metrics);
        const RasterGlyph glyph = rasterizer.Rasterize(U"\U0001F468‍\U0001F469‍\U0001F467", false, false, 2);
        if (glyph.image.isNull() || !glyph.bColor)
            QSKIP("no color emoji font");
        VerifyInBox(glyph, metrics, 2);
    }

    void accentedLetterInCell()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        const GlyphRasterizer rasterizer(BaseFont(), metrics);
        const RasterGlyph combining = rasterizer.Rasterize(U"e\u0301", false, false, 1);
        QVERIFY(!combining.image.isNull());
        VerifyInBox(combining, metrics, 1);
        const RasterGlyph precomposed = rasterizer.Rasterize(U"\u00E9", false, false, 1);
        QVERIFY(!precomposed.image.isNull());
        VerifyInBox(precomposed, metrics, 1);
    }

    void inkBottomOnBaseline()
    {
        for (qreal dRatio : {1.0, 2.0})
        {
            const CellMetrics metrics = CellMetrics::Compute(BaseFont(), dRatio);
            const GlyphRasterizer rasterizer(BaseFont(), metrics);
            const RasterGlyph glyph = rasterizer.Rasterize(U"A", false, false, 1);
            QVERIFY(std::abs(glyph.offset.y() + glyph.image.height() - metrics.iBaseline) <= 1);
        }
    }

    void hangulNotOverShrunk()
    {
        for (qreal dRatio : {1.0, 2.0})
        {
            const CellMetrics metrics = CellMetrics::Compute(BaseFont(), dRatio);
            const GlyphRasterizer rasterizer(BaseFont(), metrics);
            const QRect unscaled = UnscaledInk(QString::fromUcs4(U"한"), false, false, metrics);
            const RasterGlyph glyph = rasterizer.Rasterize(U"한", false, false, 2);
            QVERIFY(!glyph.image.isNull());
            if (unscaled.top() >= 0 && unscaled.bottom() < metrics.iCellHeight &&
                unscaled.left() >= 0 && unscaled.right() < metrics.iCellWidth * 2)
                QVERIFY(std::abs(glyph.image.height() - unscaled.height()) <= 1);
            else
                QVERIFY(glyph.image.height() <= metrics.iCellHeight);
        }
    }

    void italicOverhangNotClipped()
    {
        for (qreal dRatio : {1.0, 2.0})
        {
            const CellMetrics metrics = CellMetrics::Compute(BaseFont(), dRatio);
            const GlyphRasterizer rasterizer(BaseFont(), metrics);
            for (const char32_t* text : {U"f", U"W"})
            {
                const QRect unscaled = UnscaledInk(QString::fromUcs4(text), true, true, metrics);
                const RasterGlyph glyph = rasterizer.Rasterize(text, true, true, 1);
                QVERIFY(!glyph.image.isNull());
                if (unscaled.width() <= metrics.iCellWidth && unscaled.height() <= metrics.iCellHeight)
                    QVERIFY(std::abs(glyph.image.width() - unscaled.width()) <= 1);
                else
                    QVERIFY(glyph.image.width() <= metrics.iCellWidth && glyph.image.height() <= metrics.iCellHeight);
                VerifyInBox(glyph, metrics, 1);
            }
        }
    }

    void boldHeavierThanRegular()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        const GlyphRasterizer rasterizer(BaseFont(), metrics);
        const RasterGlyph regular = rasterizer.Rasterize(U"A", false, false, 1);
        const RasterGlyph bold = rasterizer.Rasterize(U"A", true, false, 1);
        QVERIFY(AlphaSum(bold.image) > AlphaSum(regular.image));
    }

    void italicDiffers()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        const GlyphRasterizer rasterizer(BaseFont(), metrics);
        const RasterGlyph regular = rasterizer.Rasterize(U"l", false, false, 1);
        const RasterGlyph italic = rasterizer.Rasterize(U"l", false, true, 1);
        QVERIFY(regular.image != italic.image || regular.offset != italic.offset);
    }

    void spaceIsNull()
    {
        const CellMetrics metrics = CellMetrics::Compute(BaseFont(), 1.0);
        const GlyphRasterizer rasterizer(BaseFont(), metrics);
        QVERIFY(rasterizer.Rasterize(U" ", false, false, 1).image.isNull());
    }

    void fitScale()
    {
        QVERIFY(qFuzzyCompare(GlyphRasterizer::FitScale(QSizeF(30, 10), QSizeF(20, 20)), qreal(2.0 / 3.0)));
        QVERIFY(qFuzzyCompare(GlyphRasterizer::FitScale(QSizeF(10, 10), QSizeF(20, 20)), qreal(1)));
        QVERIFY(qFuzzyCompare(GlyphRasterizer::FitScale(QSizeF(10, 40), QSizeF(20, 20)), qreal(0.5)));
    }
};

QObject* CreateGlyphRasterizerTest() { return new GlyphRasterizerTest(); }

#include "GlyphRasterizerTest.moc"
