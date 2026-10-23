#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"

#include <algorithm>

GlyphCache::GlyphCache(const QFont& font, qreal devicePixelRatio, int atlasInitialSize, int atlasMaxSize)
    : m_iAtlasInitialSize(atlasInitialSize)
    , m_iAtlasMaxSize(atlasMaxSize)
    , m_font(font)
    , m_metrics(CellMetrics::Compute(font, devicePixelRatio))
    , m_rasterizer(font, m_metrics)
    , m_monoAtlas(EAtlasFormat::A8, atlasInitialSize, atlasMaxSize)
    , m_colorAtlas(EAtlasFormat::RGBA8, atlasInitialSize, atlasMaxSize)
{
}

const CellMetrics& GlyphCache::GetMetrics() const
{
    return m_metrics;
}

qreal GlyphCache::GetDevicePixelRatio() const
{
    return m_metrics.dDevicePixelRatio;
}

GlyphAtlas& GlyphCache::GetAtlas(EAtlasPage page)
{
    return page == EAtlasPage::MONO ? m_monoAtlas : m_colorAtlas;
}

const GlyphAtlas& GlyphCache::GetAtlas(EAtlasPage page) const
{
    return page == EAtlasPage::MONO ? m_monoAtlas : m_colorAtlas;
}

uint32_t GlyphCache::GetGeneration() const
{
    return m_uiGeneration;
}

int GlyphCache::GetRasterizeCount() const
{
    return m_iRasterizeCount;
}

GlyphEntry GlyphCache::Store(const QImage& image, bool bColor, const QPoint& offset)
{
    GlyphEntry entry;
    if (image.isNull())
        return entry;

    const EAtlasPage page = bColor ? EAtlasPage::COLOR : EAtlasPage::MONO;
    const int iMaxSize = std::max(m_iAtlasInitialSize, m_iAtlasMaxSize);
    if (image.width() > iMaxSize || image.height() > iMaxSize)
        return entry;   // 어차피 못 넣는다. 캐시를 비우지 않는다.
    QPoint topLeft;
    if (!GetAtlas(page).Insert(image, topLeft))
    {
        m_monoAtlas.Reset();
        m_colorAtlas.Reset();
        m_mapGlyphs.clear();
        m_mapUnderlines.clear();
        ++m_uiGeneration;
        if (!GetAtlas(page).Insert(image, topLeft))
            return entry;   // 아틀라스 최대 크기보다 큰 이미지
    }
    entry.bEmpty = false;
    entry.ePage = page;
    entry.atlasRect = QRect(topLeft, image.size());
    entry.offset = offset;
    return entry;
}

GlyphEntry GlyphCache::GetGlyph(const std::u32string& text, bool bold, bool italic, int cellSpan)
{
    const bool bBox = cellSpan == 1 && text.size() == 1 && BoxGlyphPainter::Handles(text[0]);
    const GlyphKey tupKey(text, bBox ? false : bold, bBox ? false : italic, cellSpan);
    const auto itFound = m_mapGlyphs.find(tupKey);
    if (itFound != m_mapGlyphs.end())
        return itFound->second;

    ++m_iRasterizeCount;
    GlyphEntry entry;
    if (bBox)
    {
        entry = Store(BoxGlyphPainter::Paint(text[0], m_metrics), false, QPoint(0, 0));
    }
    else
    {
        const RasterGlyph raster = m_rasterizer.Rasterize(text, bold, italic, cellSpan);
        entry = Store(raster.image, raster.bColor, raster.offset);
    }
    // Store 가 캐시를 비웠어도 방금 넣은 항목은 새 세대의 것이므로 그대로 기록한다.
    m_mapGlyphs[tupKey] = entry;
    return entry;
}

GlyphEntry GlyphCache::GetUnderlineSprite(EUnderlineSprite sprite)
{
    const auto itFound = m_mapUnderlines.find(sprite);
    if (itFound != m_mapUnderlines.end())
        return itFound->second;

    const GlyphEntry entry = Store(BoxGlyphPainter::PaintUnderline(sprite, m_metrics), false,
                                   QPoint(0, BoxGlyphPainter::GetUnderlineSpriteTop(m_metrics)));
    m_mapUnderlines[sprite] = entry;
    return entry;
}
