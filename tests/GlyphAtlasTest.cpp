#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphAtlas.h"

#include <QTest>

#include <memory>
#include <set>

class GlyphAtlasTest : public QObject
{
    Q_OBJECT

private:
    static QImage MakeA8(int width, int height, uchar value)
    {
        QImage image(width, height, QImage::Format_Alpha8);
        image.fill(value);
        return image;
    }

    static QImage MakeRgba(int width, int height, QRgb color)
    {
        QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor::fromRgba(color));
        return image;
    }

    static bool Covers(const std::vector<QRect>& vecRects, const QRect& target)
    {
        for (int iY = target.top(); iY <= target.bottom(); ++iY)
            for (int iX = target.left(); iX <= target.right(); ++iX)
            {
                bool bHit = false;
                for (const QRect& rc : vecRects)
                    bHit = bHit || rc.contains(iX, iY);
                if (!bHit)
                    return false;
            }
        return true;
    }

private slots:
    void shelfPlacementAndPixelsA8()
    {
        GlyphAtlas atlas(EAtlasFormat::A8, 64, 128);
        QPoint a, b;
        QVERIFY(atlas.Insert(MakeA8(10, 10, 100), a));
        QVERIFY(atlas.Insert(MakeA8(10, 10, 200), b));
        QCOMPARE(a, QPoint(0, 0));
        QCOMPARE(b, QPoint(11, 0));
        QCOMPARE(atlas.GetImage().format(), QImage::Format_Alpha8);
        QCOMPARE(int(atlas.GetImage().constScanLine(5)[3]), 100);
        QCOMPARE(int(atlas.GetImage().constScanLine(9)[11 + 9]), 200);
        QCOMPARE(int(atlas.GetImage().constScanLine(5)[10]), 0);

        const std::vector<QRect> vecDirty = atlas.TakeDirtyRects();
        QVERIFY(Covers(vecDirty, QRect(0, 0, 10, 10)));
        QVERIFY(Covers(vecDirty, QRect(11, 0, 10, 10)));
        QVERIFY(atlas.TakeDirtyRects().empty());
    }

    void pixelsRgba()
    {
        GlyphAtlas atlas(EAtlasFormat::RGBA8, 64, 128);
        QPoint a, b;
        QVERIFY(atlas.Insert(MakeRgba(4, 4, qRgba(10, 20, 30, 255)), a));
        QVERIFY(atlas.Insert(MakeRgba(4, 4, qRgba(1, 2, 3, 255)), b));
        QCOMPARE(b, QPoint(5, 0));
        QCOMPARE(atlas.GetImage().format(), QImage::Format_RGBA8888_Premultiplied);
        QCOMPARE(atlas.GetImage().pixel(2, 2), qRgba(10, 20, 30, 255));
        QCOMPARE(atlas.GetImage().pixel(6, 2), qRgba(1, 2, 3, 255));
    }

    void wrapsToNextShelf()
    {
        GlyphAtlas atlas(EAtlasFormat::A8, 64, 128);
        QPoint p;
        QVERIFY(atlas.Insert(MakeA8(30, 10, 1), p));
        QVERIFY(atlas.Insert(MakeA8(30, 12, 1), p));
        QCOMPARE(p, QPoint(31, 0));
        QVERIFY(atlas.Insert(MakeA8(10, 5, 1), p));
        QCOMPARE(p, QPoint(0, 13));
        QVERIFY(atlas.Insert(MakeA8(10, 5, 1), p));
        QCOMPARE(p, QPoint(11, 13));
    }

    void growsKeepingCoordinates()
    {
        GlyphAtlas atlas(EAtlasFormat::A8, 64, 128);
        QPoint first, p;
        QVERIFY(atlas.Insert(MakeA8(60, 60, 77), first));
        atlas.TakeDirtyRects();
        QVERIFY(atlas.Insert(MakeA8(60, 60, 88), p));
        QCOMPARE(atlas.GetSize(), 128);
        QCOMPARE(atlas.GetImage().width(), 128);
        QCOMPARE(atlas.GetImage().height(), 128);
        QCOMPARE(int(atlas.GetImage().constScanLine(first.y() + 30)[first.x() + 30]), 77);
        QCOMPARE(int(atlas.GetImage().constScanLine(p.y() + 30)[p.x() + 30]), 88);
        QCOMPARE(atlas.GetGeneration(), 0u);
        QVERIFY(atlas.WasResizedSinceLastTake());
        const std::vector<QRect> vecDirty = atlas.TakeDirtyRects();
        QCOMPARE(int(vecDirty.size()), 1);
        QCOMPARE(vecDirty[0], QRect(0, 0, 128, 128));
        QVERIFY(!atlas.WasResizedSinceLastTake());
    }

    void failsWhenFull()
    {
        GlyphAtlas atlas(EAtlasFormat::A8, 64, 128);
        QPoint p;
        QVERIFY(atlas.Insert(MakeA8(60, 60, 1), p));
        QVERIFY(atlas.Insert(MakeA8(60, 60, 1), p));
        QVERIFY(atlas.Insert(MakeA8(60, 60, 1), p));
        QVERIFY(atlas.Insert(MakeA8(60, 60, 1), p));
        QVERIFY(!atlas.Insert(MakeA8(60, 60, 1), p));
        QCOMPARE(atlas.GetSize(), 128);
    }

    void resetKeepsSizeAndBumpsGeneration()
    {
        GlyphAtlas atlas(EAtlasFormat::A8, 64, 128);
        QPoint p;
        QVERIFY(atlas.Insert(MakeA8(60, 60, 9), p));
        QVERIFY(atlas.Insert(MakeA8(60, 60, 9), p));
        atlas.TakeDirtyRects();
        atlas.Reset();
        QCOMPARE(atlas.GetGeneration(), 1u);
        QCOMPARE(atlas.GetSize(), 128);
        QVERIFY(!atlas.WasResizedSinceLastTake());
        QCOMPARE(int(atlas.GetImage().constScanLine(10)[10]), 0);
        QVERIFY(atlas.Insert(MakeA8(5, 5, 3), p));
        QCOMPARE(p, QPoint(0, 0));
        const std::vector<QRect> vecDirty = atlas.TakeDirtyRects();
        QCOMPARE(int(vecDirty.size()), 1);
        QCOMPARE(vecDirty[0], QRect(0, 0, 128, 128));
    }

    void tooLargeImageFailsWithoutLooping()
    {
        GlyphAtlas atlas(EAtlasFormat::A8, 64, 128);
        QPoint p;
        QVERIFY(!atlas.Insert(MakeA8(129, 4, 1), p));
        QVERIFY(!atlas.Insert(MakeA8(4, 129, 1), p));
        QCOMPARE(atlas.GetGeneration(), 0u);
        QVERIFY(!atlas.Insert(QImage(), p));
    }

    // GPU 텍스처가 "다른 아틀라스"를 알아보는 번호: 객체마다 다르고(지운 뒤 새로 만들어도 다시 쓰지 않는다),
    // 넣기·키우기·비우기에는 바뀌지 않는다
    void instanceIdIsUniqueAndStable()
    {
        std::unique_ptr<GlyphAtlas> upFirst = std::make_unique<GlyphAtlas>(EAtlasFormat::A8, 64, 128);
        const GlyphAtlas second(EAtlasFormat::RGBA8, 64, 128);
        const uint64_t uFirst = upFirst->GetInstanceId();
        QVERIFY(uFirst != 0);
        QVERIFY(second.GetInstanceId() != 0);
        QVERIFY(second.GetInstanceId() != uFirst);

        QPoint p;
        QVERIFY(upFirst->Insert(MakeA8(60, 60, 1), p));
        QVERIFY(upFirst->Insert(MakeA8(60, 60, 1), p));
        QCOMPARE(upFirst->GetSize(), 128);
        upFirst->TakeDirtyRects();
        upFirst->Reset();
        QCOMPARE(upFirst->GetInstanceId(), uFirst);

        upFirst.reset();
        std::set<uint64_t> setSeen{uFirst, second.GetInstanceId()};
        for (int iIndex = 0; iIndex < 8; ++iIndex)
        {
            const std::unique_ptr<GlyphAtlas> upNext = std::make_unique<GlyphAtlas>(EAtlasFormat::A8, 64, 128);
            QVERIFY(upNext->GetInstanceId() != 0);
            QVERIFY(setSeen.insert(upNext->GetInstanceId()).second);
        }
    }

    void convertsArgbToRgbaByteOrder()
    {
        GlyphAtlas atlas(EAtlasFormat::RGBA8, 64, 128);
        QImage src(2, 2, QImage::Format_ARGB32_Premultiplied);
        src.fill(qRgba(10, 20, 30, 255));
        QPoint p;
        QVERIFY(atlas.Insert(src, p));
        const uchar* pPixel = atlas.GetImage().constScanLine(p.y()) + p.x() * 4;
        QCOMPARE(int(pPixel[0]), 10);
        QCOMPARE(int(pPixel[1]), 20);
        QCOMPARE(int(pPixel[2]), 30);
        QCOMPARE(int(pPixel[3]), 255);
    }
};

QObject* CreateGlyphAtlasTest() { return new GlyphAtlasTest(); }

#include "GlyphAtlasTest.moc"
