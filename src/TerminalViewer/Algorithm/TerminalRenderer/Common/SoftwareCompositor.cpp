#include "TerminalViewer/Algorithm/TerminalRenderer/Common/SoftwareCompositor.h"

#include <QRect>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
constexpr uint32_t kChannelMax = 255;
constexpr int kBitsPerByte = 8;
constexpr int kColorBytesPerPixel = 4;   // 컬러 아틀라스: R,G,B,A

// SoftwareCompositor::DivideBy255 와 같은 식. 픽셀 루프에서 부르므로 인라인으로 둔다.
// t 가 0..255*255 이면 (t + 127) / 255 와 같다(SoftwareCompositorTest 전수 검사)
inline uint32_t DivideBy255Inline(uint32_t t)
{
    return ((t + 128) * 257) >> 16;
}

// 0xAARRGGBB (ARGB32_Premultiplied) 로 묶는다.
inline uint32_t PackArgb(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (a << (3 * kBitsPerByte)) | (r << (2 * kBitsPerByte)) | (g << kBitsPerByte) | b;
}

// premultiplied source-over. 채널마다 min(255, src + round(dst × (255 - sa) / 255)).
// (src 가 알파보다 큰 틀린 premultiplied 값도 기준 식 그대로 더하고 255 에서 자른다)
inline uint32_t Blend(uint32_t dst, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb)
{
    const uint32_t uInverse = kChannelMax - sa;
    const uint32_t uR = std::min(kChannelMax, sr + DivideBy255Inline(((dst >> (2 * kBitsPerByte)) & kChannelMax) * uInverse));
    const uint32_t uG = std::min(kChannelMax, sg + DivideBy255Inline(((dst >> kBitsPerByte) & kChannelMax) * uInverse));
    const uint32_t uB = std::min(kChannelMax, sb + DivideBy255Inline((dst & kChannelMax) * uInverse));
    const uint32_t uA = std::min(kChannelMax, sa + DivideBy255Inline(((dst >> (3 * kBitsPerByte)) & kChannelMax) * uInverse));
    return PackArgb(uR, uG, uB, uA);
}

// 이미지의 픽셀 메모리를 직접 본다(줄마다 QImage::scanLine 을 부르지 않는다)
struct PixelPlane
{
    const uchar* pBits = nullptr;
    int iStride = 0;   // 줄 간격(바이트)
    int iWidth = 0;
    int iHeight = 0;
};

// 합성 대상과 두 아틀라스의 픽셀 메모리
struct Canvas
{
    uchar* pBits = nullptr;   // 대상, ARGB32_Premultiplied
    int iStride = 0;          // 대상의 줄 간격(바이트)
    PixelPlane mono;          // Alpha8
    PixelPlane color;         // RGBA8888_Premultiplied

    uint32_t* GetRow(int y) const { return reinterpret_cast<uint32_t*>(pBits + qptrdiff(y) * iStride); }
};

PixelPlane MakePlane(const QImage& image)
{
    PixelPlane plane;
    plane.pBits = image.constBits();
    plane.iStride = image.bytesPerLine();
    plane.iWidth = image.width();
    plane.iHeight = image.height();
    return plane;
}

// 쿼드를 band 로 잘라 그린다. 빠른 길은 모두 기준 식(Blend)과 같은 값을 낸다:
// - 색이 0 인 사각형 · 단색 글리프, 커버리지 0, 네 바이트가 모두 0 인 컬러 픽셀: Blend(dst, 0, 0, 0, 0) == dst 라 건너뛴다.
//   (컬러 글리프는 쿼드 색을 쓰지 않으므로 색이 0 이어도 그린다. 알파만 0 이고 색이 있는 컬러 픽셀은 기준대로 더한다)
// - 알파 255 인 사각형 · (커버리지 255 이고 알파 255 인) 단색 글리프 · 알파 255 인 컬러 픽셀: Blend 가 src 를 그대로 내므로 쓴다.
// - 커버리지 255: round(색 × 255 / 255) == 색.
// 글리프는 아틀라스 밖으로 나가는 픽셀을 그리지 않는다: rect 를 아틀라스 범위와 교차해 픽셀마다 범위를 보지 않는다.
void DrawQuadInBand(const DrawQuad& quad, const DrawList& list, const Canvas& canvas, const QRect& band)
{
    const int iLeft = list.iPaddingPx + quad.iX;
    const int iTop = list.iPaddingPx + quad.iRow * list.iCellHeight + quad.iY - list.iScrollOffsetPx;
    QRect rect = QRect(iLeft, iTop, quad.uW, quad.uH).intersected(band);
    if (rect.isEmpty())
    {
        return;
    }

    const uint32_t uQuadR = quad.uColor & kChannelMax;
    const uint32_t uQuadG = (quad.uColor >> kBitsPerByte) & kChannelMax;
    const uint32_t uQuadB = (quad.uColor >> (2 * kBitsPerByte)) & kChannelMax;
    const uint32_t uQuadA = (quad.uColor >> (3 * kBitsPerByte)) & kChannelMax;

    // 사각형(모르는 종류도 기준처럼 사각형으로 그린다)
    if (quad.eKind != EQuadKind::GLYPH_MONO && quad.eKind != EQuadKind::GLYPH_COLOR)
    {
        if (quad.uColor == 0)
        {
            return;
        }
        const int iWidth = rect.width();
        if (uQuadA == kChannelMax)
        {
            const uint32_t uPacked = PackArgb(uQuadR, uQuadG, uQuadB, kChannelMax);
            for (int iY = rect.top(); iY <= rect.bottom(); ++iY)
            {
                uint32_t* pRow = canvas.GetRow(iY) + rect.left();
                std::fill(pRow, pRow + iWidth, uPacked);
            }
            return;
        }
        for (int iY = rect.top(); iY <= rect.bottom(); ++iY)
        {
            uint32_t* pRow = canvas.GetRow(iY) + rect.left();
            for (int iX = 0; iX < iWidth; ++iX)
            {
                pRow[iX] = Blend(pRow[iX], uQuadA, uQuadR, uQuadG, uQuadB);
            }
        }
        return;
    }

    // 글리프: 아틀라스 좌표 = uAtlas + (화면 좌표 - 쿼드 왼쪽 위)
    const PixelPlane& atlas = quad.eKind == EQuadKind::GLYPH_MONO ? canvas.mono : canvas.color;
    rect = rect.intersected(QRect(iLeft - int(quad.uAtlasX), iTop - int(quad.uAtlasY), atlas.iWidth, atlas.iHeight));
    if (rect.isEmpty())
    {
        return;
    }
    const int iAtlasX0 = int(quad.uAtlasX) + (rect.left() - iLeft);
    const int iAtlasY0 = int(quad.uAtlasY) + (rect.top() - iTop);
    const int iWidth = rect.width();

    if (quad.eKind == EQuadKind::GLYPH_MONO)
    {
        if (quad.uColor == 0)
        {
            return;
        }
        const bool bOpaque = uQuadA == kChannelMax;
        const uint32_t uPacked = PackArgb(uQuadR, uQuadG, uQuadB, uQuadA);
        const uchar* pCoverage = atlas.pBits + qptrdiff(iAtlasY0) * atlas.iStride + iAtlasX0;
        for (int iY = rect.top(); iY <= rect.bottom(); ++iY, pCoverage += atlas.iStride)
        {
            uint32_t* pRow = canvas.GetRow(iY) + rect.left();
            for (int iX = 0; iX < iWidth; ++iX)
            {
                const uint32_t uCoverage = pCoverage[iX];
                if (uCoverage == 0)
                {
                    continue;
                }
                if (uCoverage == kChannelMax)
                {
                    pRow[iX] = bOpaque ? uPacked : Blend(pRow[iX], uQuadA, uQuadR, uQuadG, uQuadB);
                    continue;
                }
                pRow[iX] = Blend(pRow[iX], DivideBy255Inline(uQuadA * uCoverage), DivideBy255Inline(uQuadR * uCoverage),
                                 DivideBy255Inline(uQuadG * uCoverage), DivideBy255Inline(uQuadB * uCoverage));
            }
        }
        return;
    }

    const uchar* pAtlasRow = atlas.pBits + qptrdiff(iAtlasY0) * atlas.iStride + iAtlasX0 * kColorBytesPerPixel;
    for (int iY = rect.top(); iY <= rect.bottom(); ++iY, pAtlasRow += atlas.iStride)
    {
        uint32_t* pRow = canvas.GetRow(iY) + rect.left();
        const uchar* pPixel = pAtlasRow;
        for (int iX = 0; iX < iWidth; ++iX, pPixel += kColorBytesPerPixel)
        {
            const uint32_t uR = pPixel[0];
            const uint32_t uG = pPixel[1];
            const uint32_t uB = pPixel[2];
            const uint32_t uA = pPixel[3];
            if ((uR | uG | uB | uA) == 0)
            {
                continue;
            }
            pRow[iX] = uA == kChannelMax ? PackArgb(uR, uG, uB, kChannelMax) : Blend(pRow[iX], uA, uR, uG, uB);
        }
    }
}
}

uint32_t SoftwareCompositor::DivideBy255(uint32_t t)
{
    return DivideBy255Inline(t);
}

QRegion SoftwareCompositor::Compose(const DrawList& list, const GlyphAtlas& mono, const GlyphAtlas& color,
                                    QImage& target, const QSize& targetSize)
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
    {
        return QRegion();
    }

    // 다시 그릴 띠(위에서 아래로, 서로 떨어져 있다). 이어진 바뀐 행은 띠 하나로 합친다: 픽셀마다 결과는 지운 색과
    // 그 픽셀을 덮는 쿼드를 목록 순서로 섞은 값이라 띠를 어떻게 나눠도 같다.
    const QRect all(QPoint(0, 0), target.size());
    std::vector<QRect> vecBands;
    if (bFull)
    {
        vecBands.push_back(all);
    }
    else
    {
        std::vector<int> vecRows = list.vecDirtyRows;
        std::sort(vecRows.begin(), vecRows.end());
        vecRows.erase(std::unique(vecRows.begin(), vecRows.end()), vecRows.end());
        for (size_t uIndex = 0; uIndex < vecRows.size();)
        {
            size_t uEnd = uIndex + 1;
            while (uEnd < vecRows.size() && vecRows[uEnd] == vecRows[uEnd - 1] + 1)
            {
                ++uEnd;
            }
            const int iTop = list.iPaddingPx + vecRows[uIndex] * list.iCellHeight - list.iScrollOffsetPx;
            const int iHeight = int(uEnd - uIndex) * list.iCellHeight;
            const QRect band = QRect(0, iTop, all.width(), iHeight).intersected(all);
            if (!band.isEmpty())
            {
                vecBands.push_back(band);
            }
            uIndex = uEnd;
        }
    }
    if (vecBands.empty())
    {
        return QRegion();
    }

    // bits() 는 띠가 있을 때 한 번만 부른다: 이미지를 함께 쓰는 조각(TerminalRendererCPU)이 남아 있으면 여기서 떼어 복사한다
    Canvas canvas;
    canvas.pBits = target.bits();
    canvas.iStride = target.bytesPerLine();
    canvas.mono = MakePlane(mono.GetImage());
    canvas.color = MakePlane(color.GetImage());

    const uint32_t uClear = PackArgb(list.clearColor.r, list.clearColor.g, list.clearColor.b, kChannelMax);
    QRegion region;
    for (const QRect& band : vecBands)
    {
        for (int iY = band.top(); iY <= band.bottom(); ++iY)
        {
            uint32_t* pRow = canvas.GetRow(iY);
            std::fill(pRow + band.left(), pRow + band.right() + 1, uClear);
        }
        region += band;
    }
    if (vecBands.size() == 1)
    {
        for (const DrawQuad& quad : list.vecQuads)
        {
            DrawQuadInBand(quad, list, canvas, vecBands.front());
        }
        return region;
    }

    // 띠가 여럿이면 쿼드를 세로로 겹치는 띠마다 한 번씩 나눠 담고(CSR, 목록 순서 그대로) 띠마다 담긴 쿼드만 그린다.
    // 띠는 위에서 아래로 겹치지 않으므로 쿼드와 겹치는 띠는 이어진 번호 [first, last] 다.
    const int iBandCount = int(vecBands.size());
    std::vector<int> vecFirstBandFromY(size_t(all.height()) + 1, iBandCount);   // y 에서 아래쪽으로 처음 만나는 띠(바닥이 y 이상)
    {
        int iBand = 0;
        for (int iY = 0; iY <= all.height(); ++iY)
        {
            while (iBand < iBandCount && vecBands[size_t(iBand)].bottom() < iY)
            {
                ++iBand;
            }
            vecFirstBandFromY[size_t(iY)] = iBand;
        }
    }
    std::vector<int> vecQuadFirstBand(list.vecQuads.size(), 0);
    std::vector<int> vecQuadLastBand(list.vecQuads.size(), -1);   // 겹치는 띠가 없으면 first > last
    // 띠 b 의 쿼드는 vecBandQuads[vecBandOffsets[b] .. vecBandOffsets[b + 1]). 먼저 띠마다 쿼드 수를 한 칸 뒤에 세고 누적한다
    std::vector<int> vecBandOffsets(size_t(iBandCount) + 1, 0);
    for (size_t uQuad = 0; uQuad < list.vecQuads.size(); ++uQuad)
    {
        const DrawQuad& quad = list.vecQuads[uQuad];
        if (quad.uW == 0 || quad.uH == 0)
        {
            continue;
        }
        const int iTop = list.iPaddingPx + quad.iRow * list.iCellHeight + quad.iY - list.iScrollOffsetPx;
        const int iBottom = iTop + int(quad.uH) - 1;
        if (iBottom < 0 || iTop >= all.height())
        {
            continue;
        }
        const int iFirst = vecFirstBandFromY[size_t(std::max(0, iTop))];
        int iLast = iFirst - 1;
        while (iLast + 1 < iBandCount && vecBands[size_t(iLast + 1)].top() <= iBottom)
        {
            ++iLast;
        }
        vecQuadFirstBand[uQuad] = iFirst;
        vecQuadLastBand[uQuad] = iLast;
        for (int iBand = iFirst; iBand <= iLast; ++iBand)
        {
            ++vecBandOffsets[size_t(iBand) + 1];
        }
    }
    for (int iBand = 0; iBand < iBandCount; ++iBand)
    {
        vecBandOffsets[size_t(iBand) + 1] += vecBandOffsets[size_t(iBand)];
    }
    // static_cast: size_t(…) 로 쓰면 함수 선언으로 읽힌다(most vexing parse)
    std::vector<uint32_t> vecBandQuads(static_cast<size_t>(vecBandOffsets[size_t(iBandCount)]));
    std::vector<int> vecBandNextSlot(vecBandOffsets.begin(), vecBandOffsets.end() - 1);
    for (size_t uQuad = 0; uQuad < list.vecQuads.size(); ++uQuad)
    {
        for (int iBand = vecQuadFirstBand[uQuad]; iBand <= vecQuadLastBand[uQuad]; ++iBand)
        {
            vecBandQuads[size_t(vecBandNextSlot[size_t(iBand)]++)] = uint32_t(uQuad);
        }
    }
    for (int iBand = 0; iBand < iBandCount; ++iBand)
    {
        const QRect& band = vecBands[size_t(iBand)];
        for (int iSlot = vecBandOffsets[size_t(iBand)]; iSlot < vecBandOffsets[size_t(iBand) + 1]; ++iSlot)
        {
            DrawQuadInBand(list.vecQuads[vecBandQuads[size_t(iSlot)]], list, canvas, band);
        }
    }
    return region;
}
