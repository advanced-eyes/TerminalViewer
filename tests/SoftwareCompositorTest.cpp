#include "TerminalViewer/Algorithm/TerminalRenderer/Common/SoftwareCompositor.h"

#include <QTest>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

namespace
{
// 기준 합성(naive reference): 최적화하기 전 SoftwareCompositor::Compose 를 그대로 옮겼다. 행 띠마다 목록의 모든 쿼드를 띠로 잘라
// 그리고, 픽셀마다 /255 로 섞는다. 최적화한 Compose 는 이것과 비트 단위로 같아야 한다.
namespace Reference
{
constexpr uint32_t kChannelMax = 255;
constexpr uint32_t kRound = 127;

uint32_t Mul255(uint32_t a, uint32_t b)
{
    return (a * b + kRound) / kChannelMax;
}

uint32_t PackArgb(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32_t Blend(uint32_t dst, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb)
{
    const uint32_t uInverse = kChannelMax - sa;
    auto funcChannel = [&](uint32_t src, int shift) {
        const uint32_t uDst = (dst >> shift) & kChannelMax;
        return std::min(kChannelMax, src + (uDst * uInverse + kRound) / kChannelMax);
    };
    return PackArgb(funcChannel(sr, 16), funcChannel(sg, 8), funcChannel(sb, 0), funcChannel(sa, 24));
}

void DrawQuadInBand(const DrawQuad& quad, const DrawList& list, const GlyphAtlas& mono, const GlyphAtlas& color,
                    const QRect& band, QImage& target)
{
    const int iLeft = list.iPaddingPx + quad.iX;
    const int iTop = list.iPaddingPx + quad.iRow * list.iCellHeight + quad.iY - list.iScrollOffsetPx;
    const QRect rect = QRect(iLeft, iTop, quad.uW, quad.uH).intersected(band);
    if (rect.isEmpty())
        return;
    const uint32_t uQuadR = quad.uColor & kChannelMax;
    const uint32_t uQuadG = (quad.uColor >> 8) & kChannelMax;
    const uint32_t uQuadB = (quad.uColor >> 16) & kChannelMax;
    const uint32_t uQuadA = (quad.uColor >> 24) & kChannelMax;
    const QImage* pAtlas = nullptr;
    if (quad.eKind == EQuadKind::GLYPH_MONO)
        pAtlas = &mono.GetImage();
    else if (quad.eKind == EQuadKind::GLYPH_COLOR)
        pAtlas = &color.GetImage();
    for (int iY = rect.top(); iY <= rect.bottom(); ++iY)
    {
        uint32_t* pRow = reinterpret_cast<uint32_t*>(target.scanLine(iY));
        const int iAtlasY = quad.uAtlasY + (iY - iTop);
        for (int iX = rect.left(); iX <= rect.right(); ++iX)
        {
            const int iAtlasX = quad.uAtlasX + (iX - iLeft);
            uint32_t uSrcR = uQuadR;
            uint32_t uSrcG = uQuadG;
            uint32_t uSrcB = uQuadB;
            uint32_t uSrcA = uQuadA;
            if (pAtlas)
            {
                if (iAtlasX < 0 || iAtlasY < 0 || iAtlasX >= pAtlas->width() || iAtlasY >= pAtlas->height())
                    continue;
                if (quad.eKind == EQuadKind::GLYPH_MONO)
                {
                    const uint32_t uAlpha = pAtlas->constScanLine(iAtlasY)[iAtlasX];
                    uSrcR = Mul255(uQuadR, uAlpha);
                    uSrcG = Mul255(uQuadG, uAlpha);
                    uSrcB = Mul255(uQuadB, uAlpha);
                    uSrcA = Mul255(uQuadA, uAlpha);
                }
                else
                {
                    const uchar* pPixel = pAtlas->constScanLine(iAtlasY) + iAtlasX * 4;
                    uSrcR = pPixel[0];
                    uSrcG = pPixel[1];
                    uSrcB = pPixel[2];
                    uSrcA = pPixel[3];
                }
            }
            pRow[iX] = Blend(pRow[iX], uSrcA, uSrcR, uSrcG, uSrcB);
        }
    }
}

QRegion Compose(const DrawList& list, const GlyphAtlas& mono, const GlyphAtlas& color, QImage& target,
                const QSize& targetSize = QSize())
{
    bool bFull = list.bFullRedraw;
    QSize size = targetSize;
    if (!size.isValid())
    {
        size = target.isNull() ? QSize(list.iCols * list.iCellWidth + 2 * list.iPaddingPx,
                                       list.iRows * list.iCellHeight + 2 * list.iPaddingPx)
                               : target.size();
    }
    if (target.format() != QImage::Format_ARGB32_Premultiplied || target.size() != size)
    {
        target = QImage(size, QImage::Format_ARGB32_Premultiplied);
        bFull = true;
    }
    if (target.isNull())
        return QRegion();
    const QRect all(QPoint(0, 0), target.size());
    std::vector<QRect> vecBands;
    if (bFull)
        vecBands.push_back(all);
    else
    {
        std::vector<int> vecRows = list.vecDirtyRows;
        std::sort(vecRows.begin(), vecRows.end());
        vecRows.erase(std::unique(vecRows.begin(), vecRows.end()), vecRows.end());
        for (int iRow : vecRows)
        {
            const int iTop = list.iPaddingPx + iRow * list.iCellHeight - list.iScrollOffsetPx;
            const QRect band = QRect(0, iTop, all.width(), list.iCellHeight).intersected(all);
            if (!band.isEmpty())
                vecBands.push_back(band);
        }
    }
    const uint32_t uClear = PackArgb(list.clearColor.r, list.clearColor.g, list.clearColor.b, kChannelMax);
    QRegion region;
    for (const QRect& band : vecBands)
    {
        for (int iY = band.top(); iY <= band.bottom(); ++iY)
        {
            uint32_t* pRow = reinterpret_cast<uint32_t*>(target.scanLine(iY));
            std::fill(pRow + band.left(), pRow + band.right() + 1, uClear);
        }
        for (const DrawQuad& quad : list.vecQuads)
            DrawQuadInBand(quad, list, mono, color, band, target);
        region += band;
    }
    return region;
}
}   // namespace Reference

constexpr int kAtlasSize = 64;       // 무작위 아틀라스 이미지의 가로 · 세로
constexpr int kRandomSeeds = 600;    // matchesReferenceOnRandomLists 의 씨드 수(partialComposesStitchToFull 은 절반)
constexpr int kBytesPerPixel = 4;    // ARGB32_Premultiplied · RGBA8888_Premultiplied

// 무작위 그리기 목록 하나와 그것을 그릴 처음 이미지
struct RandomCase
{
    DrawList list;
    QImage imgStart;     // 부분 합성의 출발 이미지(쓰레기 픽셀). 비어 있으면 Compose 가 만든다
    QSize sizeTarget;    // Compose 의 targetSize(비어 있을 수 있다)
};

// 커버리지: 0 · 255 가 많고 그 사이 값도 섞는다
QImage MakeRandomMonoAtlasImage(std::mt19937& rng)
{
    QImage image(kAtlasSize, kAtlasSize, QImage::Format_Alpha8);
    for (int iY = 0; iY < kAtlasSize; ++iY)
    {
        uchar* pLine = image.scanLine(iY);
        for (int iX = 0; iX < kAtlasSize; ++iX)
        {
            const uint32_t uKind = rng() % 10;
            pLine[iX] = uKind < 4 ? 0 : uKind < 7 ? 255 : uchar(rng() % 256);
        }
    }
    return image;
}

// RGBA(premultiplied 바이트 그대로): 완전 투명 0, 불투명, 알파 0 인데 색이 있는 픽셀(더하기), 올바른 premultiplied, 틀린 premultiplied
QImage MakeRandomColorAtlasImage(std::mt19937& rng)
{
    QImage image(kAtlasSize, kAtlasSize, QImage::Format_RGBA8888_Premultiplied);
    for (int iY = 0; iY < kAtlasSize; ++iY)
    {
        uchar* pLine = image.scanLine(iY);
        for (int iX = 0; iX < kAtlasSize; ++iX)
        {
            uchar* pPixel = pLine + iX * kBytesPerPixel;
            const uint32_t uKind = rng() % 20;
            if (uKind < 7)
            {
                pPixel[0] = pPixel[1] = pPixel[2] = pPixel[3] = 0;
            }
            else if (uKind < 12)
            {
                pPixel[0] = uchar(rng());
                pPixel[1] = uchar(rng());
                pPixel[2] = uchar(rng());
                pPixel[3] = 255;
            }
            else if (uKind < 13)
            {
                pPixel[0] = uchar(rng());
                pPixel[1] = uchar(rng());
                pPixel[2] = uchar(rng());
                pPixel[3] = 0;
            }
            else if (uKind < 18)
            {
                const uint32_t uAlpha = rng() % 256;
                pPixel[0] = uchar(rng() % (uAlpha + 1));
                pPixel[1] = uchar(rng() % (uAlpha + 1));
                pPixel[2] = uchar(rng() % (uAlpha + 1));
                pPixel[3] = uchar(uAlpha);
            }
            else
            {
                pPixel[0] = uchar(rng());
                pPixel[1] = uchar(rng());
                pPixel[2] = uchar(rng());
                pPixel[3] = uchar(rng());
            }
        }
    }
    return image;
}

// premultiplied RGBA8(R | G<<8 | B<<16 | A<<24): 0, 불투명, 알파 0 인데 색, 올바른 premultiplied, 아무 값
uint32_t MakeRandomQuadColor(std::mt19937& rng)
{
    const uint32_t uKind = rng() % 10;
    if (uKind == 0)
        return 0;
    if (uKind < 5)
        return (rng() & 0x00FFFFFFu) | 0xFF000000u;
    if (uKind == 5)
        return rng() & 0x00FFFFFFu;
    if (uKind < 9)
    {
        const uint32_t uAlpha = rng() % 256;
        return (rng() % (uAlpha + 1)) | ((rng() % (uAlpha + 1)) << 8) | ((rng() % (uAlpha + 1)) << 16) | (uAlpha << 24);
    }
    return rng();
}

void FillGarbage(QImage& image, std::mt19937& rng)
{
    for (int iY = 0; iY < image.height(); ++iY)
    {
        uint32_t* pRow = reinterpret_cast<uint32_t*>(image.scanLine(iY));
        for (int iX = 0; iX < image.width(); ++iX)
            pRow[iX] = rng();
    }
}

int PickRandomIn(std::mt19937& rng, int low, int high)   // [low, high]
{
    return low + int(rng() % uint32_t(high - low + 1));
}

// 겹치는 쿼드, 행을 넘는 글리프(위·아래), 컬러·단색 글리프, 반투명·불투명 사각형, 아틀라스 밖으로 나가는 글리프,
// 화면 밖 쿼드, 스크롤 오프셋, 여백, 중복 · 정렬 안 된 · 범위 밖 바뀐 행
RandomCase MakeRandomCase(uint32_t seed)
{
    std::mt19937 rng(seed);
    RandomCase randomCase;
    DrawList& list = randomCase.list;
    list.iCols = PickRandomIn(rng, 1, 14);
    list.iRows = PickRandomIn(rng, 1, 7);
    list.iCellWidth = PickRandomIn(rng, 3, 12);
    list.iCellHeight = PickRandomIn(rng, 4, 16);
    list.iPaddingPx = PickRandomIn(rng, 0, 6);
    list.iScrollOffsetPx = rng() % 3 == 0 ? 0 : PickRandomIn(rng, 0, list.iCellHeight - 1);
    list.clearColor = TerminalRgb{uint8_t(rng()), uint8_t(rng()), uint8_t(rng())};
    const int iQuads = PickRandomIn(rng, 0, 120);
    const int iGridWidth = list.iCols * list.iCellWidth;
    for (int iQuad = 0; iQuad < iQuads; ++iQuad)
    {
        DrawQuad quad;
        const uint32_t uKind = rng() % 3;
        quad.eKind = uKind == 0 ? EQuadKind::RECT : uKind == 1 ? EQuadKind::GLYPH_MONO : EQuadKind::GLYPH_COLOR;
        quad.iRow = int16_t(PickRandomIn(rng, -2, list.iRows + 2));
        quad.iX = int16_t(PickRandomIn(rng, -15, iGridWidth + 10));
        quad.iY = int16_t(PickRandomIn(rng, -list.iCellHeight, 2 * list.iCellHeight));
        quad.uW = uint16_t(PickRandomIn(rng, 0, 3 * list.iCellWidth));
        quad.uH = uint16_t(PickRandomIn(rng, 0, 2 * list.iCellHeight + 2));
        if (quad.eKind != EQuadKind::RECT)
        {
            quad.uAtlasX = uint16_t(PickRandomIn(rng, 0, kAtlasSize + 6));
            quad.uAtlasY = uint16_t(PickRandomIn(rng, 0, kAtlasSize + 6));
        }
        quad.uColor = MakeRandomQuadColor(rng);
        list.vecQuads.push_back(quad);
    }
    list.bFullRedraw = rng() % 4 == 0;
    const int iDirty = PickRandomIn(rng, 0, list.iRows + 3);
    for (int iIndex = 0; iIndex < iDirty; ++iIndex)
        list.vecDirtyRows.push_back(PickRandomIn(rng, -2, list.iRows + 1));

    const QSize sizeNatural(iGridWidth + 2 * list.iPaddingPx, list.iRows * list.iCellHeight + 2 * list.iPaddingPx);
    const uint32_t uTarget = rng() % 8;
    if (uTarget == 0)
    {
        // 빈 이미지: Compose 가 목록 크기로 만든다
    }
    else if (uTarget == 1)
    {
        // 화면 크기가 다르다(남는 오른쪽 · 아래): 다시 만들고 전체 합성
        randomCase.imgStart = QImage(sizeNatural, QImage::Format_ARGB32_Premultiplied);
        FillGarbage(randomCase.imgStart, rng);
        randomCase.sizeTarget = sizeNatural + QSize(PickRandomIn(rng, 1, 9), PickRandomIn(rng, 1, 9));
    }
    else
    {
        // 같은 크기의 이미지 위에 부분 합성. 화면 크기를 목록과 다르게(남는 영역) 잡기도 한다
        const QSize size = rng() % 3 == 0 ? sizeNatural + QSize(PickRandomIn(rng, 0, 9), PickRandomIn(rng, 0, 9)) : sizeNatural;
        randomCase.imgStart = QImage(size, QImage::Format_ARGB32_Premultiplied);
        FillGarbage(randomCase.imgStart, rng);
        randomCase.sizeTarget = rng() % 2 ? size : QSize();
    }
    return randomCase;
}

bool HasSameBits(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.format() != b.format())
        return false;
    for (int iY = 0; iY < a.height(); ++iY)
    {
        if (std::memcmp(a.constScanLine(iY), b.constScanLine(iY), size_t(a.width()) * kBytesPerPixel) != 0)
            return false;
    }
    return true;
}

// 두 이미지의 첫 번째 다른 픽셀(없으면 빈 문자열)
QString DescribeFirstDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return QStringLiteral("size %1x%2 vs %3x%4").arg(a.width()).arg(a.height()).arg(b.width()).arg(b.height());
    for (int iY = 0; iY < a.height(); ++iY)
    {
        const uint32_t* pRowA = reinterpret_cast<const uint32_t*>(a.constScanLine(iY));
        const uint32_t* pRowB = reinterpret_cast<const uint32_t*>(b.constScanLine(iY));
        for (int iX = 0; iX < a.width(); ++iX)
        {
            if (pRowA[iX] != pRowB[iX])
                return QStringLiteral("(%1,%2): %3 vs %4").arg(iX).arg(iY).arg(pRowA[iX], 8, 16, QChar('0')).arg(pRowB[iX], 8, 16, QChar('0'));
        }
    }
    return QString();
}
}   // namespace

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

    // 무작위 목록을 여러 씨드로 만들어 최적화한 Compose 와 기준 합성이 비트 단위로 같은지, 다시 그린 영역도 같은지 본다.
    void matchesReferenceOnRandomLists()
    {
        for (uint32_t uSeed = 1; uSeed <= kRandomSeeds; ++uSeed)
        {
            std::mt19937 rngAtlas(uSeed * 7919u);
            GlyphAtlas mono(EAtlasFormat::A8, kAtlasSize, kAtlasSize);
            GlyphAtlas color(EAtlasFormat::RGBA8, kAtlasSize, kAtlasSize);
            QPoint pos;
            QVERIFY(mono.Insert(MakeRandomMonoAtlasImage(rngAtlas), pos));
            QVERIFY(color.Insert(MakeRandomColorAtlasImage(rngAtlas), pos));

            const RandomCase randomCase = MakeRandomCase(uSeed);
            QImage imgExpected = randomCase.imgStart.copy();
            QImage imgActual = randomCase.imgStart.copy();
            const QRegion regionExpected = Reference::Compose(randomCase.list, mono, color, imgExpected, randomCase.sizeTarget);
            const QRegion regionActual = SoftwareCompositor::Compose(randomCase.list, mono, color, imgActual, randomCase.sizeTarget);
            QVERIFY2(regionActual == regionExpected, qPrintable(QStringLiteral("seed %1: region differs").arg(uSeed)));
            QVERIFY2(HasSameBits(imgActual, imgExpected),
                     qPrintable(QStringLiteral("seed %1: %2").arg(uSeed).arg(DescribeFirstDifference(imgActual, imgExpected))));
        }
    }

    // 실제 그리기 목록에 가까운 크기(쿼드 수천 개, 여러 행이 바뀜)에서도 기준과 같다
    void matchesReferenceOnLargeList()
    {
        std::mt19937 rng(424242);
        GlyphAtlas mono(EAtlasFormat::A8, kAtlasSize, kAtlasSize);
        GlyphAtlas color(EAtlasFormat::RGBA8, kAtlasSize, kAtlasSize);
        QPoint pos;
        QVERIFY(mono.Insert(MakeRandomMonoAtlasImage(rng), pos));
        QVERIFY(color.Insert(MakeRandomColorAtlasImage(rng), pos));
        DrawList list = MakeList();
        list.iCols = 80;
        list.iRows = 24;
        list.iCellWidth = 8;
        list.iCellHeight = 17;
        list.iPaddingPx = 4;
        for (int iScroll : {0, 9})
        {
            list.iScrollOffsetPx = iScroll;
            list.vecQuads.clear();
            for (int iRow = -1; iRow <= list.iRows; ++iRow)
            {
                for (int iCol = 0; iCol < list.iCols; ++iCol)
                {
                    DrawQuad background = MakeQuad(iCol * 8, 0, 8, 17, MakeRandomQuadColor(rng), EQuadKind::RECT, iRow);
                    list.vecQuads.push_back(background);
                }
            }
            for (int iRow = -1; iRow <= list.iRows; ++iRow)
            {
                for (int iCol = 0; iCol < list.iCols; ++iCol)
                {
                    DrawQuad glyph = MakeQuad(iCol * 8 + PickRandomIn(rng, -2, 2), PickRandomIn(rng, -4, 4), PickRandomIn(rng, 4, 16),
                                              PickRandomIn(rng, 10, 24), MakeRandomQuadColor(rng),
                                              rng() % 5 == 0 ? EQuadKind::GLYPH_COLOR : EQuadKind::GLYPH_MONO, iRow);
                    glyph.uAtlasX = uint16_t(PickRandomIn(rng, 0, kAtlasSize - 8));
                    glyph.uAtlasY = uint16_t(PickRandomIn(rng, 0, kAtlasSize - 8));
                    list.vecQuads.push_back(glyph);
                }
            }
            for (int iRepeat = 0; iRepeat < 6; ++iRepeat)
            {
                list.bFullRedraw = iRepeat == 0;
                list.vecDirtyRows.clear();
                for (int iRow = -1; iRow <= list.iRows; ++iRow)
                {
                    if (rng() % 3 == 0)
                        list.vecDirtyRows.push_back(iRow);
                }
                QImage imgStart(648, 416, QImage::Format_ARGB32_Premultiplied);
                FillGarbage(imgStart, rng);
                QImage imgExpected = imgStart.copy();
                QImage imgActual = imgStart.copy();
                const QRegion regionExpected = Reference::Compose(list, mono, color, imgExpected);
                const QRegion regionActual = SoftwareCompositor::Compose(list, mono, color, imgActual);
                QCOMPARE(regionActual, regionExpected);
                QVERIFY2(HasSameBits(imgActual, imgExpected), qPrintable(DescribeFirstDifference(imgActual, imgExpected)));
            }
        }
    }

    // 부분 합성을 이어 붙여도 전체 합성과 같다:
    // (1) 전체 합성한 그림의 띠 영역을 망가뜨린 뒤 모든 행을 바뀐 행으로 부분 합성하면 전체 합성과 같다.
    // (2) 바뀐 행을 한 번에 합성한 것과 한 행씩 따로 합성해 이어 붙인 것이 같다(그리고 기준과 같다).
    void partialComposesStitchToFull()
    {
        for (uint32_t uSeed = 1; uSeed <= kRandomSeeds / 2; ++uSeed)
        {
            std::mt19937 rngAtlas(uSeed * 104729u);
            GlyphAtlas mono(EAtlasFormat::A8, kAtlasSize, kAtlasSize);
            GlyphAtlas color(EAtlasFormat::RGBA8, kAtlasSize, kAtlasSize);
            QPoint pos;
            QVERIFY(mono.Insert(MakeRandomMonoAtlasImage(rngAtlas), pos));
            QVERIFY(color.Insert(MakeRandomColorAtlasImage(rngAtlas), pos));
            const RandomCase randomCase = MakeRandomCase(uSeed + 100000u);
            DrawList list = randomCase.list;

            // (1)
            list.bFullRedraw = true;
            QImage imgFull;
            SoftwareCompositor::Compose(list, mono, color, imgFull);
            QImage imgStitched = imgFull.copy();
            std::mt19937 rng(uSeed);
            list.bFullRedraw = false;
            list.vecDirtyRows.clear();
            for (int iRow = list.iRows - 1; iRow >= 0; --iRow)   // 거꾸로 넣어도 같다
            {
                list.vecDirtyRows.push_back(iRow);
                const int iTop = list.iPaddingPx + iRow * list.iCellHeight - list.iScrollOffsetPx;
                const QRect band = QRect(0, iTop, imgStitched.width(), list.iCellHeight).intersected(imgStitched.rect());
                for (int iY = band.top(); iY <= band.bottom(); ++iY)
                {
                    uint32_t* pRow = reinterpret_cast<uint32_t*>(imgStitched.scanLine(iY));
                    for (int iX = band.left(); iX <= band.right(); ++iX)
                        pRow[iX] = rng();
                }
            }
            SoftwareCompositor::Compose(list, mono, color, imgStitched);
            QVERIFY2(HasSameBits(imgStitched, imgFull),
                     qPrintable(QStringLiteral("seed %1 (1): %2").arg(uSeed).arg(DescribeFirstDifference(imgStitched, imgFull))));

            // (2)
            QImage imgStart(imgFull.size(), QImage::Format_ARGB32_Premultiplied);
            FillGarbage(imgStart, rng);
            std::vector<int> vecRows;
            for (int iRow = -1; iRow <= list.iRows; ++iRow)
            {
                if (rng() % 2)
                    vecRows.push_back(iRow);
            }
            QImage imgOnce = imgStart.copy();
            list.vecDirtyRows = vecRows;
            const QRegion regionOnce = SoftwareCompositor::Compose(list, mono, color, imgOnce);
            QImage imgReference = imgStart.copy();
            const QRegion regionReference = Reference::Compose(list, mono, color, imgReference);
            QImage imgEach = imgStart.copy();
            QRegion regionEach;
            for (int iRow : vecRows)
            {
                list.vecDirtyRows = {iRow};
                regionEach += SoftwareCompositor::Compose(list, mono, color, imgEach);
            }
            QVERIFY2(regionOnce == regionEach && regionOnce == regionReference, qPrintable(QStringLiteral("seed %1 (2): region").arg(uSeed)));
            QVERIFY2(HasSameBits(imgOnce, imgEach),
                     qPrintable(QStringLiteral("seed %1 (2): %2").arg(uSeed).arg(DescribeFirstDifference(imgOnce, imgEach))));
            QVERIFY2(HasSameBits(imgOnce, imgReference),
                     qPrintable(QStringLiteral("seed %1 (2) ref: %2").arg(uSeed).arg(DescribeFirstDifference(imgOnce, imgReference))));
        }
    }

    // 0..255*255 의 모든 값에서 나눗셈 없는 식이 (t + 127) / 255 와 같다(전수 검사)
    void divideBy255MatchesRoundedDivisionForAllProducts()
    {
        int iMismatches = 0;
        uint32_t uFirst = 0;
        for (uint32_t uValue = 0; uValue <= 255u * 255u; ++uValue)
        {
            if (SoftwareCompositor::DivideBy255(uValue) != (uValue + 127) / 255)
            {
                if (iMismatches++ == 0)
                    uFirst = uValue;
            }
        }
        QVERIFY2(iMismatches == 0, qPrintable(QStringLiteral("%1 mismatches, first t=%2").arg(iMismatches).arg(uFirst)));
        // 채널 곱(색 × 커버리지, 대상 × (255 - 알파))의 모든 짝
        for (uint32_t uA = 0; uA <= 255; ++uA)
        {
            for (uint32_t uB = 0; uB <= 255; ++uB)
            {
                if (SoftwareCompositor::DivideBy255(uA * uB) != (uA * uB + 127) / 255)
                    QFAIL(qPrintable(QStringLiteral("a=%1 b=%2").arg(uA).arg(uB)));
            }
        }
    }
};

QObject* CreateSoftwareCompositorTest() { return new SoftwareCompositorTest(); }

#include "SoftwareCompositorTest.moc"
