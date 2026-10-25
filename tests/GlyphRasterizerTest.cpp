#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphRasterizer.h"

#include "TestFonts.h"

#include <QFontInfo>
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
    // 글꼴 목록은 렌더러와 같다(BuildFontFamilies): 플랫폼마다 다른 Qt 대체 글꼴(Windows 는 Gulim · Batang 등)과 비교하지 않게 한다
    static QRect UnscaledInk(const QString& text, bool bold, bool italic, const CellMetrics& metrics)
    {
        QFont font = BaseFont();
        font.setFamilies(GlyphRasterizer::BuildFontFamilies(BaseFont()));
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

    // 글꼴을 그대로(대체 목록 없이) QPainter 로 그린 잉크를 잘라 낸 알파 그림. 기준선은 iBaseline, 왼쪽은 정수 위치.
    static QImage DrawDirectGlyph(const QString& text, const QFont& baseFont, const CellMetrics& metrics)
    {
        QFont font = baseFont;
        font.setStyleStrategy(QFont::NoSubpixelAntialias);
        const qreal dRatio = metrics.dDevicePixelRatio;
        constexpr int MARGIN = 100;
        constexpr QSize CANVAS_SIZE(300, 300);
        QImage canvas(CANVAS_SIZE * dRatio, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        canvas.setDevicePixelRatio(dRatio);
        {
            QPainter painter(&canvas);
            painter.setRenderHint(QPainter::TextAntialiasing, true);
            painter.setFont(font);
            painter.setPen(Qt::black);
            painter.drawText(QPointF(MARGIN, MARGIN + metrics.iBaseline / dRatio), text);
        }
        canvas.setDevicePixelRatio(1.0);
        QRect ink;
        for (int iY = 0; iY < canvas.height(); ++iY)
            for (int iX = 0; iX < canvas.width(); ++iX)
                if (qAlpha(canvas.pixel(iX, iY)) > 0)
                    ink = ink.united(QRect(iX, iY, 1, 1));
        const QImage cropped = canvas.copy(ink);
        QImage alpha(cropped.size(), QImage::Format_Alpha8);
        for (int iY = 0; iY < cropped.height(); ++iY)
            for (int iX = 0; iX < cropped.width(); ++iX)
                alpha.scanLine(iY)[iX] = uchar(qAlpha(cropped.pixel(iX, iY)));
        return alpha;
    }

    // Cascadia Mono 와 모양이 다른, 설치된 고정폭 글꼴(14px). 없으면 빈 이름
    static QFont FindOtherMonospaceFont()
    {
        for (const char* pFamily : {"Consolas", "Courier New", "Menlo", "Courier", "DejaVu Sans Mono", "Liberation Mono"})
        {
            QFont font(QString::fromLatin1(pFamily));
            font.setPixelSize(14);
            if (QFontInfo(font).family().compare(QString::fromLatin1(pFamily), Qt::CaseInsensitive) == 0)
                return font;
        }
        return QFont(QString());
    }

    // 컬러 판정용 합성 그림: 8x8 투명 바탕에 (2,2)~(5,5) 를 칠한다. pixelAt(x, y) 가 그 픽셀 값(premultiplied)
    template <typename Fn>
    static QImage MakeSyntheticGlyph(Fn pixelAt)
    {
        QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        for (int iY = 2; iY < 6; ++iY)
            for (int iX = 2; iX < 6; ++iX)
                image.setPixel(iX, iY, pixelAt(iX, iY));
        return image;
    }

    // 펜 색 pen 으로 그린 단색 글리프(가장자리는 반투명)
    static QImage MakeMonoGlyph(QRgb pen)
    {
        return MakeSyntheticGlyph([pen](int x, int y) {
            const int iAlpha = (x == 2 || y == 2) ? 96 : 255;
            return qPremultiply(qRgba(qRed(pen), qGreen(pen), qBlue(pen), iAlpha));
        });
    }

    // 펜 색을 따르지 않는 회색 · 흰색뿐인 컬러 글리프(흰 몸통 + 회색 테두리, 가장자리는 반투명)
    static QImage MakeGrayColorGlyph()
    {
        return MakeSyntheticGlyph([](int x, int y) {
            const bool bEdge = x == 2 || x == 5 || y == 2 || y == 5;
            return bEdge ? qPremultiply(qRgba(128, 128, 128, 200)) : qRgba(255, 255, 255, 255);
        });
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

    // 넘겨받은 글꼴로 그린다(칸 크기를 잰 글꼴과 같다): 그 글꼴로 직접 그린 것과 같고 Cascadia Mono 로 그린 것과 다르다
    void usesBaseFontFamily()
    {
        const QFont other = FindOtherMonospaceFont();
        if (other.family().isEmpty())
            QSKIP("no monospace font other than Cascadia Mono");
        for (qreal dRatio : {1.0, 2.0})
        {
            const CellMetrics metrics = CellMetrics::Compute(other, dRatio);
            const GlyphRasterizer rasterizer(other, metrics);
            for (const char32_t* pText : {U"M", U"g", U"0"})
            {
                const QString strText = QString::fromUcs4(pText);
                const QImage direct = DrawDirectGlyph(strText, other, metrics);
                const RasterGlyph glyph = rasterizer.Rasterize(pText, false, false, 1);
                QVERIFY(!glyph.image.isNull());
                VerifyInBox(glyph, metrics, 1);
                QVERIFY(direct != DrawDirectGlyph(strText, BaseFont(), metrics));
                QCOMPARE(glyph.image, direct);
            }
        }
    }

    void fontFamiliesList()
    {
#if defined(Q_OS_MAC)
        const QString strHangul = QStringLiteral("Apple SD Gothic Neo");
        const QString strEmoji = QStringLiteral("Apple Color Emoji");
#else
        const QString strHangul = QStringLiteral("Malgun Gothic");
        const QString strEmoji = QStringLiteral("Segoe UI Emoji");
#endif
        // 기본 글꼴(Cascadia Mono)은 고치기 전 목록 그대로
        QCOMPARE(GlyphRasterizer::BuildFontFamilies(BaseFont()), QStringList({QStringLiteral("Cascadia Mono"), strHangul, strEmoji}));

        // families() 가 앞, 그다음 family()(쉼표로 나누고 따옴표를 벗긴다), 겹치는 이름(대소문자 무시)은 한 번
        QFont font(QStringLiteral(" cascadia mono , \"TerminalViewer Extra\""));
        font.setPixelSize(14);
        font.setFamilies({QStringLiteral("Cascadia Mono"), strHangul});
        QCOMPARE(GlyphRasterizer::BuildFontFamilies(font),
                 QStringList({QStringLiteral("Cascadia Mono"), strHangul, QStringLiteral("TerminalViewer Extra"), strEmoji}));
    }

    // 없는 family 를 받으면 Qt 가 그 글꼴 대신 고른 글꼴(칸 크기를 잰 글꼴)로 그린다. 한글 대체 글꼴이 아니다
    void unknownFamilyUsesResolvedFont()
    {
        QFont unknown(QStringLiteral("TerminalViewer No Such Family"));
        unknown.setPixelSize(14);
        const QString strResolved = QFontInfo(unknown).family();
        if (strResolved.compare(QStringLiteral("Cascadia Mono"), Qt::CaseInsensitive) == 0)
            QSKIP("Qt resolves the unknown family to Cascadia Mono");
        const CellMetrics metrics = CellMetrics::Compute(unknown, 1.0);
        const GlyphRasterizer rasterizer(unknown, metrics);
        const RasterGlyph glyph = rasterizer.Rasterize(U"M", false, false, 1);
        QVERIFY(!glyph.image.isNull());
        QCOMPARE(glyph.image, DrawDirectGlyph(QStringLiteral("M"), unknown, metrics));
    }

    // 회색 · 흰색뿐이어도 검은 펜을 따르지 않은 잉크(rgb 가 0 이 아님)가 있으면 컬러(⚪ 🤍 🏳 같은 이모지)
    void grayColorGlyphIsColor()
    {
        QVERIFY(GlyphRasterizer::IsColorGlyph(MakeGrayColorGlyph()));
        // 흰색뿐
        QVERIFY(GlyphRasterizer::IsColorGlyph(MakeSyntheticGlyph([](int, int) { return qRgba(255, 255, 255, 255); })));
        // 아주 어두운 회색뿐(검은 이모지도 순수한 검정이 아니면)
        QVERIFY(GlyphRasterizer::IsColorGlyph(MakeSyntheticGlyph([](int, int) { return qRgba(33, 33, 33, 255); })));
        // 잉크 한 픽셀만 검정이 아니어도
        QImage almost = MakeMonoGlyph(qRgb(0, 0, 0));
        almost.setPixel(3, 3, qRgba(1, 1, 1, 255));
        QVERIFY(GlyphRasterizer::IsColorGlyph(almost));
        // premultiplied 가 아닌 포맷도 같은 판정
        QVERIFY(GlyphRasterizer::IsColorGlyph(MakeGrayColorGlyph().convertToFormat(QImage::Format_ARGB32)));
    }

    // 검은 펜을 따른 단색 글리프(알파만, rgb 0)는 단색
    void blackPenGlyphIsMono()
    {
        QVERIFY(!GlyphRasterizer::IsColorGlyph(MakeMonoGlyph(qRgb(0, 0, 0))));
        QVERIFY(!GlyphRasterizer::IsColorGlyph(MakeMonoGlyph(qRgb(0, 0, 0)).convertToFormat(QImage::Format_ARGB32)));
    }

    // 고치기 전 규칙도 그대로: 회색이 아닌 잉크가 하나라도 있으면 컬러. 완전 투명한 픽셀의 값은 보지 않는다
    void nonGrayInkIsColor()
    {
        QImage tinted = MakeMonoGlyph(qRgb(0, 0, 0));
        tinted.setPixel(4, 4, qRgba(200, 30, 30, 255));
        QVERIFY(GlyphRasterizer::IsColorGlyph(tinted));
        QImage hidden = MakeMonoGlyph(qRgb(0, 0, 0));
        hidden.setPixel(0, 0, qRgba(200, 30, 30, 0));
        QVERIFY(!GlyphRasterizer::IsColorGlyph(hidden));
    }

    void emptyIsMono()
    {
        QImage empty(8, 8, QImage::Format_ARGB32_Premultiplied);
        empty.fill(Qt::transparent);
        QVERIFY(!GlyphRasterizer::IsColorGlyph(empty));
        QVERIFY(!GlyphRasterizer::IsColorGlyph(QImage()));
    }

    // 실제 글꼴의 단색 글리프는 어떤 배율 · 굵기 · 줄임에서도 단색이다(검은 펜 잉크는 rgb 0 이다)
    void monochromeGlyphsStayMono()
    {
        for (qreal dRatio : {1.0, 1.25, 1.5, 2.0})
        {
            const CellMetrics metrics = CellMetrics::Compute(BaseFont(), dRatio);
            const GlyphRasterizer rasterizer(BaseFont(), metrics);
            for (const char32_t* pText : {U"A", U"g", U"@", U"é", U"█", U"☺"})
            {
                for (bool bBold : {false, true})
                {
                    const RasterGlyph glyph = rasterizer.Rasterize(pText, bBold, !bBold, 1);
                    QVERIFY(!glyph.image.isNull());
                    QVERIFY(!glyph.bColor);
                    QCOMPARE(glyph.image.format(), QImage::Format_Alpha8);
                }
            }
            // 1칸으로 줄여 그리는 한글(크기 변환이 있는 그리기)
            const RasterGlyph shrunk = rasterizer.Rasterize(U"한", false, false, 1);
            QVERIFY(!shrunk.image.isNull());
            QVERIFY(!shrunk.bColor);
        }
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
