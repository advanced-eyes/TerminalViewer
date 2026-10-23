#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"

#include "TestFonts.h"

#include <QTest>

#include <cstdlib>

class GlyphCacheTest : public QObject
{
    Q_OBJECT

private:
    static QFont BaseFont()
    {
        QFont font(QStringLiteral("Cascadia Mono"));
        font.setPixelSize(14);
        return font;
    }

private slots:
    void initTestCase() { TestFonts::Register(); }

    void sameKeyHitsCache()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const GlyphEntry first = cache.GetGlyph(U"A", false, false, 1);
        const GlyphEntry second = cache.GetGlyph(U"A", false, false, 1);
        QVERIFY(!first.bEmpty);
        QCOMPARE(first.atlasRect, second.atlasRect);
        QCOMPARE(cache.GetRasterizeCount(), 1);
    }

    void boxGlyphUsesPainter()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const GlyphEntry entry = cache.GetGlyph(U"─", false, false, 1);
        QCOMPARE(entry.ePage, EAtlasPage::MONO);
        QCOMPARE(entry.atlasRect.width(), cache.GetMetrics().iCellWidth);
        QCOMPARE(entry.atlasRect.height(), cache.GetMetrics().iCellHeight);
        QCOMPARE(entry.offset, QPoint(0, 0));
        const GlyphEntry bold = cache.GetGlyph(U"─", true, true, 1);
        QCOMPARE(bold.atlasRect, entry.atlasRect);
    }

    void emojiGoesToColorPage()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const GlyphEntry entry = cache.GetGlyph(U"\U0001F600", false, false, 2);
        if (entry.bEmpty || entry.ePage != EAtlasPage::COLOR)
            QSKIP("no color emoji font");
        QCOMPARE(entry.ePage, EAtlasPage::COLOR);
    }

    void spaceIsEmpty()
    {
        GlyphCache cache(BaseFont(), 1.0);
        QVERIFY(cache.GetGlyph(U" ", false, false, 1).bEmpty);
    }

    void atlasFullResetsAndRepopulates()
    {
        GlyphCache cache(BaseFont(), 1.0, 64, 64);
        const GlyphEntry color = cache.GetGlyph(U"\U0001F600", false, false, 2);
        const GlyphEntry sprite = cache.GetUnderlineSprite(EUnderlineSprite::DOTTED);
        QVERIFY(!sprite.bEmpty);
        const uint32_t uiColorGeneration = cache.GetAtlas(EAtlasPage::COLOR).GetGeneration();
        const uint32_t uiGeneration0 = cache.GetGeneration();
        const std::u32string keys = U"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        for (char32_t ch : keys)
            cache.GetGlyph(std::u32string(1, ch), false, false, 1);
        QVERIFY(cache.GetGeneration() > uiGeneration0);
        QVERIFY(cache.GetAtlas(EAtlasPage::COLOR).GetGeneration() > uiColorGeneration);

        const GlyphEntry last = cache.GetGlyph(U"%", false, false, 1);
        QVERIFY(!last.bEmpty);
        const QImage& atlas = cache.GetAtlas(EAtlasPage::MONO).GetImage();
        bool bInk = false;
        for (int iY = last.atlasRect.top(); iY <= last.atlasRect.bottom(); ++iY)
            for (int iX = last.atlasRect.left(); iX <= last.atlasRect.right(); ++iX)
                bInk = bInk || qAlpha(atlas.pixel(iX, iY)) > 0;
        QVERIFY(bInk);

        // 비워진 뒤에는 컬러 글리프와 밑줄 무늬도 다시 만들어 현재 아틀라스 안에 넣는다.
        int iBefore = cache.GetRasterizeCount();
        const GlyphEntry colorAgain = cache.GetGlyph(U"\U0001F600", false, false, 2);
        if (!color.bEmpty)
        {
            QCOMPARE(cache.GetRasterizeCount(), iBefore + 1);
            QVERIFY(!colorAgain.bEmpty);
            QVERIFY(QRect(QPoint(0, 0), cache.GetAtlas(colorAgain.ePage).GetImage().size()).contains(colorAgain.atlasRect));
        }
        iBefore = cache.GetRasterizeCount();
        const GlyphEntry spriteAgain = cache.GetUnderlineSprite(EUnderlineSprite::DOTTED);
        QVERIFY(!spriteAgain.bEmpty);
        QVERIFY(QRect(QPoint(0, 0), cache.GetAtlas(spriteAgain.ePage).GetImage().size()).contains(spriteAgain.atlasRect));
        QVERIFY(cache.GetAtlas(spriteAgain.ePage).GetGeneration() > 0);

        iBefore = cache.GetRasterizeCount();
        cache.GetGlyph(U"A", false, false, 1);
        QCOMPARE(cache.GetRasterizeCount(), iBefore + 1);
    }

    void oversizedImageDoesNotFlushCache()
    {
        GlyphCache cache(BaseFont(), 1.0, 8, 8);   // 한 글자도 8x8 에 들어가지 않는다
        const uint32_t uiGeneration = cache.GetGeneration();
        const GlyphEntry entry = cache.GetGlyph(U"A", false, false, 1);
        QVERIFY(entry.bEmpty);
        QCOMPARE(cache.GetGeneration(), uiGeneration);
        QCOMPARE(cache.GetAtlas(EAtlasPage::MONO).GetGeneration(), uint32_t(0));
    }

    void underlineSpritesCached()
    {
        GlyphCache cache(BaseFont(), 1.0);
        for (EUnderlineSprite sprite : {EUnderlineSprite::CURLY, EUnderlineSprite::DOTTED, EUnderlineSprite::DASHED})
        {
            const GlyphEntry first = cache.GetUnderlineSprite(sprite);
            const GlyphEntry second = cache.GetUnderlineSprite(sprite);
            QVERIFY(!first.bEmpty);
            QCOMPARE(first.atlasRect, second.atlasRect);
        }
    }

    // 배율 2 는 장치 픽셀로 그린다: 글꼴을 두 배 크기로 배율 1 에서 그린 글리프와 크기가 같아야 한다.
    // (배율 1 글리프의 두 배와 비교하면 배율 1 의 안티에일리어싱 가장자리 줄이 두 배로 세어져 글꼴 엔진마다 2px 까지 어긋난다.)
    void devicePixelRatioScalesImage()
    {
        QFont doubleFont = BaseFont();
        doubleFont.setPixelSize(2 * BaseFont().pixelSize());
        GlyphCache two(BaseFont(), 2.0);
        GlyphCache doubleSize(doubleFont, 1.0);
        const GlyphEntry b = two.GetGlyph(U"A", false, false, 1);
        const GlyphEntry expected = doubleSize.GetGlyph(U"A", false, false, 1);
        QVERIFY2(std::abs(b.atlasRect.height() - expected.atlasRect.height()) <= 1,
                 qPrintable(QStringLiteral("x2 %1, double font %2").arg(b.atlasRect.height()).arg(expected.atlasRect.height())));
        QVERIFY2(std::abs(b.atlasRect.width() - expected.atlasRect.width()) <= 1,
                 qPrintable(QStringLiteral("x2 %1, double font %2").arg(b.atlasRect.width()).arg(expected.atlasRect.width())));
    }
};

QObject* CreateGlyphCacheTest() { return new GlyphCacheTest(); }

#include "GlyphCacheTest.moc"
