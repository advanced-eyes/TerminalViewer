#include "TerminalViewer/Algorithm/TerminalRenderer/Common/SoftwareCompositor.h"

#include <QTest>

class SoftwareCompositorTest : public QObject
{
    Q_OBJECT

private:
    static constexpr uint32_t kBlue = 0xFF0000FF;    // 0xAARRGGBB
    static constexpr uint32_t kRed = 0xFFFF0000;
    static constexpr uint32_t kGreen = 0xFF00FF00;

    GlyphAtlas m_mono{EAtlasFormat::A8, 64, 64};
    GlyphAtlas m_color{EAtlasFormat::RGBA8, 64, 64};

    static uint32_t Px(const QImage& img, int x, int y)
    {
        return reinterpret_cast<const uint32_t*>(img.constScanLine(y))[x];
    }

    static uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
    {
        return r | (g << 8) | (b << 16) | (a << 24);
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

    static DrawQuad MakeQuad(int x, int y, int w, int h, uint32_t color, EQuadKind kind = EQuadKind::RECT, int row = 0)
    {
        DrawQuad quad;
        quad.iX = int16_t(x);
        quad.iY = int16_t(y);
        quad.uW = uint16_t(w);
        quad.uH = uint16_t(h);
        quad.uColor = color;
        quad.eKind = kind;
        quad.iRow = int16_t(row);
        return quad;
    }

    static QImage MakeA8(int w, int h, uchar v)
    {
        QImage image(w, h, QImage::Format_Alpha8);
        image.fill(v);
        return image;
    }

    static uint32_t Mul255(uint32_t a, uint32_t b) { return (a * b + 127) / 255; }

private slots:
    void opaqueRect()
    {
        DrawList list = MakeList();
        list.vecQuads.push_back(MakeQuad(0, 0, 10, 10, Rgba(255, 0, 0, 255)));
        QImage target;
        const QRegion region = SoftwareCompositor::Compose(list, m_mono, m_color, target);
        QCOMPARE(target.size(), QSize(200, 100));
        QCOMPARE(target.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(Px(target, 5, 5), kRed);
        QCOMPARE(Px(target, 15, 5), kBlue);
        QCOMPARE(region.boundingRect(), QRect(0, 0, 200, 100));
    }

    void translucentRectBlend()
    {
        DrawList list = MakeList();
        // premultiplied 반투명 빨강: (128, 0, 0, 128)
        list.vecQuads.push_back(MakeQuad(0, 0, 10, 10, Rgba(128, 0, 0, 128)));
        QImage target;
        SoftwareCompositor::Compose(list, m_mono, m_color, target);
        const uint32_t sa = 128;
        const uint32_t inv = 255 - sa;
        const uint32_t r = 128 + (0 * inv + 127) / 255;
        const uint32_t b = 0 + (255 * inv + 127) / 255;
        const uint32_t a = sa + (255 * inv + 127) / 255;
        QCOMPARE(Px(target, 3, 3), (a << 24) | (r << 16) | b);
    }

    void monoGlyph()
    {
        QPoint full, half;
        QVERIFY(m_mono.Insert(MakeA8(4, 4, 255), full));
        QVERIFY(m_mono.Insert(MakeA8(4, 4, 128), half));
        DrawList list = MakeList();
        DrawQuad q1 = MakeQuad(0, 0, 4, 4, Rgba(255, 255, 255, 255), EQuadKind::GLYPH_MONO);
        q1.uAtlasX = uint16_t(full.x());
        q1.uAtlasY = uint16_t(full.y());
        DrawQuad q2 = MakeQuad(10, 0, 4, 4, Rgba(255, 255, 255, 255), EQuadKind::GLYPH_MONO);
        q2.uAtlasX = uint16_t(half.x());
        q2.uAtlasY = uint16_t(half.y());
        list.vecQuads = {q1, q2};
        QImage target;
        SoftwareCompositor::Compose(list, m_mono, m_color, target);
        QCOMPARE(Px(target, 1, 1), 0xFFFFFFFFu);

        const uint32_t s = Mul255(255, 128);   // 채널별 색 × 알파
        const uint32_t inv = 255 - s;
        const uint32_t r = s + (0 * inv + 127) / 255;
        const uint32_t b = s + (255 * inv + 127) / 255;
        const uint32_t a = s + (255 * inv + 127) / 255;
        QCOMPARE(Px(target, 11, 1), (a << 24) | (r << 16) | (r << 8) | b);
    }

    void colorGlyph()
    {
        QImage rgba(4, 4, QImage::Format_RGBA8888_Premultiplied);
        rgba.fill(QColor(0, 0, 0, 0));
        for (int y = 0; y < 4; ++y)
        {
            uchar* p = rgba.scanLine(y);
            for (int x = 0; x < 4; ++x)
            {
                p[x * 4 + 0] = 100;   // R
                p[x * 4 + 1] = 50;    // G
                p[x * 4 + 2] = 0;     // B
                p[x * 4 + 3] = 100;   // A
            }
        }
        QPoint pos;
        QVERIFY(m_color.Insert(rgba, pos));
        DrawList list = MakeList();
        DrawQuad quad = MakeQuad(0, 0, 4, 4, 0xFFFFFFFF, EQuadKind::GLYPH_COLOR);
        quad.uAtlasX = uint16_t(pos.x());
        quad.uAtlasY = uint16_t(pos.y());
        list.vecQuads.push_back(quad);
        QImage target;
        SoftwareCompositor::Compose(list, m_mono, m_color, target);
        const uint32_t inv = 255 - 100;
        const uint32_t r = 100 + (0 * inv + 127) / 255;
        const uint32_t g = 50 + (0 * inv + 127) / 255;
        const uint32_t b = 0 + (255 * inv + 127) / 255;
        const uint32_t a = 100 + (255 * inv + 127) / 255;
        QCOMPARE(Px(target, 2, 2), (a << 24) | (r << 16) | (g << 8) | b);
    }

    void positionFormula()
    {
        DrawList list = MakeList();
        list.iPaddingPx = 4;
        list.iScrollOffsetPx = 5;
        list.vecQuads.push_back(MakeQuad(2, 3, 1, 1, Rgba(255, 0, 0, 255), EQuadKind::RECT, 2));
        QImage target;
        SoftwareCompositor::Compose(list, m_mono, m_color, target);
        QCOMPARE(target.size(), QSize(208, 108));
        const int iY = 4 + 40 + 3 - 5;
        QCOMPARE(Px(target, 4 + 2, iY), kRed);
        QCOMPARE(Px(target, 4 + 2, iY - 1), kBlue);
        QCOMPARE(Px(target, 4 + 2, iY + 1), kBlue);
    }

    void offscreenQuadsAreClipped()
    {
        DrawList list = MakeList();
        list.vecQuads.push_back(MakeQuad(-5, -5, 10, 10, Rgba(255, 0, 0, 255)));
        list.vecQuads.push_back(MakeQuad(195, 95, 30, 30, Rgba(255, 0, 0, 255)));
        list.vecQuads.push_back(MakeQuad(-100, 0, 10, 10, Rgba(255, 0, 0, 255)));
        list.vecQuads.push_back(MakeQuad(0, 0, 4, 4, Rgba(255, 0, 0, 255), EQuadKind::GLYPH_MONO, -3));
        QImage target;
        SoftwareCompositor::Compose(list, m_mono, m_color, target);
        QCOMPARE(Px(target, 0, 0), kRed);
        QCOMPARE(Px(target, 4, 4), kRed);
        QCOMPARE(Px(target, 5, 5), kBlue);
        QCOMPARE(Px(target, 199, 99), kRed);
        QCOMPARE(Px(target, 194, 94), kBlue);
    }

    void partialComposeOnlyDirtyBand()
    {
        DrawList list = MakeList();
        QImage target(200, 100, QImage::Format_ARGB32_Premultiplied);
        target.fill(QColor(0, 255, 0));
        list.bFullRedraw = false;
        list.vecDirtyRows = {1};
        list.vecQuads.push_back(MakeQuad(0, 0, 10, 10, Rgba(255, 0, 0, 255), EQuadKind::RECT, 1));
        const QRegion region = SoftwareCompositor::Compose(list, m_mono, m_color, target);
        QCOMPARE(Px(target, 5, 5), kGreen);
        QCOMPARE(Px(target, 5, 25), kRed);
        QCOMPARE(Px(target, 50, 25), kBlue);
        QCOMPARE(Px(target, 5, 45), kGreen);
        QCOMPARE(region, QRegion(QRect(0, 20, 200, 20)));
    }

    void overflowFromOtherRowIsDrawnInBand()
    {
        DrawList list = MakeList();
        QImage target(200, 100, QImage::Format_ARGB32_Premultiplied);
        target.fill(QColor(0, 255, 0));
        list.bFullRedraw = false;
        list.vecDirtyRows = {1};
        // 행 0 의 쿼드가 아래로 넘쳐 y 15..24 (행 1 띠는 20..39)
        list.vecQuads.push_back(MakeQuad(0, 15, 10, 10, Rgba(255, 0, 0, 255), EQuadKind::RECT, 0));
        SoftwareCompositor::Compose(list, m_mono, m_color, target);
        QCOMPARE(Px(target, 5, 17), kGreen);   // 행 0 은 그대로
        QCOMPARE(Px(target, 5, 22), kRed);     // 넘친 부분이 그려진다
        QCOMPARE(Px(target, 5, 26), kBlue);
    }

    void differentSizeRecreatesAndComposesAll()
    {
        DrawList list = MakeList();
        list.bFullRedraw = false;
        list.vecDirtyRows = {1};
        list.vecQuads.push_back(MakeQuad(0, 0, 10, 10, Rgba(255, 0, 0, 255), EQuadKind::RECT, 3));
        QImage target(50, 50, QImage::Format_ARGB32_Premultiplied);
        target.fill(QColor(0, 255, 0));
        const QRegion region = SoftwareCompositor::Compose(list, m_mono, m_color, target, QSize(200, 100));
        QCOMPARE(target.size(), QSize(200, 100));
        QCOMPARE(region.boundingRect(), QRect(0, 0, 200, 100));
        QCOMPARE(Px(target, 5, 65), kRed);
        QCOMPARE(Px(target, 5, 5), kBlue);
    }

    void dirtyRowsOutsideTargetAreClipped()
    {
        DrawList list = MakeList();
        QImage target(200, 100, QImage::Format_ARGB32_Premultiplied);
        target.fill(QColor(0, 255, 0));
        list.bFullRedraw = false;
        list.vecDirtyRows = {-1, 5, 2};
        const QRegion region = SoftwareCompositor::Compose(list, m_mono, m_color, target);
        QCOMPARE(region, QRegion(QRect(0, 40, 200, 20)));
    }
};

QObject* CreateSoftwareCompositorTest() { return new SoftwareCompositorTest(); }

#include "SoftwareCompositorTest.moc"
