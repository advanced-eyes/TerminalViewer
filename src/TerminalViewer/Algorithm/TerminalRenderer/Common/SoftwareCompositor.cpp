#include "TerminalViewer/Algorithm/TerminalRenderer/Common/SoftwareCompositor.h"

#include <QRect>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
constexpr uint32_t kChannelMax = 255;
constexpr uint32_t kRound = 127;
constexpr int kBitsPerByte = 8;

uint32_t Mul255(uint32_t a, uint32_t b)
{
    return (a * b + kRound) / kChannelMax;
}

// 0xAARRGGBB (ARGB32_Premultiplied) 로 묶는다.
uint32_t PackArgb(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (a << (3 * kBitsPerByte)) | (r << (2 * kBitsPerByte)) | (g << kBitsPerByte) | b;
}

uint32_t Blend(uint32_t dst, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb)
{
    const uint32_t uInverse = kChannelMax - sa;
    auto funcChannel = [&](uint32_t src, int shift) {
        const uint32_t uDst = (dst >> shift) & kChannelMax;
        return std::min(kChannelMax, src + (uDst * uInverse + kRound) / kChannelMax);
    };
    return PackArgb(funcChannel(sr, 2 * kBitsPerByte), funcChannel(sg, kBitsPerByte), funcChannel(sb, 0),
                    funcChannel(sa, 3 * kBitsPerByte));
}

void DrawQuadInBand(const DrawQuad& quad, const DrawList& list, const GlyphAtlas& mono, const GlyphAtlas& color,
                    const QRect& band, QImage& target)
{
    const int iLeft = list.iPaddingPx + quad.iX;
    const int iTop = list.iPaddingPx + quad.iRow * list.iCellHeight + quad.iY - list.iScrollOffsetPx;
    const QRect rect = QRect(iLeft, iTop, quad.uW, quad.uH).intersected(band);
    if (rect.isEmpty())
    {
        return;
    }

    const uint32_t uQuadR = quad.uColor & kChannelMax;
    const uint32_t uQuadG = (quad.uColor >> kBitsPerByte) & kChannelMax;
    const uint32_t uQuadB = (quad.uColor >> (2 * kBitsPerByte)) & kChannelMax;
    const uint32_t uQuadA = (quad.uColor >> (3 * kBitsPerByte)) & kChannelMax;

    const QImage* pAtlas = nullptr;
    if (quad.eKind == EQuadKind::GLYPH_MONO)
    {
        pAtlas = &mono.GetImage();
    }
    else if (quad.eKind == EQuadKind::GLYPH_COLOR)
    {
        pAtlas = &color.GetImage();
    }

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
                {
                    continue;
                }
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
                    const uchar* pPixel = pAtlas->constScanLine(iAtlasY) + iAtlasX * 4;   // R,G,B,A
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
        for (int iRow : vecRows)
        {
            const int iTop = list.iPaddingPx + iRow * list.iCellHeight - list.iScrollOffsetPx;
            const QRect band = QRect(0, iTop, all.width(), list.iCellHeight).intersected(all);
            if (!band.isEmpty())
            {
                vecBands.push_back(band);
            }
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
